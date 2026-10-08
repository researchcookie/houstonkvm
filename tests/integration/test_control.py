"""Per-target control and input: scoped acquire/release/status/input routes,
the input WebSocket bound to a target, and — the point of the exercise — that
whatever ends a driver's control also releases the keys and buttons they had
down, so nothing is left stuck on the target.

Each target here is wired to a pty standing in for a CH9329 serial link. A
FakeChip on the master side answers the server's health probes (so the
backend comes up ready) and records every frame the server writes, which lets
these tests look at the actual HID reports a target would receive rather than
inferring from HTTP status codes. CH9329 framing: 57 AB ADDR CMD LEN DATA SUM;
CMD 0x02 is a keyboard report (data: modifiers, 0, six keys), 0x04 an absolute
mouse report, 0x05 a relative mouse report (data: 0x01, buttons, dx, dy, wheel).
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
from server_harness import Client
from test_targets import OWNER, TargetTestBase, login

KEY_A, KEY_B = 0x04, 0x05
SHIFT_LEFT = 0x02  # modifier bit


class FakeChip:
    """A pty pair posing as one CH9329 on a serial port.

    By default it answers like a healthy chip the target has enumerated. The
    options (also settable while running) make it misbehave the ways real
    links do: `silent` answers nothing (a host-side fault), `enumerated`
    False is a chip the target hasn't recognised (the target-side cable),
    `fail_every` loses every Nth GET_INFO reply (a marginal cable),
    `ack_delay` delays the acks to input reports (as at low baud rates),
    `info_delay` answers GET_INFO late but in time (a slow chip), and
    `baud` makes it deaf to traffic at any other line speed. A new stored rate
    (SET_PARA_CFG) takes effect as `applies` says: "now", after a "reset", or
    "never" (a board whose configuration pins force the default), and
    `config_delay` holds back the reply to it, so a speed change lasts long
    enough to be seen running.
    """

    SPEEDS = {1200: termios.B1200, 2400: termios.B2400, 4800: termios.B4800,
              9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400,
              57600: termios.B57600, 115200: termios.B115200}

    def __init__(self, enumerated=True, silent=False, fail_every=0, ack_delay=0.0, baud=None,
                 applies="now", config_delay=0.0, info_delay=0.0):
        self.enumerated, self.silent = enumerated, silent
        self.info_delay = info_delay
        self.config_delay = config_delay
        self.fail_every, self.ack_delay, self.baud = fail_every, ack_delay, baud
        self.applies, self.stored = applies, baud or 9600
        self.info_requests = 0
        self.master, self.slave = os.openpty()  # slave stays open: else master reads EIO
        self.path = os.ttyname(self.slave)
        self._lock = threading.Lock()
        self._frames = []   # (cmd, bytes)
        self._pending = []  # (due, reply) for delayed acks
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def close(self):
        self._stop.set()
        self._thread.join(timeout=2)
        os.close(self.master)
        os.close(self.slave)

    def config(self):
        # Laid out like the real board's: hardware-pin modes, then the baud.
        cfg = bytearray(50)
        cfg[0:3] = bytes([0x80, 0x80, 0x00])
        cfg[3:7] = self.stored.to_bytes(4, "big")
        cfg[7:15] = bytes([0x08, 0x00, 0x00, 0x03, 0x86, 0x1A, 0x29, 0xE1])
        return bytes(cfg)

    def _reply(self, cmd, data, delay=0.0):
        # In order, like the real chip: a reply never overtakes the acks for
        # commands sent before it, however late those are.
        body = bytes([0x57, 0xAB, 0x00, cmd | 0x80, len(data)]) + data
        frame = body + bytes([sum(body) & 0xFF])
        due = time.monotonic() + delay
        if self._pending:
            due = max(due, self._pending[-1][0])
        if due > time.monotonic():
            self._pending.append((due, frame))
        else:
            os.write(self.master, frame)

    def _answer(self, cmd, data):
        if self.silent:
            return
        if cmd == 0x01:                                   # GET_INFO
            self.info_requests += 1
            if self.fail_every and self.info_requests % self.fail_every == 0:
                return
            self._reply(cmd, bytes([0x30, 0x01 if self.enumerated else 0x00, 0, 0, 0, 0, 0, 0]),
                        self.info_delay)
        elif cmd == 0x08:                                 # GET_PARA_CFG
            self._reply(cmd, self.config())
        elif cmd == 0x09:                                 # SET_PARA_CFG
            if len(data) != 50:
                self._reply(cmd, b"\xE5")
                return
            self.stored = int.from_bytes(data[3:7], "big")
            self._reply(cmd, b"\x00", self.config_delay)
            if self.applies == "now":
                self.baud = self.stored
        elif cmd == 0x0F:                                 # RESET
            self._reply(cmd, b"\x00")
            if self.applies == "reset":
                self.baud = self.stored
        else:                                             # input reports: a status ack
            self._reply(cmd, b"\x00", self.ack_delay)

    def _run(self):
        buf = b""
        while not self._stop.is_set():
            now = time.monotonic()
            while self._pending and self._pending[0][0] <= now:
                os.write(self.master, self._pending.pop(0)[1])
            ready, _, _ = select.select([self.master], [], [], 0.005)
            if not ready:
                continue
            try:
                chunk = os.read(self.master, 4096)
            except OSError:
                time.sleep(0.05)
                continue
            if self.baud and termios.tcgetattr(self.master)[4] != self.SPEEDS[self.baud]:
                buf = b""  # sent at another line speed: noise to the chip
                continue
            buf += chunk
            while True:
                i = buf.find(b"\x57\xab")
                if i < 0:
                    buf = buf[-1:]
                    break
                buf = buf[i:]
                if len(buf) < 5:
                    break
                total = 6 + buf[4]
                if len(buf) < total:
                    break
                frame, rest = buf[:total], buf[total:]
                if sum(frame[:-1]) & 0xFF != frame[-1]:
                    buf = buf[1:]  # not a frame after all; resync
                    continue
                buf = rest
                cmd = frame[3]
                with self._lock:
                    self._frames.append((cmd, frame[5:-1]))
                self._answer(cmd, frame[5:-1])

    def frames(self, cmd):
        with self._lock:
            return [d for c, d in self._frames if c == cmd]

    def keyboard(self):
        """(modifiers, {keycodes}) from the latest keyboard report, or None."""
        kb = self.frames(0x02)
        if not kb:
            return None
        return kb[-1][0], {k for k in kb[-1][2:8] if k}

    def held(self):
        kb = self.keyboard()
        return set() if kb is None else ({"mod%02x" % kb[0]} if kb[0] else set()) | kb[1]

    def buttons(self):
        rel = self.frames(0x05)
        return rel[-1][1] if rel else 0


class ControlTestBase(TargetTestBase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.chips = {}
        cls.op1 = cls.operator
        cls.op2 = cls._make_user("operator2", "operatorpassword2", "operator")
        cls.tid = {}
        for name in ("A", "B"):
            cls.tid[name] = cls._start_target(name)

    @classmethod
    def _start_target(cls, name):
        chip = FakeChip()
        cls.chips[name] = chip
        TargetTestBase._seq += 1
        status, raw, _ = cls.owner.post("/api/targets", {
            "name": f"Chip {name}", "serial_device": chip.path,
            "v4l2_device": f"/dev/video{200 + TargetTestBase._seq}", "enabled": True,
        })
        assert status == 201, raw
        tid = cls.owner.as_json(raw)["id"]
        cls._wait_ready(tid)
        return tid

    @classmethod
    def _wait_ready(cls, tid, timeout=40):
        deadline = time.time() + timeout
        while time.time() < deadline:
            st = cls.owner.as_json(cls.owner.get(f"/api/targets/{tid}")[1])
            if st["status"]["input_ready"]:
                return
            time.sleep(0.2)
        raise AssertionError(f"target {tid} never became input-ready")

    @classmethod
    def tearDownClass(cls):
        super().tearDownClass()
        for chip in cls.chips.values():
            chip.close()

    # -- helpers ---------------------------------------------------------
    def A(self):
        return self.tid["A"]

    def B(self):
        return self.tid["B"]

    def acquire(self, client, tid, expect=200):
        status, raw, _ = client.post(f"/api/targets/{tid}/control/acquire")
        self.assertEqual(status, expect, raw)
        if status == 200:
            self.addCleanup(self._release_quietly, client, tid)
        return raw

    def _release_quietly(self, client, tid):
        try:
            client.post(f"/api/targets/{tid}/control/release")
        except Exception:
            pass

    def release(self, client, tid):
        status, raw, _ = client.post(f"/api/targets/{tid}/control/release")
        self.assertEqual(status, 200, raw)

    def send(self, client, tid, msg, expect=200):
        status, raw, _ = client.post(f"/api/targets/{tid}/input", msg)
        self.assertEqual(status, expect, raw)
        return raw

    def key(self, client, tid, code, pressed=True):
        self.send(client, tid, {"type": "key", "code": code, "pressed": pressed})

    def status_of(self, client, tid):
        status, raw, _ = client.get(f"/api/targets/{tid}/control/status")
        self.assertEqual(status, 200, raw)
        return client.as_json(raw)

    def wait_held(self, chip, expected, what=""):
        self.wait_for(lambda: chip.held() == expected, timeout=10,
                      what=f"{what} chip to hold {expected} (has {chip.held()})")

    def new_operator(self, name):
        client = self._make_user(name, "operatorpassword9", "operator")
        return client

    def user_id(self, name):
        users = self.owner.as_json(self.owner.get("/api/users")[1])
        users = users["users"] if isinstance(users, dict) else users
        return next(u["id"] for u in users if u["username"] == name)


class TestPerTargetControl(ControlTestBase):
    def test_01_locks_are_per_target(self):
        self.acquire(self.op1, self.A())
        self.acquire(self.op2, self.A(), expect=409)   # A is taken...
        self.acquire(self.op2, self.B())               # ...B is independent

        self.assertEqual(self.status_of(self.op1, self.A()),
                          {"controlled": True, "is_driver": True, "driver": "operator1", "typing_remaining": 0})
        self.assertEqual(self.status_of(self.op2, self.A()),
                          {"controlled": True, "is_driver": False, "driver": "operator1", "typing_remaining": 0})
        self.assertEqual(self.status_of(self.op2, self.B()),
                          {"controlled": True, "is_driver": True, "driver": "operator2", "typing_remaining": 0})

        listed = {t["id"]: t["status"]["controlled"]
                  for t in self.owner.as_json(self.owner.get("/api/targets")[1])["targets"]}
        self.assertTrue(listed[self.A()] and listed[self.B()])

        # Only Operators drive; and ids are checked.
        self.assertEqual(self.viewer.post(f"/api/targets/{self.A()}/control/acquire")[0], 403)
        self.assertEqual(self.owner.post(f"/api/targets/{self.A()}/control/acquire")[0], 403)
        self.assertEqual(self.op1.post("/api/targets/99999/control/acquire")[0], 404)
        self.assertEqual(self.op1.post("/api/targets/abc/control/acquire")[0], 400)
        self.assertEqual(self.op1.get("/api/targets/99999/control/status")[0], 404)

    def test_02_input_lands_on_the_right_target(self):
        self.acquire(self.op1, self.A())
        self.key(self.op1, self.A(), "KeyA")
        self.wait_held(self.chips["A"], {KEY_A}, "A")
        self.assertEqual(self.chips["B"].held(), set())   # nothing leaked to B

        # Holding A's lock says nothing about B.
        status, raw, _ = self.op1.post(f"/api/targets/{self.B()}/input",
                                       {"type": "key", "code": "KeyA", "pressed": True})
        self.assertEqual((status, raw), (403, b"Not the driver"))
        self.assertEqual(self.chips["B"].held(), set())

        self.key(self.op1, self.A(), "KeyA", pressed=False)
        self.wait_held(self.chips["A"], set(), "A")

    def test_03_releasing_control_releases_held_keys_and_buttons(self):
        chip = self.chips["A"]
        self.acquire(self.op1, self.A())
        self.key(self.op1, self.A(), "KeyA")
        self.key(self.op1, self.A(), "ShiftLeft")
        self.send(self.op1, self.A(), {"type": "mousebutton", "button": 0, "pressed": True})
        self.wait_held(chip, {KEY_A, "mod%02x" % SHIFT_LEFT}, "A")
        self.wait_for(lambda: chip.buttons() == 0x01, timeout=10, what="button down")

        # The client never sends a keyup or button-up — as when a tab dies.
        self.release(self.op1, self.A())
        self.wait_held(chip, set(), "A after release")
        self.wait_for(lambda: chip.buttons() == 0, timeout=10, what="button release")
        self.assertEqual(self.status_of(self.op2, self.A())["controlled"], False)

    def test_04_release_never_moves_the_cursor(self):
        chip = self.chips["A"]
        before = len(chip.frames(0x04))
        self.acquire(self.op1, self.A())
        self.key(self.op1, self.A(), "KeyA")
        self.wait_held(chip, {KEY_A}, "A")
        self.release(self.op1, self.A())
        self.wait_held(chip, set(), "A")
        # An absolute report would snap the target's cursor to our last-sent
        # position (the centre, if it never moved); no key-only session
        # should produce one.
        self.assertEqual(len(chip.frames(0x04)), before)

    def test_05_acquire_starts_from_a_clean_state(self):
        chip = self.chips["B"]
        before = len(chip.frames(0x02))
        self.acquire(self.op1, self.B())
        self.wait_for(lambda: len(chip.frames(0x02)) > before, timeout=10, what="clean-slate report")
        self.assertEqual(chip.held(), set())

    def test_06_only_the_driver_or_an_owner_can_release(self):
        chip = self.chips["A"]
        self.acquire(self.op1, self.A())
        self.key(self.op1, self.A(), "KeyA")
        self.wait_held(chip, {KEY_A}, "A")

        self.release(self.op2, self.A())  # 200, but not theirs to release
        self.assertEqual(self.status_of(self.op1, self.A()),
                          {"controlled": True, "is_driver": True, "driver": "operator1", "typing_remaining": 0})
        time.sleep(0.5)
        self.assertEqual(chip.held(), {KEY_A})

        status, raw, _ = self.owner.post(f"/api/targets/{self.A()}/control/release")
        self.assertEqual(status, 200, raw)
        self.wait_held(chip, set(), "A after owner override")
        self.assertEqual(self.status_of(self.op2, self.A())["controlled"], False)
        self.assertEqual(self.owner.post("/api/targets/99999/control/release")[0], 404)

    def test_07_logout_releases(self):
        chip = self.chips["A"]
        op = self.new_operator("logout_op")
        self.acquire(op, self.A())
        self.key(op, self.A(), "KeyB")
        self.wait_held(chip, {KEY_B}, "A")
        self.assertEqual(op.post("/api/logout")[0], 200)
        self.wait_held(chip, set(), "A after logout")
        self.acquire(self.op2, self.A())  # lock is free again


class TestInputSocket(ControlTestBase):
    def test_01_socket_is_bound_to_its_target(self):
        chip_a, chip_b = self.chips["A"], self.chips["B"]
        self.acquire(self.op1, self.A())
        ws = self.op1.ws(f"/api/targets/{self.A()}/input/ws")
        self.addCleanup(ws.close)
        self.assertTrue(ws.upgraded, ws.status_line)
        ws.send_json({"type": "key", "code": "KeyA", "pressed": True})
        self.wait_held(chip_a, {KEY_A}, "A")
        self.assertEqual(chip_b.held(), set())

    def test_02_not_driver_is_reported(self):
        self.acquire(self.op1, self.A())
        # The previous test's cleanup releases asynchronously; start from a
        # settled chip so the check below can only see what *this* test caused.
        self.wait_held(self.chips["A"], set(), "A baseline")
        ws = self.op2.ws(f"/api/targets/{self.A()}/input/ws")  # authorized to connect, not to drive
        self.addCleanup(ws.close)
        self.assertTrue(ws.upgraded)
        ws.send_json({"type": "key", "code": "KeyA", "pressed": True})
        self.assertEqual(ws.recv_json(), {"ok": False, "reason": "not_driver"})
        time.sleep(0.3)  # give a wrongly-accepted key time to reach the chip
        self.assertEqual(self.chips["A"].held(), set())

    def test_03_upgrade_refusals(self):
        self.assertEqual(self.new_client().ws(f"/api/targets/{self.A()}/input/ws").status, 401)
        self.assertEqual(self.viewer.ws(f"/api/targets/{self.A()}/input/ws").status, 403)
        self.assertEqual(self.op1.ws("/api/targets/99999/input/ws").status, 404)
        self.assertEqual(self.op1.ws("/api/targets/abc/input/ws").status, 400)

    def test_04_closing_the_socket_clears_keys_but_keeps_the_seat(self):
        chip = self.chips["A"]
        self.acquire(self.op1, self.A())
        ws = self.op1.ws(f"/api/targets/{self.A()}/input/ws")
        self.assertTrue(ws.upgraded)
        ws.send_json({"type": "key", "code": "KeyB", "pressed": True})
        self.wait_held(chip, {KEY_B}, "A")

        ws.close()  # tab closed / connection lost, no keyup ever sent
        self.wait_held(chip, set(), "A after socket close")
        # A blip shouldn't cost the driver their seat...
        self.assertTrue(self.status_of(self.op1, self.A())["is_driver"])
        # ...and a reconnect carries on driving.
        ws2 = self.op1.ws(f"/api/targets/{self.A()}/input/ws")
        self.addCleanup(ws2.close)
        ws2.send_json({"type": "key", "code": "KeyA", "pressed": True})
        self.wait_held(chip, {KEY_A}, "A after reconnect")

    def test_05_a_non_driver_socket_closing_disturbs_nobody(self):
        chip = self.chips["A"]
        self.acquire(self.op1, self.A())
        self.key(self.op1, self.A(), "KeyA")
        self.wait_held(chip, {KEY_A}, "A")
        bystander = self.op2.ws(f"/api/targets/{self.A()}/input/ws")
        bystander.close()
        time.sleep(0.5)
        self.assertEqual(chip.held(), {KEY_A})


class TestTargetLifecycle(ControlTestBase):
    def test_01_stopping_a_target_releases_its_keys(self):
        chip = self.chips["B"]
        self.acquire(self.op1, self.B())
        self.key(self.op1, self.B(), "KeyA")
        self.wait_held(chip, {KEY_A}, "B")

        status, raw, _ = self.owner.put(f"/api/targets/{self.B()}", {"enabled": False})
        self.assertEqual(status, 200, raw)
        # The chip keeps its last report after the server stops talking to
        # it, so the release has to go out before the backend is destroyed.
        self.wait_held(chip, set(), "B after disable")
        self.assertEqual(self.op1.post(f"/api/targets/{self.B()}/control/acquire")[1], b"Target is disabled")

        # Coming back up: nobody holds a lock on the new runtime.
        self.assertEqual(self.owner.put(f"/api/targets/{self.B()}", {"enabled": True})[0], 200)
        self._wait_ready(self.B())
        self.assertFalse(self.status_of(self.op2, self.B())["controlled"])
        self.acquire(self.op2, self.B())


class TestLapsedCredentials(ControlTestBase):
    """The input hot path trusts the driver lock without a database lookup,
    so a driver whose credentials stop being valid keeps driving unless
    something checks — the server does, every couple of seconds."""

    LAPSE_TIMEOUT = 12

    def _driver_with_key(self, client, tid=None):
        tid = tid or self.A()
        self.acquire(client, tid)
        self.key(client, tid, "KeyA")
        self.wait_held(self.chips["A"] if tid == self.A() else self.chips["B"], {KEY_A}, "target")

    def _assert_released(self, tid, chip):
        self.wait_for(lambda: not self.status_of(self.op2, tid)["controlled"],
                      timeout=self.LAPSE_TIMEOUT, what="lock to be released")
        self.wait_held(chip, set(), "released chip")
        self.acquire(self.op2, tid)  # and someone else can now drive

    def test_01_deleted_user(self):
        op = self.new_operator("doomed_op")
        self._driver_with_key(op)
        status, raw, _ = self.owner.delete(f"/api/users/{self.user_id('doomed_op')}")
        self.assertEqual(status, 200, raw)
        self._assert_released(self.A(), self.chips["A"])

    def test_02_demoted_user(self):
        op = self.new_operator("demoted_op")
        self._driver_with_key(op)
        status, raw, _ = self.owner.put(f"/api/users/{self.user_id('demoted_op')}/role", {"role": "viewer"})
        self.assertEqual(status, 200, raw)
        self._assert_released(self.A(), self.chips["A"])

    def test_03_revoked_api_token(self):
        # An automation client's shape: a bearer token, not a session.
        op = self.new_operator("bot_owner")
        status, raw, _ = op.post("/api/tokens", {"label": "driver"})
        created = op.as_json(raw)
        bot = Client(self.server.port)
        bot.bearer = created["token"]

        self.acquire(bot, self.A())
        self.key(bot, self.A(), "KeyA")
        self.wait_held(self.chips["A"], {KEY_A}, "A")

        self.assertEqual(op.delete(f"/api/tokens/{created['id']}")[0], 200)
        self._assert_released(self.A(), self.chips["A"])
        # The revoked token can't drive, or even ask to.
        self.assertEqual(bot.post(f"/api/targets/{self.A()}/input",
                                  {"type": "key", "code": "KeyA", "pressed": True})[0], 401)

    def test_04_a_valid_driver_is_left_alone(self):
        self._driver_with_key(self.op1)
        time.sleep(5)  # comfortably more than two revalidation cycles
        self.assertTrue(self.status_of(self.op1, self.A())["is_driver"])
        self.assertEqual(self.chips["A"].held(), {KEY_A})


# HID usage -> (unshifted, shifted) on a US layout, for reading typed text
# back out of the keyboard reports a chip received.
US_KEYS = {0x28: ("\n", None), 0x2B: ("\t", None), 0x2C: (" ", None),
           0x2D: ("-", "_"), 0x2E: ("=", "+"), 0x2F: ("[", "{"), 0x30: ("]", "}"),
           0x31: ("\\", "|"), 0x33: (";", ":"), 0x34: ("'", '"'), 0x35: ("`", "~"),
           0x36: (",", "<"), 0x37: (".", ">"), 0x38: ("/", "?")}
for _i, _c in enumerate("abcdefghijklmnopqrstuvwxyz"):
    US_KEYS[0x04 + _i] = (_c, _c.upper())
for _i, (_d, _s) in enumerate(zip("1234567890", "!@#$%^&*()")):
    US_KEYS[0x1E + _i] = (_d, _s)
SHIFT_BITS = 0x22  # left or right Shift
KEY_ESCAPE = 0x29


class TestTypingText(ControlTestBase):
    """Pasted text arrives as {"type": "text"} and the server types it one
    key at a time, paced for the link, between the driver's own input."""

    def typed(self, chip, since):
        """The text a target would have seen from keyboard reports since
        frame `since`: every newly pressed key, read with the Shift state of
        the report that pressed it. Keys the chip was already holding, or
        that aren't printable, come back as "<xx>"."""
        out, prev = [], set()
        for report in chip.frames(0x02)[since:]:
            keys = {k for k in report[2:8] if k}
            for k in sorted(keys - prev):
                plain, shifted = US_KEYS.get(k, (None, None))
                ch = shifted if report[0] & SHIFT_BITS else plain
                out.append(ch if ch is not None else "<%02x>" % k)
            prev = keys
        return "".join(out)

    def start_typing(self, client, tid, text):
        chip = self.chips["A"]
        since = len(chip.frames(0x02))
        self.send(client, tid, {"type": "text", "text": text})
        return chip, since

    def wait_typed(self, chip, since, expected, timeout=30):
        try:
            self.wait_for(lambda: self.typed(chip, since) == expected and chip.held() == set(),
                          timeout=timeout, what=f"typed {expected!r}")
        except AssertionError as e:
            raise AssertionError(f"{e}; have {self.typed(chip, since)!r}, holding {chip.held()}") from None

    def test_01_text_is_typed_exactly_and_nothing_is_left_held(self):
        self.acquire(self.op1, self.A())
        chip, since = self.start_typing(self.op1, self.A(), "Hello, World!\r\nline 2\tTAB\rend")
        # CRLF and a lone CR are each one Enter.
        self.wait_typed(chip, since, "Hello, World!\nline 2\tTAB\nend")
        self.assertEqual(self.status_of(self.op1, self.A())["typing_remaining"], 0)

    def test_02_every_printable_character(self):
        # A faster link (115200 baud) so the whole set is quick.
        TargetTestBase._seq += 1
        chip = FakeChip()
        self.chips["C"] = chip
        status, raw, _ = self.owner.post("/api/targets", {
            "name": "Chip C", "serial_device": chip.path, "serial_baud": 115200,
            "v4l2_device": f"/dev/video{200 + TargetTestBase._seq}", "enabled": True,
        })
        self.assertEqual(status, 201, raw)
        tid = self.owner.as_json(raw)["id"]
        self._wait_ready(tid)
        self.acquire(self.op1, tid)
        text = "".join(chr(c) for c in range(0x20, 0x7F)) + "\t\n"
        since = len(chip.frames(0x02))
        self.send(self.op1, tid, {"type": "text", "text": text})
        self.wait_typed(chip, since, text)

    def test_03_text_with_an_untypeable_character_is_refused_whole(self):
        chip = self.chips["A"]
        self.acquire(self.op1, self.A())
        since = len(chip.frames(0x02))
        raw = self.send(self.op1, self.A(), {"type": "text", "text": "café au lait"}, expect=400)
        self.assertIn(b"Character 4 ('\xc3\xa9' U+00E9)", raw)
        raw = self.send(self.op1, self.A(), {"type": "text", "text": "bell\x07"}, expect=400)
        self.assertIn(b"Character 5 (U+0007)", raw)
        time.sleep(0.3)
        self.assertEqual(self.typed(chip, since), "")

    def test_04_cancelling_stops_typing_and_leaves_nothing_held(self):
        self.acquire(self.op1, self.A())
        text = "Ab" * 200
        chip, since = self.start_typing(self.op1, self.A(), text)
        self.wait_for(lambda: len(self.typed(chip, since)) >= 3, timeout=10, what="typing to start")
        self.assertGreater(self.status_of(self.op1, self.A())["typing_remaining"], 0)
        self.send(self.op1, self.A(), {"type": "text_cancel"})
        self.wait_for(lambda: self.status_of(self.op1, self.A())["typing_remaining"] == 0 and chip.held() == set(),
                      timeout=10, what="typing to stop")
        typed = self.typed(chip, since)
        time.sleep(0.5)
        self.assertEqual(self.typed(chip, since), typed)  # and it stays stopped
        self.assertTrue(text.startswith(typed) and len(typed) < len(text), typed)

    def test_05_releasing_control_stops_typing(self):
        self.acquire(self.op1, self.A())
        chip, since = self.start_typing(self.op1, self.A(), "ZZZZ" * 100)
        self.wait_for(lambda: len(self.typed(chip, since)) >= 3, timeout=10, what="typing to start")
        self.release(self.op1, self.A())
        self.wait_held(chip, set(), "A after release")
        self.assertEqual(self.status_of(self.op2, self.A())["typing_remaining"], 0)
        typed = self.typed(chip, since)
        time.sleep(0.5)
        self.assertEqual(self.typed(chip, since), typed)
        # The next driver starts with nothing queued from the last one.
        self.acquire(self.op2, self.A())
        time.sleep(0.3)
        self.assertEqual(self.typed(chip, since), typed)

    def test_06_the_drivers_own_keys_are_not_held_up_behind_typing(self):
        self.acquire(self.op1, self.A())
        chip, since = self.start_typing(self.op1, self.A(), "a" * 400)
        self.wait_for(lambda: len(self.typed(chip, since)) >= 3, timeout=10, what="typing to start")
        started = time.monotonic()
        self.key(self.op1, self.A(), "Escape")
        self.wait_for(lambda: "<%02x>" % KEY_ESCAPE in self.typed(chip, since), timeout=5, what="Escape")
        self.assertLess(time.monotonic() - started, 1.0)
        self.assertGreater(self.status_of(self.op1, self.A())["typing_remaining"], 0)
        self.key(self.op1, self.A(), "Escape", pressed=False)
        self.send(self.op1, self.A(), {"type": "text_cancel"})
        self.wait_held(chip, set(), "A after cancel")

    def test_07_too_much_waiting_text_is_refused(self):
        self.acquire(self.op1, self.A())
        raw = self.send(self.op1, self.A(), {"type": "text", "text": "x" * 65537}, expect=503)
        self.assertEqual(raw, b"Too much text is already waiting to be typed")
        self.assertEqual(self.status_of(self.op1, self.A())["typing_remaining"], 0)

    def test_08_only_the_driver_can_type(self):
        self.acquire(self.op1, self.A())
        status, raw, _ = self.op2.post(f"/api/targets/{self.A()}/input", {"type": "text", "text": "hi"})
        self.assertEqual((status, raw), (403, b"Not the driver"))

    def test_09_text_over_the_input_socket(self):
        self.acquire(self.op1, self.A())
        chip = self.chips["A"]
        since = len(chip.frames(0x02))
        ws = self.op1.ws(f"/api/targets/{self.A()}/input/ws")
        self.addCleanup(ws.close)
        self.assertTrue(ws.upgraded)
        ws.send_json({"type": "text", "text": "echo ok\n" * 18})  # a frame past 125 bytes
        self.wait_typed(chip, since, "echo ok\n" * 18)


class TestUnscopedAliases(ControlTestBase):
    def test_unscoped_routes_drive_the_default_target(self):
        self.assertEqual(self.owner.put(f"/api/targets/{self.A()}", {"default": True})[0], 200)
        chip_a, chip_b = self.chips["A"], self.chips["B"]

        status, raw, _ = self.op1.post("/api/control/acquire")
        self.assertEqual(status, 200, raw)
        self.addCleanup(self.op1.post, "/api/control/release")
        self.assertTrue(self.status_of(self.op1, self.A())["is_driver"])
        self.assertTrue(self.op1.as_json(self.op1.get("/api/control/status")[1])["is_driver"])

        self.assertEqual(self.op1.post("/api/input", {"type": "key", "code": "KeyA", "pressed": True})[0], 200)
        self.wait_held(chip_a, {KEY_A}, "A")

        ws = self.op1.ws("/api/input/ws")
        self.addCleanup(ws.close)
        self.assertTrue(ws.upgraded)
        ws.send_json({"type": "key", "code": "KeyB", "pressed": True})
        self.wait_held(chip_a, {KEY_A, KEY_B}, "A")

        # Unscoped release clears the keys just the same.
        self.assertEqual(self.op1.post("/api/control/release")[0], 200)
        self.wait_held(chip_a, set(), "A after unscoped release")

        # Re-designating the default re-points the aliases; the old
        # driver's socket doesn't follow along and steer the new machine.
        self.assertEqual(self.owner.put(f"/api/targets/{self.B()}", {"default": True})[0], 200)
        ws.send_json({"type": "key", "code": "KeyA", "pressed": True})
        self.assertEqual(ws.recv_json(), {"ok": False, "reason": "not_driver"})
        self.assertEqual(chip_b.held(), set())

        self.assertEqual(self.op1.post("/api/control/acquire")[0], 200)
        self.assertEqual(self.op1.post("/api/input", {"type": "key", "code": "KeyB", "pressed": True})[0], 200)
        self.wait_held(chip_b, {KEY_B}, "B")
        self.assertEqual(chip_a.held(), set())


if __name__ == "__main__":
    unittest.main()
