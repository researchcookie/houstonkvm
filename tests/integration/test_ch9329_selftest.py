"""scripts/ch9329-selftest.py's --detect-baud and --set-baud, against a
simulated chip on a pty.

The simulator honours line speed the way a real UART does: bytes sent at a
rate other than the one the chip is running at are garbage to it, so it
ignores them. A pty carries no real signal, but its termios settings are
shared between the two ends, so the simulator can read the rate the script
set and treat a mismatch as noise.
"""
import os
import select
import subprocess
import sys
import termios
import threading
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "ch9329-selftest.py"

SPEEDS = {
    1200: termios.B1200, 2400: termios.B2400, 4800: termios.B4800,
    9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400,
    57600: termios.B57600, 115200: termios.B115200,
}


def factory_config() -> bytearray:
    # Laid out like the real board's: hardware-pin modes, 9600 baud, then
    # the rest of the 50 bytes the script must leave untouched.
    cfg = bytearray(50)
    cfg[0:7] = bytes([0x80, 0x80, 0x00, 0x00, 0x00, 0x25, 0x80])
    cfg[7:15] = bytes([0x08, 0x00, 0x00, 0x03, 0x86, 0x1A, 0x29, 0xE1])
    cfg[18], cfg[20] = 0x01, 0x0D
    return cfg


class SimChip:
    """applies: "now" (new rate live at once), "reset" (after CMD_RESET),
    or "never" (configuration pins force the default)."""

    def __init__(self, applies="now", live=9600):
        self.applies = applies
        self.live = live
        self.config = factory_config()
        self.resets = 0
        self.master, self.slave = os.openpty()  # slave stays open: else master reads EIO
        self.path = os.ttyname(self.slave)
        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def close(self):
        self._stop = True
        self._thread.join(timeout=2)
        os.close(self.master)
        os.close(self.slave)

    def _reply(self, cmd, data=b""):
        body = bytes([0x57, 0xAB, 0x00, cmd | 0x80, len(data)]) + data
        os.write(self.master, body + bytes([sum(body) & 0xFF]))

    def _handle(self, cmd, data):
        if cmd == 0x01:                                   # GET_INFO
            self._reply(cmd, bytes([0x30, 0x01, 0x00]))
        elif cmd == 0x08:                                 # GET_PARA_CFG
            self._reply(cmd, bytes(self.config))
        elif cmd == 0x09:                                 # SET_PARA_CFG
            if len(data) != 50:
                self._reply(cmd, b"\xE5")
                return
            self.config = bytearray(data)
            self._reply(cmd, b"\x00")
            if self.applies == "now":
                self.live = int.from_bytes(data[3:7], "big")
        elif cmd == 0x0F:                                 # RESET
            self.resets += 1
            self._reply(cmd, b"\x00")
            if self.applies == "reset":
                self.live = int.from_bytes(self.config[3:7], "big")

    def _run(self):
        buf = b""
        while not self._stop:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if not r:
                continue
            try:
                chunk = os.read(self.master, 256)
            except OSError:
                continue
            if termios.tcgetattr(self.master)[4] != SPEEDS[self.live]:
                buf = b""        # wrong rate: noise to the chip
                continue
            buf += chunk
            while len(buf) >= 6:
                if buf[:2] != b"\x57\xAB":
                    buf = buf[1:]
                    continue
                end = 5 + buf[4] + 1
                if len(buf) < end:
                    break
                frame, buf = buf[:end], buf[end:]
                if sum(frame[:-1]) & 0xFF == frame[-1]:
                    self._handle(frame[3], frame[5:-1])


def run(*args, stdin=""):
    return subprocess.run([sys.executable, str(SCRIPT), *args], input=stdin,
                          capture_output=True, text=True, timeout=120)


class TestBaudTools(unittest.TestCase):
    def chip(self, **kw):
        chip = SimChip(**kw)
        self.addCleanup(chip.close)
        return chip

    def test_detect_finds_the_live_rate(self):
        for live in (9600, 57600):
            with self.subTest(live=live):
                chip = self.chip(live=live)
                out = run(chip.path, "--detect-baud")
                self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
                self.assertIn(f"answers at {live}", out.stdout)

    def test_set_baud_changes_only_the_baud_bytes(self):
        chip = self.chip(applies="now")
        before = bytes(chip.config)
        out = run(chip.path, "--set-baud", "115200", "--yes")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertEqual(chip.live, 115200)
        self.assertEqual(bytes(chip.config[3:7]), bytes([0x00, 0x01, 0xC2, 0x00]))
        self.assertEqual(chip.config[:3] + chip.config[7:], before[:3] + before[7:])
        self.assertEqual(chip.resets, 0)                 # live at once: no reset needed
        self.assertIn("20/20 probes succeeded", out.stdout)
        self.assertIn("Admin -> Targets", out.stdout)

    def test_resets_the_chip_when_the_new_rate_is_not_live_yet(self):
        chip = self.chip(applies="reset")
        out = run(chip.path, "--set-baud", "115200", "--yes")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertEqual(chip.resets, 1)
        self.assertEqual(chip.live, 115200)

    def test_says_so_when_pins_force_the_default(self):
        chip = self.chip(applies="never")
        out = run(chip.path, "--set-baud", "115200", "--yes")
        self.assertEqual(out.returncode, 1, out.stdout)
        self.assertEqual(chip.live, 9600)
        self.assertIn("configuration pins", out.stdout)

    def test_declining_writes_nothing(self):
        chip = self.chip()
        before = bytes(chip.config)
        out = run(chip.path, "--set-baud", "115200", stdin="n\n")
        self.assertEqual(out.returncode, 1, out.stdout)
        self.assertEqual(bytes(chip.config), before)
        self.assertIn("Cancelled; nothing changed.", out.stdout)

    def test_and_back_again(self):
        chip = self.chip(live=115200)
        chip.config[3:7] = (115200).to_bytes(4, "big")
        out = run(chip.path, "--set-baud", "9600", "--yes")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertEqual(chip.live, 9600)

    def test_no_chip_at_all(self):
        chip = self.chip()
        SPEEDS[300] = termios.B300
        self.addCleanup(SPEEDS.pop, 300)
        chip.live = 300                                   # answers at no standard rate
        out = run(chip.path, "--detect-baud")
        self.assertEqual(out.returncode, 1)
        self.assertIn("didn't answer", out.stdout)


if __name__ == "__main__":
    unittest.main()
