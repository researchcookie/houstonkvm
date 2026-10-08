"""Input-link health: GET /api/targets/:id/health, the on-demand self-test,
and the `input_health` summary in the target list.

Each target is wired to a FakeChip (see test_control.py) set up to fail the
way a real link does, and the tests check the server blames the right thing:
the host-side cable when the chip goes quiet, the target-side cable when the
target hasn't enumerated it, the baud setting when it answers at another
rate, and a marginal cable when replies go missing now and then.

The server checks each chip every 5 seconds, so the passive tests wait on
the order of that interval.
"""
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import Client
from test_control import FakeChip
from test_targets import TargetTestBase


class HealthTestBase(TargetTestBase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.chips = []

    @classmethod
    def tearDownClass(cls):
        super().tearDownClass()
        for chip in cls.chips:
            chip.close()

    def target_with(self, chip, **extra):
        type(self).chips.append(chip)
        TargetTestBase._seq += 1
        body = {"name": f"Health {TargetTestBase._seq}", "serial_device": chip.path,
                "v4l2_device": f"/dev/video{300 + TargetTestBase._seq}", "enabled": True}
        body.update(extra)
        status, raw, _ = self.owner.post("/api/targets", body)
        self.assertEqual(status, 201, raw)
        tid = self.owner.as_json(raw)["id"]
        # The input backend is built on the target's worker thread after start.
        self.wait_for(lambda: self.owner.get(f"/api/targets/{tid}/health")[0] == 200
                      and self.health(tid)["kind"] == "ch9329", timeout=30, what="target to start")
        return tid

    def health(self, tid, client=None):
        client = client or self.owner
        status, raw, _ = client.get(f"/api/targets/{tid}/health")
        self.assertEqual(status, 200, raw)
        return client.as_json(raw)

    def wait_state(self, tid, states, timeout=30):
        states = {states} if isinstance(states, str) else set(states)
        self.wait_for(lambda: self.health(tid)["state"] in states, timeout=timeout,
                      what=f"health in {states} (now {self.health(tid)})")
        return self.health(tid)

    def listed(self, tid):
        targets = self.owner.as_json(self.owner.get("/api/targets")[1])["targets"]
        return next(t for t in targets if t["id"] == tid)["status"]

    def run_test(self, tid):
        status, raw, _ = self.owner.post(f"/api/targets/{tid}/health/test")
        self.assertEqual(status, 202, raw)
        self.wait_for(lambda: self.health(tid)["test"]["state"] == "done", timeout=30,
                      what="self-test to finish")
        return self.health(tid)["test"]


class TestPassiveHealth(HealthTestBase):
    def test_healthy_link(self):
        tid = self.target_with(FakeChip())
        h = self.wait_state(tid, "good")
        self.assertEqual(h["kind"], "ch9329")
        self.assertIs(h["target_enumerated"], True)
        self.assertEqual(h["firmware"], "3.0")
        self.assertEqual((h["configured_baud"], h["chip_baud"]), (9600, 9600))
        self.assertIsNone(h["answers_at_baud"])
        self.assertGreaterEqual(h["last_hour"]["checks_ok"], 1)
        self.assertEqual(h["last_hour"]["checks_failed"], 0)
        self.assertEqual(self.listed(tid)["input_health"], "good")
        self.assertEqual(h["test"], {"state": "idle"})

    def test_target_side_cable(self):
        tid = self.target_with(FakeChip(enumerated=False))
        h = self.wait_state(tid, "down")
        self.assertIs(h["target_enumerated"], False)
        self.assertIn("USB cable to the target", h["summary"])

    def test_host_side_silence_and_recovery(self):
        chip = FakeChip()
        tid = self.target_with(chip)
        self.wait_state(tid, "good")
        chip.silent = True                                   # the adapter's cable is pulled
        h = self.wait_state(tid, "down")
        self.assertIn("isn't answering", h["summary"])
        self.assertEqual(self.listed(tid)["input_health"], "down")
        self.assertFalse(self.listed(tid)["input_ready"])    # can't take control of a dead link
        self.assertEqual(self.operator.post(f"/api/targets/{tid}/control/acquire")[0], 503)

        chip.silent = False                                  # plugged back in
        self.wait_for(lambda: self.listed(tid)["input_ready"], timeout=40, what="recovery")
        # It was down a while, so the hour's record says so.
        self.assertIn(self.health(tid)["state"], ("good", "degraded"))

    def test_baud_mismatch_is_named(self):
        tid = self.target_with(FakeChip(baud=115200))       # target left at the default 9600
        h = self.wait_state(tid, "down")
        self.wait_for(lambda: self.health(tid)["answers_at_baud"] == 115200, timeout=30,
                      what="baud scan to find the chip")
        h = self.health(tid)
        self.assertIn("answers at 115200 baud", h["summary"])
        self.assertIn("set to 9600", h["summary"])

    def test_marginal_cable_is_degraded(self):
        tid = self.target_with(FakeChip(fail_every=2))
        h = self.wait_state(tid, "degraded", timeout=45)
        self.assertGreaterEqual(h["last_hour"]["checks_failed"], 1)
        self.assertIn("loose or failing cable", h["summary"])
        self.assertEqual(self.listed(tid)["input_health"], "degraded")

    def test_late_input_acks_are_not_failures(self):
        # The chip acks every input report. An ack arriving after the probe
        # has flushed the line isn't the probe's reply: read as one, it would
        # fail the probe and force a reconnect in the middle of someone's work.
        chip = FakeChip(ack_delay=0.02)
        tid = self.target_with(chip)
        self.wait_state(tid, "good")
        op = self.operator
        self.assertEqual(op.post(f"/api/targets/{tid}/control/acquire")[0], 200)
        self.addCleanup(op.post, f"/api/targets/{tid}/control/release")
        checks = self.health(tid)["last_hour"]["checks_ok"]
        deadline = time.time() + 12                          # spans at least two probes
        i = 0
        while time.time() < deadline:
            i += 1
            op.post(f"/api/targets/{tid}/input", {"type": "mousemove", "x": (i % 100) / 100, "y": 0.5})
            time.sleep(0.01)
        h = self.health(tid)
        self.assertGreaterEqual(h["last_hour"]["checks_ok"], checks + 2)
        self.assertEqual(h["last_hour"]["checks_failed"], 0, h)
        self.assertEqual(h["last_hour"]["reconnects"], 0, h)

    def test_a_slow_chip_never_stalls_the_server(self):
        # A probe holds the serial line until the chip answers. Input waiting
        # for the line mustn't hold the target's lock meanwhile, or every
        # request reading the target's status waits too, and the whole server
        # freezes for as long as the chip takes.
        chip = FakeChip(info_delay=1.0)                      # slow, but within the timeout
        tid = self.target_with(chip)
        self.wait_state(tid, "good")
        op = self.operator
        self.assertEqual(op.post(f"/api/targets/{tid}/control/acquire")[0], 200)
        self.addCleanup(op.post, f"/api/targets/{tid}/control/release")

        stop = threading.Event()
        def type_keys():
            pressed = False
            while not stop.is_set():
                pressed = not pressed
                op.post(f"/api/targets/{tid}/input", {"type": "key", "code": "KeyA", "pressed": pressed})
                time.sleep(0.01)
        typist = threading.Thread(target=type_keys)
        typist.start()
        self.addCleanup(typist.join)
        self.addCleanup(stop.set)

        probes = chip.info_requests
        slowest = 0.0
        deadline = time.time() + 15
        while chip.info_requests < probes + 2 and time.time() < deadline:
            t0 = time.monotonic()
            self.assertEqual(self.owner.get(f"/api/targets/{tid}")[0], 200)
            slowest = max(slowest, time.monotonic() - t0)
            time.sleep(0.02)
        self.assertGreaterEqual(chip.info_requests, probes + 2, "no probes ran")
        self.assertLess(slowest, 0.4, f"a status request took {slowest:.2f} s")

    def test_backends_that_cannot_talk_back(self):
        default = next(t for t in self.owner.as_json(self.owner.get("/api/targets")[1])["targets"]
                       if t["default"])
        # The local USB gadget: none here. Its first check lands once the
        # target has started, which takes longer where ICE gathering is slow.
        h = self.wait_state(default["id"], "down")
        self.assertEqual(h["kind"], "usb-gadget")
        self.assertIn("/dev/hidg0", h["summary"])
        self.assertIsNone(h["test"])
        status, raw, _ = self.owner.post(f"/api/targets/{default['id']}/health/test")
        self.assertEqual(status, 400, raw)

    def test_who_may_see_it(self):
        tid = self.target_with(FakeChip())
        self.assertEqual(self.health(tid, self.viewer)["kind"], "ch9329")
        self.assertEqual(Client(self.server.port).get(f"/api/targets/{tid}/health")[0], 401)
        self.assertEqual(self.owner.get("/api/targets/99999/health")[0], 404)
        self.assertNotIn(FakeChip.__name__, str(self.health(tid)))
        self.assertNotIn("/dev/pts", str(self.health(tid)))    # no device paths


class TestSelfTest(HealthTestBase):
    def test_healthy_chip(self):
        tid = self.target_with(FakeChip())
        t = self.run_test(tid)
        self.assertEqual(t["verdict_state"], "good", t)
        self.assertEqual((t["probes"], t["ok"]), (20, 20))
        self.assertEqual((t["chip_baud"], t["work_mode"], t["serial_mode"]), (9600, 0x80, 0x80))
        self.assertIs(t["target_enumerated"], True)
        self.assertGreater(t["finished_at"], 0)

    def test_marginal_cable(self):
        tid = self.target_with(FakeChip())
        chip = type(self).chips[-1]
        chip.fail_every = 4                                  # loses 5 of the burst's 20
        t = self.run_test(tid)
        chip.fail_every = 0
        self.assertEqual(t["verdict_state"], "degraded", t)
        self.assertLess(t["ok"], t["probes"])
        self.assertIn("of 20 checks failed", t["verdict"])

    def test_target_side(self):
        tid = self.target_with(FakeChip(enumerated=False))
        t = self.run_test(tid)
        self.assertEqual(t["verdict_state"], "down")
        self.assertIn("USB cable to the target", t["verdict"])

    def test_rules(self):
        tid = self.target_with(FakeChip())
        self.wait_state(tid, "good")
        self.assertEqual(self.operator.post(f"/api/targets/{tid}/health/test")[0], 403)

        # Owner-only actions are for a browser session, not an API token.
        status, raw, _ = self.owner.post("/api/tokens", {"label": "collector"})
        bot = Client(self.server.port)
        bot.bearer = self.owner.as_json(raw)["token"]
        self.assertIn(bot.post(f"/api/targets/{tid}/health/test")[0], (401, 403))
        self.assertEqual(bot.get(f"/api/targets/{tid}/health")[0], 200)   # reading is fine

        # Never while someone is driving...
        self.assertEqual(self.operator.post(f"/api/targets/{tid}/control/acquire")[0], 200)
        self.assertEqual(self.owner.post(f"/api/targets/{tid}/health/test")[0], 409)
        self.operator.post(f"/api/targets/{tid}/control/release")

        # ...and nobody takes control, or starts another, while one runs.
        self.assertEqual(self.owner.post(f"/api/targets/{tid}/health/test")[0], 202)
        self.assertEqual(self.operator.post(f"/api/targets/{tid}/control/acquire")[0], 409)
        self.assertEqual(self.owner.post(f"/api/targets/{tid}/health/test")[0], 409)
        self.wait_for(lambda: self.health(tid)["test"]["state"] == "done", timeout=30,
                      what="self-test to finish")
        self.assertEqual(self.operator.post(f"/api/targets/{tid}/control/acquire")[0], 200)
        self.operator.post(f"/api/targets/{tid}/control/release")


class TestBaudChange(HealthTestBase):
    def change_baud(self, tid, baud):
        status, raw, _ = self.owner.post(f"/api/targets/{tid}/health/baud", {"baud": baud})
        self.assertEqual(status, 202, raw)
        self.wait_for(lambda: self.health(tid)["baud_change"]["state"] == "done", timeout=30,
                      what="speed change to finish")
        return self.health(tid)

    def saved_baud(self, tid):
        return self.owner.as_json(self.owner.get(f"/api/targets/{tid}")[1])["config"]["serial_baud"]

    def check_switched(self, chip):
        tid = self.target_with(chip)
        self.wait_state(tid, "good")
        h = self.change_baud(tid, 115200)
        c = h["baud_change"]
        self.assertIs(c["ok"], True, c)
        self.assertEqual((c["from_baud"], c["to_baud"]), (9600, 115200))
        self.assertEqual((h["configured_baud"], h["chip_baud"]), (115200, 115200))
        self.assertEqual((chip.baud, chip.stored), (115200, 115200))
        self.wait_for(lambda: self.saved_baud(tid) == 115200, timeout=10,
                      what="new rate saved on the target")
        # Still answering at the new rate on the next routine checks.
        before = self.health(tid)["last_hour"]["checks_ok"]
        self.wait_for(lambda: self.health(tid)["last_hour"]["checks_ok"] > before, timeout=20,
                      what="a check at the new rate")
        self.assertEqual(self.health(tid)["state"], "good")

    def test_chip_that_switches_at_once(self):
        self.check_switched(FakeChip(baud=9600, applies="now"))

    def test_chip_that_switches_after_a_reset(self):
        self.check_switched(FakeChip(baud=9600, applies="reset"))

    def test_chip_whose_pins_fix_the_rate(self):
        chip = FakeChip(baud=9600, applies="never")
        tid = self.target_with(chip)
        self.wait_state(tid, "good")
        c = self.change_baud(tid, 115200)["baud_change"]
        self.assertIs(c["ok"], False)
        self.assertIn("stayed at 9600 baud", c["message"])
        # The stored setting is put back, and nothing on the target changes.
        self.assertEqual((chip.baud, chip.stored), (9600, 9600))
        self.assertEqual(self.saved_baud(tid), 9600)
        self.assertEqual(self.health(tid)["configured_baud"], 9600)

    def test_rules(self):
        # Slow to take the new rate: with an instant fake chip the change can
        # be over before the next request, which then rightly gets 202, not
        # the 409 a change still running gets.
        tid = self.target_with(FakeChip(baud=9600, config_delay=0.3))
        self.wait_state(tid, "good")
        url = f"/api/targets/{tid}/health/baud"
        self.assertEqual(self.health(tid)["baud_change"], {"state": "idle"})
        self.assertEqual(self.operator.post(url, {"baud": 115200})[0], 403)
        for bad in ({"baud": 250000}, {"baud": "fast"}, {}):
            self.assertEqual(self.owner.post(url, bad)[0], 400, bad)

        self.assertEqual(self.operator.post(f"/api/targets/{tid}/control/acquire")[0], 200)
        self.assertEqual(self.owner.post(url, {"baud": 115200})[0], 409)
        self.operator.post(f"/api/targets/{tid}/control/release")

        self.assertEqual(self.owner.post(url, {"baud": 115200})[0], 202)
        self.assertEqual(self.operator.post(f"/api/targets/{tid}/control/acquire")[0], 409)
        self.assertEqual(self.owner.post(url, {"baud": 115200})[0], 409)
        self.wait_for(lambda: self.health(tid)["baud_change"]["state"] == "done", timeout=30,
                      what="speed change to finish")
        self.assertEqual(self.operator.post(f"/api/targets/{tid}/control/acquire")[0], 200)
        self.operator.post(f"/api/targets/{tid}/control/release")

    def test_backends_without_a_speed(self):
        default = next(t for t in self.owner.as_json(self.owner.get("/api/targets")[1])["targets"]
                       if t["default"])
        self.assertIsNone(self.health(default["id"])["baud_change"])   # the USB gadget
        status, raw, _ = self.owner.post(f"/api/targets/{default['id']}/health/baud", {"baud": 115200})
        self.assertEqual(status, 400, raw)

if __name__ == "__main__":
    unittest.main()
