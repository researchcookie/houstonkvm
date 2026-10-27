"""Keyboard and mouse through a board running kvmd's open-source HID firmware
(serial_adapter "kvmd-hid"): the bytes it receives, that it is driven the way
the firmware needs, its health, and the serial_adapter setting itself.

Each target is wired to a pty standing in for the USB-serial adapter, with a
FakeBoard on the master side behaving like the firmware (hid/pico and
hid/arduino in the kvmd repository): requests are bare 8-byte frames,
0x33 CMD A0 A1 A2 A3 CRC CRC (CRC-16/MODBUS of the first six, high byte
first), told apart only by counting, with a partial frame dropped after
100 ms without a byte. Every frame is answered 0x34 CODE OUT1 OUT2 0 0 CRC
CRC, CODE having bit 7 set when it was carried out. Keys go by the firmware's
own numbers, not USB usage IDs.
"""
import os
import select
import sys
import termios
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_targets import TargetTestBase

CMD_PING, CMD_CLEAR, CMD_KEY, CMD_ABS, CMD_BUTTON, CMD_WHEEL = 0x01, 0x10, 0x11, 0x12, 0x13, 0x14

KEY_A, KEY_B, KEY_C = 1, 2, 3
SHIFT_LEFT = 78
CAPS_LOCK = 53


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def s16(hi, lo):
    v = (hi << 8) | lo
    return v - 0x10000 if v & 0x8000 else v


class FakeBoard:
    """A pty pair posing as a board with kvmd's HID firmware behind a
    USB-serial adapter.

    By default it answers like a healthy board the target has recognised,
    with a USB keyboard and an absolute USB mouse. `enumerated` False is a
    board the target hasn't recognised, `mouse` sets the mouse mode bits
    (0x08 absolute, 0x10 relative, 0x18 PS/2, 0 off), `silent` answers
    nothing, `baud` makes it deaf at any other line speed, and `dynamic`
    False is a build with fixed modes, which reports them without the
    "switchable" flag. Its target
    switches the Caps Lock light on each Caps Lock press, as an operating
    system does, unless `leds` is False. `lose_byte()`
    drops the first byte of the next key frame on the wire, and
    `garble_key_reply()` corrupts the reply to the next one. Both aim at key
    frames so that a health check landing in between can't take the hit.
    """

    SPEEDS = {9600: termios.B9600, 57600: termios.B57600, 115200: termios.B115200}

    def __init__(self, enumerated=True, mouse=0x08, silent=False, baud=115200, leds=True,
                 dynamic=True):
        self.enumerated, self.mouse, self.silent, self.baud = enumerated, mouse, silent, baud
        self.dynamic = dynamic
        self.leds, self.caps = leds, False
        self._garble = False
        self._lose = False
        self.keys = set()
        self.buttons = {"left": False, "middle": False, "right": False}
        self.pos = None
        self.log = []   # (cmd, args) of every well-formed frame, in order
        self.master, self.slave = os.openpty()  # slave stays open: else master reads EIO
        self.path = os.ttyname(self.slave)
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def close(self):
        self._stop.set()
        self._thread.join(timeout=2)
        os.close(self.master)
        os.close(self.slave)

    def lose_byte(self):
        self._lose = True

    def garble_key_reply(self):
        self._garble = True

    def commands(self, cmd=None):
        with self._lock:
            return [(c, a) for c, a in self.log if cmd is None or c == cmd]

    def _reply(self, code, garble=False):
        if code & 0x80:
            if not self.enumerated:
                code |= 0x08 | 0x10   # keyboard and mouse offline
            if self.caps:
                code |= 0x01
            body = bytes([0x34, code, (0x80 if self.dynamic else 0) | 0x01 | self.mouse, 0x01, 0, 0])
        else:
            body = bytes([0x34, code, 0, 0, 0, 0])
        crc = crc16(body) ^ (0xFFFF if garble else 0)
        os.write(self.master, body + bytes([crc >> 8, crc & 0xFF]))

    def _handle(self, frame):
        if frame[0] != 0x33 or crc16(frame[:6]) != (frame[6] << 8 | frame[7]):
            return 0x40   # CRC error
        cmd, a = frame[1], frame[2:6]
        with self._lock:
            self.log.append((cmd, bytes(a)))
            if cmd == CMD_PING:
                pass
            elif cmd == CMD_CLEAR:
                self.keys.clear()
                self.buttons = dict.fromkeys(self.buttons, False)
            elif cmd == CMD_KEY:
                (self.keys.add if a[1] else self.keys.discard)(a[0])
                if a[0] == CAPS_LOCK and a[1] and self.leds:
                    self.caps = not self.caps
            elif cmd == CMD_ABS:
                self.pos = (s16(a[0], a[1]), s16(a[2], a[3]))
            elif cmd == CMD_BUTTON:
                for name, select_bit, state_bit in (("left", 0x80, 0x08), ("right", 0x40, 0x04),
                                                    ("middle", 0x20, 0x02)):
                    if a[0] & select_bit:
                        self.buttons[name] = bool(a[0] & state_bit)
            elif cmd != CMD_WHEEL:
                return 0x45   # unknown command
        return 0x80

    def _run(self):
        buf, last = b"", 0.0
        while not self._stop.is_set():
            if buf and time.monotonic() - last > 0.1:
                buf = b""                 # the firmware gives up on a partial frame...
                if not self.silent:
                    self._reply(0x48)     # ...and says so
            ready, _, _ = select.select([self.master], [], [], 0.005)
            if not ready:
                continue
            try:
                chunk = os.read(self.master, 4096)
            except OSError:
                time.sleep(0.05)
                continue
            if termios.tcgetattr(self.master)[4] != self.SPEEDS[self.baud]:
                continue                  # sent at another line speed: noise to the board
            if self._lose and not buf and chunk[:2] == bytes([0x33, CMD_KEY]):
                self._lose, chunk = False, chunk[1:]
            buf += chunk
            last = time.monotonic()
            while len(buf) >= 8:
                frame, buf = buf[:8], buf[8:]
                code = self._handle(frame)
                garble = self._garble and frame[1] == CMD_KEY and code == 0x80
                if garble:
                    self._garble = False
                if not self.silent:
                    self._reply(code, garble)


class KvmdHidTestBase(TargetTestBase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.boards = []

    @classmethod
    def tearDownClass(cls):
        super().tearDownClass()
        for board in cls.boards:
            board.close()

    def target_with(self, board, wait_ready=True, **extra):
        type(self).boards.append(board)
        TargetTestBase._seq += 1
        body = {"name": f"Pico {TargetTestBase._seq}", "serial_device": board.path,
                "serial_adapter": "kvmd-hid",
                "v4l2_device": f"/dev/video{500 + TargetTestBase._seq}", "enabled": True}
        body.update(extra)
        status, raw, _ = self.owner.post("/api/targets", body)
        self.assertEqual(status, 201, raw)
        tid = self.owner.as_json(raw)["id"]
        if wait_ready:
            self.wait_for(lambda: self.owner.as_json(self.owner.get(f"/api/targets/{tid}")[1])
                          ["status"]["input_ready"], timeout=30, what="board to be input-ready")
        else:
            self.wait_for(lambda: self.owner.get(f"/api/targets/{tid}/health")[0] == 200
                          and self.health(tid)["kind"] == "kvmd-hid", timeout=30,
                          what="target to start")
        return tid

    def health(self, tid):
        status, raw, _ = self.owner.get(f"/api/targets/{tid}/health")
        self.assertEqual(status, 200, raw)
        return self.owner.as_json(raw)

    def wait_state(self, tid, state, timeout=30):
        self.wait_for(lambda: self.health(tid)["state"] == state, timeout=timeout,
                      what=f"health {state} (now {self.health(tid)})")
        return self.health(tid)

    def drive(self, tid):
        status, raw, _ = self.operator.post(f"/api/targets/{tid}/control/acquire")
        self.assertEqual(status, 200, raw)
        self.addCleanup(self.operator.post, f"/api/targets/{tid}/control/release")

    def send(self, tid, msg):
        status, raw, _ = self.operator.post(f"/api/targets/{tid}/input", msg)
        self.assertEqual(status, 200, raw)

    def key(self, tid, code, pressed=True):
        self.send(tid, {"type": "key", "code": code, "pressed": pressed})


class TestInput(KvmdHidTestBase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.board = None

    def setUp(self):
        super().setUp()
        if type(self).board is None:
            type(self).board = FakeBoard()
            type(self).tid = self.target_with(type(self).board)

    def test_keys_go_by_the_firmwares_own_numbers(self):
        b, tid = self.board, self.tid
        self.drive(tid)
        self.key(tid, "KeyA")
        self.key(tid, "ShiftLeft")
        self.wait_for(lambda: b.keys == {KEY_A, SHIFT_LEFT}, timeout=10, what=f"A+Shift held ({b.keys})")
        self.key(tid, "KeyA", False)
        self.key(tid, "ShiftLeft", False)
        self.wait_for(lambda: b.keys == set(), timeout=10, what=f"nothing held ({b.keys})")

        # One key from each run of the numbering, where USB order and the
        # firmware's part ways.
        expected = {"KeyZ": 26, "Digit0": 36, "Backslash": 46, "Semicolon": 47, "F12": 65,
                    "PrintScreen": 66, "ScrollLock": 86, "Pause": 85, "Insert": 67, "ArrowUp": 76,
                    "NumLock": 87, "NumpadDivide": 89, "NumpadDecimal": 104, "IntlBackslash": 106,
                    "ControlRight": 81, "MetaRight": 84}
        since = len(b.commands())
        for code in expected:
            self.key(tid, code)
            self.key(tid, code, False)
        want = [(n, s) for n in expected.values() for s in (1, 0)]
        got = lambda: [(a[0], a[1]) for c, a in b.commands()[since:] if c == CMD_KEY]
        self.wait_for(lambda: got() == want, timeout=10, what=f"key numbers (got {got()})")

    def test_pointer_position_buttons_and_wheel(self):
        b, tid = self.board, self.tid
        self.drive(tid)
        for x, y, want in ((0, 0, (-32768, -32768)), (1, 1, (32767, 32767)),
                           (0.5, 0.25, (0, -16384))):
            self.send(tid, {"type": "mousemove", "x": x, "y": y})
            self.wait_for(lambda: b.pos == want, timeout=10, what=f"pointer at {want} (at {b.pos})")

        for button, name in ((0, "left"), (1, "middle"), (2, "right")):
            self.send(tid, {"type": "mousebutton", "button": button, "pressed": True})
            self.wait_for(lambda: b.buttons[name], timeout=10, what=f"{name} down")
            self.send(tid, {"type": "mousebutton", "button": button, "pressed": False})
            self.wait_for(lambda: not any(b.buttons.values()), timeout=10, what=f"{name} up")

        since = len(b.commands())
        self.send(tid, {"type": "mousescroll", "delta": 3})
        self.send(tid, {"type": "mousescroll", "delta": -2})
        wheel = lambda: [a[:2] for c, a in b.commands()[since:] if c == CMD_WHEEL]
        # Each step exactly once: a repeated scroll would scroll twice.
        self.wait_for(lambda: wheel() == [bytes([0, 3]), bytes([0, 0xFE])], timeout=10,
                      what=f"one wheel step each (got {wheel()})")

    def test_releasing_control_clears_without_moving_the_pointer(self):
        b, tid = self.board, self.tid
        self.drive(tid)
        self.send(tid, {"type": "mousemove", "x": 0.2, "y": 0.8})
        self.key(tid, "KeyB")
        self.send(tid, {"type": "mousebutton", "button": 0, "pressed": True})
        self.wait_for(lambda: b.keys == {KEY_B} and b.buttons["left"], timeout=10,
                      what="B and the left button held")
        pos = b.pos
        since = len(b.commands())
        status, raw, _ = self.operator.post(f"/api/targets/{tid}/control/release")
        self.assertEqual(status, 200, raw)
        self.wait_for(lambda: b.keys == set() and not any(b.buttons.values()), timeout=10,
                      what="everything let go")
        after = b.commands()[since:]
        self.assertIn(CMD_CLEAR, [c for c, _ in after])
        self.assertNotIn(CMD_ABS, [c for c, _ in after])
        self.assertEqual(b.pos, pos)

    def test_text_is_typed_exactly(self):
        b, tid = self.board, self.tid
        self.drive(tid)
        text = "Hello, World 42!"
        since = len(b.commands())
        self.send(tid, {"type": "text", "text": text})

        chars = {n: c for n, c in zip(range(1, 27), "abcdefghijklmnopqrstuvwxyz")}
        chars.update({n: c for n, c in zip(range(27, 37), "1234567890")})
        chars.update({41: " ", 50: ","})
        shifted = {27: "!"}

        def typed():
            out, shift = "", False
            for c, a in b.commands()[since:]:
                if c != CMD_KEY:
                    continue
                if a[0] == SHIFT_LEFT:
                    shift = bool(a[1])
                elif a[1]:
                    out += shifted.get(a[0], "?") if shift and a[0] in shifted else (
                        chars.get(a[0], "?").upper() if shift else chars.get(a[0], "?"))
            return out
        self.wait_for(lambda: typed() == text, timeout=30, what=f"{text!r} typed (got {typed()!r})")
        self.wait_for(lambda: b.keys == set(), timeout=10, what="nothing left held")

    def test_a_byte_lost_on_the_wire_is_recovered_from(self):
        # One lost byte leaves the firmware a byte short of a frame, so
        # everything after would be misaligned if the server just carried on.
        b, tid = self.board, self.tid
        self.drive(tid)
        b.lose_byte()
        self.key(tid, "KeyB")
        self.key(tid, "KeyB", False)
        self.key(tid, "KeyC")
        self.wait_for(lambda: b.keys == {KEY_C}, timeout=10, what=f"only C held ({b.keys})")
        self.key(tid, "KeyC", False)
        self.wait_for(lambda: b.keys == set(), timeout=10, what="nothing held")

    def test_a_garbled_reply_is_retried(self):
        # The board did press the key; the server can't know that, so it
        # sends it again, which a key state can take twice.
        b, tid = self.board, self.tid
        self.drive(tid)
        dropped = self.health(tid)["last_hour"]["dropped_commands"]
        since = len(b.commands())
        b.garble_key_reply()
        self.key(tid, "KeyA")
        presses = lambda: [a[:2] for c, a in b.commands()[since:] if c == CMD_KEY]
        self.wait_for(lambda: presses() == [bytes([KEY_A, 1])] * 2, timeout=10,
                      what=f"the press sent again (got {presses()})")
        self.assertEqual(b.keys, {KEY_A})
        self.assertEqual(self.health(tid)["last_hour"]["dropped_commands"], dropped)
        self.key(tid, "KeyA", False)
        self.wait_for(lambda: b.keys == set(), timeout=10, what="nothing held")


class TestHealth(KvmdHidTestBase):
    def test_healthy_board(self):
        tid = self.target_with(FakeBoard())
        h = self.wait_state(tid, "good")
        self.assertEqual(h["kind"], "kvmd-hid")
        self.assertIs(h["target_enumerated"], True)
        self.assertEqual(h["configured_baud"], 115200)   # the firmware's usual speed, by default
        self.assertIsNone(h["firmware"])
        self.assertIsNone(h["baud_change"])               # nothing in the board to rewrite
        self.assertGreaterEqual(h["last_hour"]["checks_ok"], 1)

    def test_target_has_not_recognised_it(self):
        tid = self.target_with(FakeBoard(enumerated=False), wait_ready=False)
        self.assertIn("hasn't recognised", self.wait_state(tid, "down")["summary"])

    def test_a_relative_mouse_is_named(self):
        tid = self.target_with(FakeBoard(mouse=0x10), wait_ready=False)
        self.assertIn("relative or PS/2", self.wait_state(tid, "down")["summary"])

    def test_a_fixed_mode_build_with_a_relative_mouse_is_named_too(self):
        tid = self.target_with(FakeBoard(mouse=0x10, dynamic=False), wait_ready=False)
        self.assertIn("relative or PS/2", self.wait_state(tid, "down")["summary"])

    def test_a_board_that_goes_quiet_stops_taking_input_at_once(self):
        # Without this, input would keep being accepted until the next
        # periodic check, each command waiting out its timeouts, and what
        # queued up meanwhile could reach the target seconds late.
        board = FakeBoard()
        tid = self.target_with(board)
        self.drive(tid)
        since = len(board.commands())
        board.silent = True
        for code in ("KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH"):
            self.operator.post(f"/api/targets/{tid}/input",
                               {"type": "key", "code": code, "pressed": True})
        status = lambda: self.owner.as_json(self.owner.get(f"/api/targets/{tid}")[1])["status"]
        self.wait_for(lambda: not status()["input_ready"], timeout=3,
                      what="input to be marked unavailable")
        time.sleep(1)
        # The first key's two attempts went out; what queued behind it was
        # dropped rather than tried in turn.
        keys = [c for c, _ in board.commands()[since:] if c == CMD_KEY]
        self.assertLessEqual(len(keys), 2, keys)
        refused, _, _ = self.operator.post(f"/api/targets/{tid}/input",
                                           {"type": "key", "code": "KeyB", "pressed": True})
        self.assertEqual(refused, 503)
        board.silent = False
        self.wait_for(lambda: status()["input_ready"], timeout=30, what="the board to be back")

    def test_silence_and_recovery(self):
        board = FakeBoard()
        tid = self.target_with(board)
        self.wait_state(tid, "good")
        board.silent = True
        self.assertIn("isn't answering", self.wait_state(tid, "down", timeout=40)["summary"])
        board.silent = False
        self.wait_state(tid, "good", timeout=60)

    def test_baud_mismatch_is_named(self):
        tid = self.target_with(FakeBoard(baud=115200), wait_ready=False, serial_baud=9600)
        self.wait_for(lambda: self.health(tid)["answers_at_baud"] == 115200, timeout=40,
                      what="the board's real speed to be found")
        h = self.health(tid)
        self.assertEqual(h["state"], "down")
        self.assertIn("115200", h["summary"])

    def test_self_test(self):
        tid = self.target_with(FakeBoard())
        status, raw, _ = self.owner.post(f"/api/targets/{tid}/health/test")
        self.assertEqual(status, 202, raw)
        self.wait_for(lambda: self.health(tid)["test"]["state"] == "done", timeout=30,
                      what="self-test to finish")
        t = self.health(tid)["test"]
        self.assertEqual((t["probes"], t["ok"]), (20, 20))
        self.assertEqual(t["verdict_state"], "good")
        self.assertIs(t["target_enumerated"], True)

    def test_no_speed_change(self):
        tid = self.target_with(FakeBoard())
        status, raw, _ = self.owner.post(f"/api/targets/{tid}/health/baud", {"baud": 57600})
        self.assertEqual(status, 400, raw)


class TestSetting(KvmdHidTestBase):
    def config(self, tid):
        return self.owner.as_json(self.owner.get(f"/api/targets/{tid}")[1])["config"]

    def test_existing_and_default_targets_are_ch9329(self):
        t = self.create("Plain")
        self.assertEqual((t["config"]["serial_adapter"], t["config"]["serial_baud"]), ("ch9329", 9600))

    def test_only_known_adapters(self):
        for bad in ("kvmd", "CH9329", "", 5):
            status, raw, _ = self.owner.post("/api/targets", self.mk("Bad", serial_adapter=bad))
            self.assertEqual(status, 400, (bad, raw))

    def test_an_adapter_change_without_a_speed_takes_its_usual_one(self):
        tid = self.create("Switcher", serial_adapter="kvmd-hid")["id"]
        self.assertEqual(self.config(tid)["serial_baud"], 115200)
        self.assertEqual(self.owner.put(f"/api/targets/{tid}", {"serial_adapter": "ch9329"})[0], 200)
        self.assertEqual(self.config(tid)["serial_baud"], 9600)
        # A speed given with the change is kept, and so is one changed alone.
        self.owner.put(f"/api/targets/{tid}", {"serial_adapter": "kvmd-hid", "serial_baud": 57600})
        self.assertEqual(self.config(tid)["serial_baud"], 57600)
        self.owner.put(f"/api/targets/{tid}", {"serial_baud": 38400})
        self.assertEqual((self.config(tid)["serial_adapter"], self.config(tid)["serial_baud"]),
                         ("kvmd-hid", 38400))


if __name__ == "__main__":
    unittest.main()
