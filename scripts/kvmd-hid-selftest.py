#!/usr/bin/env python3
"""Checks a board running kvmd's open-source HID firmware — a Raspberry Pi
Pico, an Arduino Micro or a Pro Micro, wired to a USB-serial adapter — on
its own, before you wire it to a HoustonKVM target, or when one is under
suspicion. Speaks the firmware's serial protocol directly (as defined in
hid/pico and hid/arduino of the kvmd repository); stdlib only, no pyserial —
same dependency bar as tests/integration/server_harness.py. Independent of the
HoustonKVM server: this never touches kvmd_hid_inject.cpp, so a clean run here
doesn't by itself prove the C++ backend is right, and a failure here isn't
caused by anything in this codebase.

Usage:
    python3 scripts/kvmd-hid-selftest.py /dev/ttyUSB0
    python3 scripts/kvmd-hid-selftest.py /dev/ttyUSB0 --baud 57600
    python3 scripts/kvmd-hid-selftest.py /dev/ttyUSB0 --skip-target-tests
    python3 scripts/kvmd-hid-selftest.py /dev/ttyUSB0 --detect-baud

Stop anything else using the device first (systemctl stop houstonkvm, or
disable the target): two programs on one serial port garble each other's
requests.

--detect-baud tries each standard rate until the board answers. It only
sends status requests, so it is safe to run at any time. The firmware's
rate is fixed when it is built: 115200 unless someone changed it.

Checks performed, in order:
  1. The board answers at all, within 500 ms. If it doesn't: the adapter's
     TX goes to the board's RX and RX to TX, the grounds are joined, the
     adapter is set to 3.3 V for a Pico, the target machine is on (it powers
     the board), and on a Pico GP22 is wired to ground — without that the
     firmware listens on SPI, not on its UART.
  2. What the board says about itself: a USB keyboard and an absolute USB
     mouse, which is what HoustonKVM drives (it places the pointer
     absolutely), and whether the target has recognised each of them.
  3. Link reliability under a burst of back-to-back requests, with reply
     times. A healthy link answers every one, in a few milliseconds.
  4. A keyboard round trip, with nothing typed: presses Caps Lock twice and
     watches for the target to switch its Caps Lock light, which the board
     reports back. A change proves keys reach the target's operating
     system and the target talks back. No change is only a warning: the
     target may be at a screen that doesn't drive the light, and macOS
     ignores a short Caps Lock press.
  5. A guided pointer test: the pointer visits the four corners of the
     target's screen, then stops in the middle, and you say whether it did.
     Needs someone watching the target.

4 and 5 press a key and move the pointer on the target; --skip-target-tests
leaves them out. Exit code is 0 only if every automated check passes and
(unless skipped) you saw the pointer follow.
"""
import argparse
import os
import select
import sys
import termios
import time
import tty

REQUEST_MAGIC = 0x33
REPLY_MAGIC = 0x34
CMD_PING = 0x01
CMD_CLEAR = 0x10
CMD_KEY = 0x11
CMD_MOUSE_ABS = 0x12
KEY_CAPS_LOCK = 53          # the firmware's own number for it, not its USB usage

PONG_DONE = 0x80
PONG_CAPS = 0x01
PONG_KBD_OFFLINE = 0x08
PONG_MOUSE_OFFLINE = 0x10

ERRORS = {
    0x24: "the board has just restarted and has no previous reply",
    0x40: "the board received a garbled request (CRC error)",
    0x45: "the board doesn't know that command",
    0x48: "the board timed out waiting for the rest of a request",
}
KEYBOARD_MODES = {0x00: "off", 0x01: "USB", 0x03: "PS/2"}
MOUSE_MODES = {0x00: "off", 0x08: "absolute USB", 0x10: "relative USB", 0x18: "PS/2",
               0x20: "absolute USB (Windows 98)"}
ABSOLUTE_MICE = (0x08, 0x20)

# The firmware's usual rate first, then fastest to slowest.
DETECT_ORDER = (115200, 57600, 38400, 19200, 9600, 4800, 2400, 1200)
PROBE_TIMEOUT_S = 0.5
RELIABILITY_PROBES = 50
# The firmware drops a partial request after 100 ms without a byte.
RESYNC_PAUSE_S = 0.12

BAUD_MAP = {
    1200: termios.B1200, 2400: termios.B2400, 4800: termios.B4800,
    9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400,
    57600: termios.B57600, 115200: termios.B115200,
}


class Result:
    def __init__(self):
        self.failed = False

    def ok(self, msg):
        print(f"  [OK]   {msg}")

    def warn(self, msg):
        print(f"  [WARN] {msg}")

    def fail(self, msg):
        print(f"  [FAIL] {msg}")
        self.failed = True


def open_serial(path: str, baud: int) -> int:
    speed = BAUD_MAP.get(baud)
    if speed is None:
        raise ValueError(f"unsupported baud rate {baud}")
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)  # equivalent to C's cfmakeraw()
    attrs = termios.tcgetattr(fd)
    attrs[4] = speed  # ispeed
    attrs[5] = speed  # ospeed
    attrs[2] |= termios.CLOCAL | termios.CREAD  # cflag
    attrs[2] &= ~termios.PARENB
    attrs[2] &= ~termios.CSTOPB
    attrs[2] &= ~termios.CSIZE
    attrs[2] |= termios.CS8
    if hasattr(termios, "CRTSCTS"):
        attrs[2] &= ~termios.CRTSCTS
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def crc16(data: bytes) -> int:
    """CRC-16/MODBUS: reflected polynomial 0xA001, starting from 0xFFFF."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def request(cmd: int, *args: int) -> bytes:
    """0x33, the command, four argument bytes, CRC (high byte first)."""
    body = bytes([REQUEST_MAGIC, cmd, *args, *([0] * (4 - len(args)))])
    crc = crc16(body)
    return body + bytes([crc >> 8, crc & 0xFF])


def exchange(fd: int, req: bytes, timeout_s: float = PROBE_TIMEOUT_S):
    """Sends one request; returns (reply bytes 1-3 or None, elapsed_s, error)."""
    termios.tcflush(fd, termios.TCIFLUSH)
    start = time.monotonic()
    os.write(fd, req)
    buf = b""
    deadline = start + timeout_s
    while True:
        while True:
            i = buf.find(bytes([REPLY_MAGIC]))
            if i < 0:
                buf = b""
                break
            buf = buf[i:]
            if len(buf) < 8:
                break
            if crc16(buf[:6]) == (buf[6] << 8 | buf[7]):
                elapsed = time.monotonic() - start
                if not buf[1] & PONG_DONE:
                    return None, elapsed, ERRORS.get(buf[1], f"error code 0x{buf[1]:02x}")
                return buf[1:4], elapsed, None
            buf = buf[1:]  # not a reply after all; resync
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None, time.monotonic() - start, "no reply"
        r, _, _ = select.select([fd], [], [], remaining)
        if not r:
            continue
        try:
            buf += os.read(fd, 64)
        except BlockingIOError:
            continue


def resync(fd: int):
    """Waits for the board to drop any partial request, so the next one lines up."""
    time.sleep(RESYNC_PAUSE_S)
    termios.tcflush(fd, termios.TCIFLUSH)


def ping(fd: int, timeout_s: float = PROBE_TIMEOUT_S):
    reply, elapsed, err = exchange(fd, request(CMD_PING), timeout_s)
    if reply is None:
        resync(fd)
    return reply, elapsed, err


def check_answers(fd: int, r: Result):
    print("\n[1/5] Does the board answer?")
    reply, elapsed, err = ping(fd)
    if reply is None:
        r.fail(f"{err} — check the wiring (adapter TX to board RX, RX to TX, grounds joined), "
               "that the target machine is on, and on a Pico that GP22 is wired to ground; "
               "try --detect-baud if the firmware may run at another rate")
        return None
    r.ok(f"answered in {elapsed * 1000:.1f} ms")
    return reply


def check_modes(reply: bytes, r: Result):
    print("\n[2/5] What the board says about itself")
    code, outputs = reply[0], reply[1]
    if not outputs:
        r.warn("the board doesn't report its keyboard and mouse modes (an old firmware?)")
    else:
        kbd = outputs & 0x07
        mouse = outputs & 0x38
        kbd_name = KEYBOARD_MODES.get(kbd, f"unknown (0x{kbd:02x})")
        mouse_name = MOUSE_MODES.get(mouse, f"unknown (0x{mouse:02x})")
        (r.ok if kbd in (0x01, 0x03) else r.fail)(f"keyboard: {kbd_name}")
        if mouse in ABSOLUTE_MICE:
            r.ok(f"mouse: {mouse_name}")
        else:
            r.fail(f"mouse: {mouse_name} — HoustonKVM places the pointer absolutely, so it needs "
                   "an absolute USB mouse (on a Pico, nothing wired to GP2 to GP9), then "
                   "unplug the board and plug it back in")
    for flag, what in ((PONG_KBD_OFFLINE, "keyboard"), (PONG_MOUSE_OFFLINE, "mouse")):
        if code & flag:
            r.fail(f"the target machine hasn't recognised the {what} — check the USB cable from "
                   "the board to the target, and that the target is on")
        else:
            r.ok(f"the target machine has recognised the {what}")


def check_reliability(fd: int, r: Result, n: int):
    print(f"\n[3/5] Link reliability ({n} requests back to back)")
    times, errors = [], {}
    for _ in range(n):
        reply, elapsed, err = ping(fd)
        if reply is None:
            errors[err] = errors.get(err, 0) + 1
        else:
            times.append(elapsed * 1000)
    if times:
        print(f"       reply time: {sum(times) / len(times):.1f} ms average, {max(times):.1f} ms worst")
    if not errors:
        r.ok(f"{n}/{n} answered")
    else:
        detail = ", ".join(f"{count} × {err}" for err, count in errors.items())
        (r.warn if len(times) >= n * 0.95 else r.fail)(
            f"{len(times)}/{n} answered ({detail}) — a loose wire or a marginal adapter")


def caps_light(fd: int):
    reply, _, _ = ping(fd)
    return None if reply is None else bool(reply[0] & PONG_CAPS)


def press_caps_lock(fd: int):
    # Held a little: macOS ignores a very short Caps Lock press.
    exchange(fd, request(CMD_KEY, KEY_CAPS_LOCK, 1))
    time.sleep(0.3)
    exchange(fd, request(CMD_KEY, KEY_CAPS_LOCK, 0))


def check_keyboard(fd: int, r: Result):
    print("\n[4/5] Keyboard round trip (Caps Lock, twice; nothing is typed)")
    before = caps_light(fd)
    if before is None:
        r.fail("the board stopped answering")
        return
    press_caps_lock(fd)
    changed = False
    deadline = time.monotonic() + 1.5
    while time.monotonic() < deadline:
        if caps_light(fd) not in (before, None):
            changed = True
            break
        time.sleep(0.05)
    press_caps_lock(fd)  # and back as it was
    if changed:
        r.ok("the target switched its Caps Lock light: keys reach it, and it answers")
    else:
        r.warn("the Caps Lock light didn't change, so the round trip is unconfirmed — fine if the "
               "target is at a screen that doesn't drive the light; otherwise check the USB cable "
               "to the target")
    exchange(fd, request(CMD_CLEAR))


def prompt_yes_no(question: str) -> bool:
    while True:
        ans = input(f"       {question} [y/n]: ").strip().lower()
        if ans in ("y", "yes"):
            return True
        if ans in ("n", "no"):
            return False


def move(fd: int, x: int, y: int):
    x &= 0xFFFF
    y &= 0xFFFF
    exchange(fd, request(CMD_MOUSE_ABS, x >> 8, x & 0xFF, y >> 8, y & 0xFF))


def check_pointer(fd: int, r: Result):
    print("\n[5/5] Pointer test — watch the TARGET machine's screen")
    print("       The pointer will visit the four corners, then stop in the middle.")
    time.sleep(1.0)
    for x, y in ((-32768, -32768), (32767, -32768), (32767, 32767), (-32768, 32767), (0, 0)):
        move(fd, x, y)
        time.sleep(0.7)
    if prompt_yes_no("Did the pointer visit all four corners and stop in the middle?"):
        r.ok("the pointer follows absolute positions")
    else:
        r.fail("the pointer didn't follow — check the mouse mode in [2/5]; a target that "
               "doesn't take absolute USB mice (some very old BIOSes) can't be driven")


def detect_baud(path: str):
    for baud in DETECT_ORDER:
        fd = open_serial(path, baud)
        try:
            resync(fd)
            reply, _, _ = exchange(fd, request(CMD_PING), 0.3)
            if reply is not None:
                return baud
        finally:
            os.close(fd)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("device", help="serial device path, e.g. /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200, help="baud rate (default: 115200)")
    parser.add_argument("--probes", type=int, default=RELIABILITY_PROBES,
                        help=f"number of reliability requests (default: {RELIABILITY_PROBES})")
    parser.add_argument("--skip-target-tests", action="store_true",
                        help="don't press Caps Lock or move the pointer on the target")
    parser.add_argument("--detect-baud", action="store_true",
                        help="find the rate the board answers at, and exit")
    args = parser.parse_args()

    try:
        if args.detect_baud:
            baud = detect_baud(args.device)
            print(f"{args.device} answers at {baud}" if baud
                  else f"{args.device} didn't answer at any of {list(DETECT_ORDER)}")
            sys.exit(0 if baud else 1)
        fd = open_serial(args.device, args.baud)
    except (OSError, ValueError) as e:
        print(f"Cannot use {args.device}: {e}", file=sys.stderr)
        sys.exit(2)

    print(f"HoustonKVM board check (kvmd's HID firmware) — {args.device} @ {args.baud} baud")
    r = Result()
    try:
        reply = check_answers(fd, r)
        if reply is not None:
            check_modes(reply, r)
            check_reliability(fd, r, args.probes)
            if args.skip_target_tests:
                print("\n[4/5] [5/5] Skipped (--skip-target-tests)")
            else:
                check_keyboard(fd, r)
                check_pointer(fd, r)
    finally:
        os.close(fd)

    print("\n" + ("FAIL — see above" if r.failed else "PASS — all checks OK"))
    sys.exit(1 if r.failed else 0)


if __name__ == "__main__":
    main()
