"""The mouse jiggler (InputQueue::setJiggler): who may turn it on, that doing
so leaves the target's pipeline alone, and where it moves the pointer. A fake
QMP server (test_qemu_input.FakeQmp) records every absolute move, and the
hidden --mouse-jiggle-seconds=1 makes the wait a second instead of a minute.
"""
import os
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import Client, HoustonKVMServer
from test_qemu_input import FakeQmp
from test_targets import OWNER, login

QMP_ABS_MAX = 0x7FFF


def qmp_abs(v):
    """A [0,1] position as QemuInject encodes it."""
    return int(v * QMP_ABS_MAX + 0.5)


def pointer_positions(events):
    """(x, y) of every absolute move, in order."""
    out, x = [], None
    for e in events:
        if e.get("type") != "abs":
            continue
        axis, value = e["data"]["axis"], e["data"]["value"]
        if axis == "x":
            x = value
        elif x is not None:
            out.append((x, value))
    return out


class TestMouseJiggler(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.qmp = FakeQmp()
        cls.server = HoustonKVMServer(extra_args=["--mouse-jiggle-seconds=1"])
        cls.server.start()
        try:
            cls.owner = Client(cls.server.port)
            assert cls.server.setup_owner(cls.owner, *OWNER)[0] == 200
            for name, role in (("op", "operator"), ("viewer", "viewer")):
                assert cls.owner.post("/api/users", {"username": name, "password": f"{name}password12",
                                                     "role": role})[0] == 200
            cls.op = Client(cls.server.port)
            login(cls.op, ("op", "oppassword12"))
            cls.viewer = Client(cls.server.port)
            login(cls.viewer, ("viewer", "viewerpassword12"))
            status, raw, _ = cls.owner.post("/api/targets", {
                "name": "VM", "qmp_socket": cls.qmp.path, "v4l2_device": "/dev/video242", "enabled": True})
            assert status == 201, raw
            cls.tid = cls.owner.as_json(raw)["id"]
            cls.wait_until(lambda: cls.target()["status"]["input_ready"], 20, "QMP input ready")
        except BaseException:
            cls.server.stop()
            cls.qmp.close()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()
        cls.qmp.close()

    def tearDown(self):
        self.set_jiggler(False)

    @classmethod
    def target(cls):
        return cls.owner.as_json(cls.owner.get(f"/api/targets/{cls.tid}")[1])

    @staticmethod
    def wait_until(predicate, timeout, what):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate():
                return
            time.sleep(0.1)
        raise AssertionError(f"timed out waiting for {what}")

    def set_jiggler(self, on, client=None):
        status, raw, _ = (client or self.op).put(f"/api/targets/{self.tid}/input/settings",
                                                 {"mouse_jiggler": on})
        self.assertEqual(status, 200, raw)

    def moves_since(self, mark):
        return pointer_positions(self.qmp.input_events()[mark:])

    def test_off_until_turned_on_and_shown_to_everyone(self):
        self.assertIs(self.target()["mouse_jiggler"], False)
        status, raw, _ = self.op.get(f"/api/targets/{self.tid}/input/settings")
        self.assertEqual((status, self.op.as_json(raw)), (200, {"mouse_jiggler": False}))
        self.set_jiggler(True)
        viewer_view = self.viewer.as_json(self.viewer.get(f"/api/targets/{self.tid}")[1])
        self.assertIs(viewer_view["mouse_jiggler"], True)

    def test_operators_and_owners_only(self):
        status, _, _ = self.viewer.put(f"/api/targets/{self.tid}/input/settings", {"mouse_jiggler": True})
        self.assertEqual(status, 403)
        self.assertEqual(self.viewer.get(f"/api/targets/{self.tid}/input/settings")[0], 403)
        self.set_jiggler(True, client=self.owner)
        # An Owner can set it with the target's other fields too.
        status, raw, _ = self.owner.put(f"/api/targets/{self.tid}", {"mouse_jiggler": False})
        self.assertEqual(status, 200, raw)
        self.assertIs(self.target()["mouse_jiggler"], False)

    def test_rejects_anything_but_a_boolean(self):
        status, _, _ = self.op.put(f"/api/targets/{self.tid}/input/settings", {"mouse_jiggler": "yes"})
        self.assertEqual(status, 400)
        self.assertIs(self.target()["mouse_jiggler"], False)

    def test_turning_it_on_is_audited_and_rebuilds_nothing(self):
        builds = sum("H.264 encoder" in line for line in self.server.output)
        self.assertGreater(builds, 0, "every build of the pipeline logs its encoder")
        self.set_jiggler(True)
        status, raw, _ = self.owner.get(f"/api/audit/events?type=target.input_settings&limit=1")
        self.assertEqual(status, 200, raw)
        event = self.owner.as_json(raw)["events"][0]
        self.assertEqual(event["user"]["username"], "op")
        self.assertEqual(event["detail"]["changes"], {"mouse_jiggler": {"from": False, "to": True}})
        time.sleep(1)
        self.assertEqual(sum("H.264 encoder" in line for line in self.server.output), builds,
                         "turning the jiggler on rebuilt the target's pipeline")

    def test_nudges_the_pointer_from_where_the_driver_left_it_and_back(self):
        self.assertEqual(self.op.post(f"/api/targets/{self.tid}/control/acquire")[0], 200)
        self.assertEqual(self.op.post(f"/api/targets/{self.tid}/input",
                                      {"type": "mousemove", "x": 0.25, "y": 0.75})[0], 200)
        self.op.post(f"/api/targets/{self.tid}/control/release")
        here = (qmp_abs(0.25), qmp_abs(0.75))
        self.wait_until(lambda: here in self.moves_since(0), 5, "the driver's move")
        mark = len(self.qmp.input_events())
        self.set_jiggler(True)
        self.wait_until(lambda: len(self.moves_since(mark)) >= 2, 5, "a jiggle")
        out, back = self.moves_since(mark)[:2]
        # A step toward the middle, a few pixels' worth, then straight back.
        self.assertGreater(out[0], here[0])
        self.assertLess(out[0] - here[0], 200)
        self.assertEqual(out[1], here[1])
        self.assertEqual(back, here)

    def test_never_drags_while_a_button_is_held(self):
        # A move with a button down is a drag: the jiggle waits until it's up.
        self.assertEqual(self.op.post(f"/api/targets/{self.tid}/control/acquire")[0], 200)
        for msg in ({"type": "mousemove", "x": 0.4, "y": 0.4},
                    {"type": "mousebutton", "button": 0, "pressed": True}):
            self.assertEqual(self.op.post(f"/api/targets/{self.tid}/input", msg)[0], 200)
        self.wait_until(lambda: (qmp_abs(0.4), qmp_abs(0.4)) in self.moves_since(0), 5, "the move")
        self.set_jiggler(True)
        mark = len(self.qmp.input_events())
        time.sleep(2.5)
        self.assertEqual(self.moves_since(mark), [], "the jiggler moved the pointer with a button down")
        self.assertEqual(self.op.post(f"/api/targets/{self.tid}/input",
                                      {"type": "mousebutton", "button": 0, "pressed": False})[0], 200)
        self.wait_until(lambda: len(self.moves_since(mark)) >= 2, 5, "a jiggle once the button is up")
        self.op.post(f"/api/targets/{self.tid}/control/release")

    def test_waits_while_the_driver_is_busy_and_stops_when_turned_off(self):
        # Driving first: turned on after a quiet spell, it would jiggle at once.
        self.assertEqual(self.op.post(f"/api/targets/{self.tid}/control/acquire")[0], 200)
        self.assertEqual(self.op.post(f"/api/targets/{self.tid}/input",
                                      {"type": "mousemove", "x": 0.05, "y": 0.5})[0], 200)
        self.wait_until(lambda: (qmp_abs(0.05), qmp_abs(0.5)) in self.moves_since(0), 5, "the first move")
        self.set_jiggler(True)
        mark = len(self.qmp.input_events())
        sent = []
        for i in range(10):   # input every 0.3 s for 3 s: never a quiet second
            pos = (0.1 + i * 0.05, 0.5)
            sent.append((qmp_abs(pos[0]), qmp_abs(pos[1])))
            self.assertEqual(self.op.post(f"/api/targets/{self.tid}/input",
                                          {"type": "mousemove", "x": pos[0], "y": pos[1]})[0], 200)
            time.sleep(0.3)
        self.op.post(f"/api/targets/{self.tid}/control/release")
        time.sleep(0.2)
        self.assertEqual(self.moves_since(mark), sent)

        self.set_jiggler(False)
        mark = len(self.qmp.input_events())
        time.sleep(2.5)
        self.assertEqual(self.moves_since(mark), [])


if __name__ == "__main__":
    unittest.main()
