#!/usr/bin/env python3
"""CH9329 conformance / health check — run against a candidate serial-to-
USB-HID dongle *before* trusting it in production, or against one already
deployed that's under suspicion. Speaks the raw WCH CH9329 serial protocol
directly (frame format, checksum, command bytes all taken from WCH's own
"CH9329 芯片串口通信协议 V1.0" datasheet); stdlib only, no pyserial — same
dependency bar as tests/integration/server_harness.py. Independent of the
HoustonKVM server: this never touches ch9329_inject.cpp, so a clean run
here doesn't by itself prove the C++ backend is right, and a failure here
isn't caused by anything in this codebase.

Usage:
    python3 scripts/ch9329-selftest.py /dev/ttyUSB0
    python3 scripts/ch9329-selftest.py /dev/ttyUSB0 --baud 4800
    python3 scripts/ch9329-selftest.py /dev/ttyUSB0 --skip-button-test
    python3 scripts/ch9329-selftest.py /dev/ttyUSB0 --detect-baud
    python3 scripts/ch9329-selftest.py /dev/ttyUSB0 --set-baud 115200

Stop anything else using the device first (systemctl stop houstonkvm, or
disable the target): two programs writing one serial port interleave their
frames.

--detect-baud tries each standard rate until the chip answers. It only
sends GET_INFO, so it is safe to run at any time.

--set-baud changes the rate stored in the chip's own configuration. The
server's per-target baud setting only changes the host side, and the two
must match, so changing one without the other leaves a chip that never
answers. This reads the chip's 50-byte configuration, changes only the four
baud bytes, shows the change and asks before writing it, resets the chip if
the new rate isn't live yet, and then checks the chip answers reliably at
the new rate. `--set-baud 9600` puts it back. If a board comes back still
at the old rate, its configuration pins are forcing the default and a
software change can't take effect on it.

Checks performed, in order:
  1. GET_INFO responds at all, and within the protocol's own 500ms budget
     (section 2 of the datasheet: no response inside 500ms = failed
     exchange, by WCH's own definition).
  2. GET_PARA_CFG's persistent chip configuration is combined
     keyboard+mouse mode and protocol serial mode — a board with its
     MODE0/MODE1 or CFG0/CFG1 hardware pins wired to something else shows
     up here without needing to touch a screwdriver.
  3. Link reliability under a burst of back-to-back probes — a healthy
     wired link should be ~100% with tight response times; recurring
     failures here predict the "stopped responding to GET_INFO,
     reconnecting" churn a flaky link produces in production.
  4. A guided absolute-vs-relative mouse button test. This is the check
     that actually caught a real defective unit during development: some
     CH9329 boards/firmware advertise full mouse button support (the
     target's kernel shows BTN_LEFT/RIGHT/MIDDLE as valid capabilities in
     /proc/bus/input/devices) but only wire the button field into ONE of
     the chip's two mouse HID report types — relative (CMD 0x05) or
     absolute (CMD 0x04) — silently discarding it on the other, even
     though position data works fine on both. This sends a distinctive,
     countable click pattern through each report type in turn and asks
     the operator (watching the target machine) how many actually
     registered — turning a one-off manual investigation into a repeatable
     procedure. Needs a human at the target; there's no way to check this
     purely from the host side, since the whole point is verifying what
     the target's OS actually receives.

Exit code is 0 only if every automated check passes and (unless skipped)
the operator confirms both button report types work.
"""
import argparse
import fcntl
import os
import select
import struct
import sys
import termios
import time
import tty

HEAD = bytes([0x57, 0xAB])
ADDR = 0x00
CMD_GET_INFO = 0x01
CMD_SEND_MS_ABS_DATA = 0x04
CMD_SEND_MS_REL_DATA = 0x05
CMD_GET_PARA_CFG = 0x08
CMD_SET_PARA_CFG = 0x09
CMD_RESET = 0x0F
CMD_RESPONSE_BIT = 0x80

PARA_CFG_LEN = 50
# Try the factory default first, then fastest to slowest.
DETECT_ORDER = (9600, 115200, 57600, 38400, 19200, 4800, 2400, 1200)

PROBE_TIMEOUT_S = 0.5  # datasheet's own "no response in 500mS = failed" rule
RELIABILITY_PROBES = 20

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
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)  # equivalent to C's cfmakeraw()
    attrs = termios.tcgetattr(fd)
    speed = BAUD_MAP.get(baud)
    if speed is None:
        raise ValueError(f"unsupported baud rate {baud}")
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


def build_packet(cmd: int, data: bytes) -> bytes:
    body = HEAD + bytes([ADDR, cmd, len(data)]) + data
    checksum = sum(body) & 0xFF
    return body + bytes([checksum])


def read_response(fd: int, expected_cmd: int, timeout_s: float):
    """Returns (ok: bool, data: bytes, elapsed_s: float, error: str|None)."""
    buf = b""
    deadline = time.monotonic() + timeout_s
    start = time.monotonic()
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        r, _, _ = select.select([fd], [], [], max(remaining, 0))
        if not r:
            break
        try:
            chunk = os.read(fd, 64)
        except BlockingIOError:
            continue
        if not chunk:
            break
        buf += chunk
        if len(buf) >= 5 and buf[0] == 0x57 and buf[1] == 0xAB:
            length = buf[4]
            frame_len = 5 + length + 1
            if len(buf) < frame_len:
                continue
            frame = buf[:frame_len]
            checksum_ok = (sum(frame[:-1]) & 0xFF) == frame[-1]
            elapsed = time.monotonic() - start
            if not checksum_ok:
                return False, b"", elapsed, "checksum mismatch"
            if frame[3] != (expected_cmd | CMD_RESPONSE_BIT):
                return False, b"", elapsed, f"unexpected cmd 0x{frame[3]:02x}"
            return True, frame[5:5 + length], elapsed, None
    return False, b"", time.monotonic() - start, "timeout"


def send_and_wait(fd: int, cmd: int, data: bytes, timeout_s: float = PROBE_TIMEOUT_S):
    termios.tcflush(fd, termios.TCIFLUSH)
    os.write(fd, build_packet(cmd, data))
    return read_response(fd, cmd, timeout_s)


def check_get_info(fd: int, r: Result):
    print("\n[1/4] GET_INFO handshake")
    ok, data, elapsed, err = send_and_wait(fd, CMD_GET_INFO, b"")
    if not ok:
        r.fail(f"no valid response ({err}) — check wiring/baud rate before anything else")
        return
    if len(data) < 3:
        r.fail(f"response too short ({len(data)} bytes, expected >= 3)")
        return
    version, usb_state = data[0], data[1]
    r.ok(f"responded in {elapsed*1000:.1f}ms — chip fw v{version >> 4}.{version & 0xF}, "
         f"USB {'enumerated' if usb_state == 1 else 'NOT enumerated with a host'}")
    if usb_state != 1:
        r.warn("chip is not currently enumerated as a USB device on its target side")


def check_para_cfg(fd: int, r: Result):
    print("\n[2/4] Persistent chip configuration (GET_PARA_CFG)")
    ok, data, elapsed, err = send_and_wait(fd, CMD_GET_PARA_CFG, b"")
    if not ok:
        r.fail(f"no valid response ({err})")
        return
    if len(data) < 2:
        r.fail(f"response too short ({len(data)} bytes)")
        return
    work_mode, serial_mode = data[0], data[1]
    if len(data) >= 7:
        print(f"       stored baud rate: {cfg_baud(data)}")
    work_mode_names = {
        0x00: "sw: kbd+mouse combined", 0x80: "hw-pin: kbd+mouse combined",
        0x01: "sw: kbd only",          0x81: "hw-pin: kbd only",
        0x02: "sw: mouse only",        0x82: "hw-pin: mouse only",
        0x03: "sw: custom HID",        0x83: "hw-pin: custom HID",
    }
    serial_mode_names = {
        0x00: "sw: protocol", 0x80: "hw-pin: protocol",
        0x01: "sw: ASCII",    0x81: "hw-pin: ASCII",
        0x02: "sw: transparent", 0x82: "hw-pin: transparent",
    }
    wm_name = work_mode_names.get(work_mode, f"unknown (0x{work_mode:02x})")
    sm_name = serial_mode_names.get(serial_mode, f"unknown (0x{serial_mode:02x})")
    print(f"       work_mode=0x{work_mode:02x} ({wm_name})")
    print(f"       serial_mode=0x{serial_mode:02x} ({sm_name})")
    if work_mode not in (0x00, 0x80):
        r.fail("work mode is not combined keyboard+mouse — HoustonKVM needs both; "
               "check MODE0/MODE1 hardware pins or CMD_SET_PARA_CFG")
    else:
        r.ok("work mode is combined keyboard+mouse, as required")
    if serial_mode not in (0x00, 0x80):
        r.fail("serial mode is not protocol mode — HoustonKVM's framed commands "
               "won't parse; check CFG0/CFG1 hardware pins")
    else:
        r.ok("serial mode is protocol mode, as required")


def check_reliability(fd: int, r: Result, n: int):
    print(f"\n[3/4] Link reliability ({n} back-to-back GET_INFO probes)")
    failures = 0
    times = []
    for i in range(n):
        ok, _, elapsed, err = send_and_wait(fd, CMD_GET_INFO, b"")
        if ok:
            times.append(elapsed)
        else:
            failures += 1
    rate = (n - failures) / n * 100
    if times:
        avg_ms = sum(times) / len(times) * 1000
        max_ms = max(times) * 1000
        print(f"       {n - failures}/{n} succeeded ({rate:.0f}%), "
              f"avg {avg_ms:.1f}ms, max {max_ms:.1f}ms")
    else:
        print(f"       0/{n} succeeded")
    if failures == 0:
        r.ok("100% reliable over this burst")
    elif failures / n <= 0.05:
        r.warn(f"{failures}/{n} probes failed — marginal link, worth investigating "
               "wiring/power/ground before deploying")
    else:
        r.fail(f"{failures}/{n} probes failed — this link is unreliable; expect "
               "the same in production (reconnect churn, possible dropped commands)")


def cfg_baud(cfg: bytes) -> int:
    # Bytes 3-6 of the configuration, big-endian (datasheet section 2.1.9).
    return int.from_bytes(cfg[3:7], "big")


def answers_at(path: str, baud: int) -> bool:
    fd = open_serial(path, baud)
    try:
        return send_and_wait(fd, CMD_GET_INFO, b"")[0]
    finally:
        os.close(fd)


def detect_baud(path: str):
    for baud in DETECT_ORDER:
        if answers_at(path, baud):
            return baud
    return None


def reliable_at(path: str, baud: int, n: int = RELIABILITY_PROBES) -> int:
    fd = open_serial(path, baud)
    try:
        return sum(1 for _ in range(n) if send_and_wait(fd, CMD_GET_INFO, b"")[0])
    finally:
        os.close(fd)


def set_baud(path: str, new_baud: int, assume_yes: bool) -> bool:
    if new_baud not in BAUD_MAP:
        print(f"Unsupported rate {new_baud}; choose one of {sorted(BAUD_MAP)}", file=sys.stderr)
        return False

    print("Finding the rate the chip answers at now...")
    old_baud = detect_baud(path)
    if old_baud is None:
        print("  The chip didn't answer at any standard rate. Check it is plugged in and "
              "that nothing else has the port open.")
        return False
    print(f"  answering at {old_baud}")

    fd = open_serial(path, old_baud)
    try:
        ok, cfg, _, err = send_and_wait(fd, CMD_GET_PARA_CFG, b"")
        if not ok or len(cfg) != PARA_CFG_LEN:
            print(f"  Couldn't read its configuration ({err or f'{len(cfg)} bytes'}); nothing changed.")
            return False
        stored = cfg_baud(cfg)
        print(f"  stored baud rate: {stored}")
        if stored == new_baud and old_baud == new_baud:
            print(f"Already at {new_baud}; nothing to do.")
            return True

        new_cfg = cfg[:3] + new_baud.to_bytes(4, "big") + cfg[7:]
        print(f"\nAbout to write the chip's configuration, changing only the baud rate:")
        print(f"  bytes 3-6: {cfg[3:7].hex(' ')}  ->  {new_cfg[3:7].hex(' ')}   "
              f"({stored} -> {new_baud})")
        if not assume_yes and not prompt_yes_no("Write it?"):
            print("Cancelled; nothing changed.")
            return False

        ok, status, _, err = send_and_wait(fd, CMD_SET_PARA_CFG, new_cfg)
        if not ok or status[:1] != b"\x00":
            detail = err if err else f"status 0x{status[0]:02x}"
            print(f"  The chip rejected the new configuration ({detail}); it should be unchanged.")
            return False
        print("  written")
    finally:
        os.close(fd)

    # Some firmware switches rate at once, some only after a reset. The line
    # speed belongs to the device, not the file descriptor, so each probe
    # opens its own at the rate it needs.
    if not answers_at(path, new_baud):
        print("  not live yet; resetting the chip")
        fd = open_serial(path, old_baud)
        try:
            send_and_wait(fd, CMD_RESET, b"")
        finally:
            os.close(fd)
        time.sleep(1.0)

    now = detect_baud(path)
    if now != new_baud:
        where = f"at {now}" if now else "at no standard rate"
        print(f"\nThe chip now answers {where}, not {new_baud}.")
        if now == old_baud:
            print("  It kept its old rate: this board's configuration pins most likely force "
                  "the default, so the rate can't be changed in software. Unplug and replug "
                  "it once to be sure, then run --detect-baud.")
        else:
            print("  Unplug and replug it, then run --detect-baud to find it.")
        return False

    good = reliable_at(path, new_baud)
    print(f"\nThe chip answers at {new_baud}: {good}/{RELIABILITY_PROBES} probes succeeded.")
    if good < RELIABILITY_PROBES:
        print(f"  That link isn't reliable at this rate. Put it back with --set-baud {old_baud}.")
        return False
    print(f"Now set this target's serial baud rate to {new_baud} in HoustonKVM "
          f"(Admin -> Targets), then start the server again.")
    return True


def prompt_yes_no(question: str) -> bool:
    while True:
        ans = input(f"       {question} [y/n]: ").strip().lower()
        if ans in ("y", "yes"):
            return True
        if ans in ("n", "no"):
            return False


def check_buttons(fd: int, r: Result):
    print("\n[4/4] Absolute vs. relative mouse button report test")
    print("       Watch the TARGET machine's screen/pointer for this step.")
    print("       Each pattern is 3 distinct left-click presses, 300ms apart.")

    def click_pattern(via_relative: bool, count: int = 3, gap_s: float = 0.3):
        for _ in range(count):
            if via_relative:
                # buttons=0x01 (left), zero delta, zero wheel — see the
                # matching workaround/comment in ch9329_inject.cpp.
                send_and_wait(fd, CMD_SEND_MS_REL_DATA, bytes([0x01, 0x01, 0x00, 0x00, 0x00]),
                              timeout_s=0.05)
                time.sleep(0.05)
                send_and_wait(fd, CMD_SEND_MS_REL_DATA, bytes([0x01, 0x00, 0x00, 0x00, 0x00]),
                              timeout_s=0.05)
            else:
                # buttons=0x01 (left), centered position (0x0800,0x0800),
                # zero wheel.
                send_and_wait(fd, CMD_SEND_MS_ABS_DATA,
                              bytes([0x02, 0x01, 0x00, 0x08, 0x00, 0x08, 0x00]), timeout_s=0.05)
                time.sleep(0.05)
                send_and_wait(fd, CMD_SEND_MS_ABS_DATA,
                              bytes([0x02, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00]), timeout_s=0.05)
            time.sleep(gap_s)

    print("\n       Sending 3 clicks via the ABSOLUTE-mouse report (CMD 0x04)...")
    click_pattern(via_relative=False)
    abs_ok = prompt_yes_no("Did you see 3 left-clicks register on the target?")

    print("\n       Sending 3 clicks via the RELATIVE-mouse report (CMD 0x05)...")
    click_pattern(via_relative=True)
    rel_ok = prompt_yes_no("Did you see 3 left-clicks register on the target?")

    if abs_ok and rel_ok:
        r.ok("both absolute and relative button reports work — no workaround needed")
    elif rel_ok and not abs_ok:
        r.warn("absolute-report buttons are broken on this unit, but relative-report "
               "buttons work — this is the exact defect HoustonKVM's Ch9329Inject "
               "backend already works around (see sendRelativeButtonReport() in "
               "src/hid/ch9329_inject.cpp); this unit is usable as-is")
    elif abs_ok and not rel_ok:
        r.warn("relative-report buttons are broken but absolute-report buttons work "
               "— unusual (the opposite of the known defect); HoustonKVM only uses "
               "absolute movement, so this unit is likely fine as-is, but flag it")
    else:
        r.fail("NEITHER report type delivers a working button press to the target — "
               "this unit cannot do mouse clicks at all; do not deploy it, return/"
               "replace it")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("device", help="serial device path, e.g. /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=9600, help="baud rate (default: 9600)")
    parser.add_argument("--probes", type=int, default=RELIABILITY_PROBES,
                         help=f"number of reliability probes (default: {RELIABILITY_PROBES})")
    parser.add_argument("--skip-button-test", action="store_true",
                         help="skip the interactive button test (needs a human at the target)")
    parser.add_argument("--detect-baud", action="store_true",
                         help="find the rate the chip currently answers at, and exit")
    parser.add_argument("--set-baud", type=int, metavar="RATE",
                         help="change the rate stored in the chip, verify it, and exit")
    parser.add_argument("--yes", action="store_true",
                         help="with --set-baud: don't ask before writing")
    args = parser.parse_args()

    try:
        if args.detect_baud:
            baud = detect_baud(args.device)
            print(f"{args.device} answers at {baud}" if baud
                  else f"{args.device} didn't answer at any of {list(DETECT_ORDER)}")
            sys.exit(0 if baud else 1)
        if args.set_baud:
            sys.exit(0 if set_baud(args.device, args.set_baud, args.yes) else 1)
    except OSError as e:
        print(f"Cannot use {args.device}: {e}", file=sys.stderr)
        sys.exit(2)

    try:
        fd = open_serial(args.device, args.baud)
    except OSError as e:
        print(f"Cannot open {args.device}: {e}", file=sys.stderr)
        sys.exit(2)

    print(f"CH9329 conformance check — {args.device} @ {args.baud} baud")
    r = Result()
    try:
        check_get_info(fd, r)
        check_para_cfg(fd, r)
        check_reliability(fd, r, args.probes)
        if args.skip_button_test:
            print("\n[4/4] Skipped (--skip-button-test)")
        else:
            check_buttons(fd, r)
    finally:
        os.close(fd)

    print("\n" + ("FAIL — see above" if r.failed else "PASS — all checks OK"))
    sys.exit(1 if r.failed else 0)


if __name__ == "__main__":
    main()
