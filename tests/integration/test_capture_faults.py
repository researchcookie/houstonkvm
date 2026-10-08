"""
test_capture_faults.py — what a target's video does when its capture device
goes away and comes back, using the synthetic source's "unplugged" and
"nosignal" files (see include/video/synthetic_capture.h) in place of pulling
a real dongle or its HDMI cable.

Needs a server built with -DHOUSTONKVM_LOADTEST=ON (the dev build and CI
build it that way; packages never do), and skips itself otherwise:

    cmake -B build-loadtest -DHOUSTONKVM_LOADTEST=ON && cmake --build build-loadtest
    HOUSTONKVM_BINARY=build-loadtest/HoustonKVM python3 -m unittest tests/integration/test_capture_faults.py
"""
import json
import os
import re
import socket
import threading
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import Client, ServerTestCase  # noqa: E402

OWNER = ("owner", "ownerpassword1")


class TestSyntheticUnplug(ServerTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        # unittest skips tearDownClass when setUpClass raises (a skip
        # included), which would leave the server running.
        try:
            cls._setUpServer()
        except BaseException:
            cls.server.stop()
            raise

    @classmethod
    def _setUpServer(cls):
        bootstrap = Client(cls.server.port)
        status, raw, _ = cls.server.setup_owner(bootstrap, *OWNER)
        assert status == 200, raw
        cls.owner = Client(cls.server.port)
        status, raw, _ = cls.owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})
        assert status == 200, raw
        cls.count = 0
        cls.device = {}

        probe = cls.make_target("")
        if not cls.wait_until(lambda: cls.capture_active(probe), timeout=5):
            raise unittest.SkipTest("this server has no synthetic capture (build with -DHOUSTONKVM_LOADTEST=ON)")

    @classmethod
    def make_target(cls, option):
        cls.count += 1
        n = cls.count
        status, raw, _ = cls.owner.post("/api/targets", {
            "name": f"synthetic-{n}", "enabled": True,
            "v4l2_device": f"synthetic:desktop/{n}{option}",
            "capture_width": 320, "capture_height": 240, "capture_fps": 30,
            # An input backend that never touches hardware.
            "qmp_socket": os.path.join(cls.server.tmpdir.name, f"no-qemu-{n}.sock"),
        })
        assert status == 201, raw
        tid = Client.as_json(raw)["id"]
        cls.device[tid] = f"synthetic:desktop/{n}"
        return tid

    @classmethod
    def target(cls, tid):
        status, raw, _ = cls.owner.get("/api/targets")
        assert status == 200, raw
        return next(t for t in Client.as_json(raw)["targets"] if t["id"] == tid)

    @classmethod
    def capture_active(cls, tid):
        return cls.target(tid)["status"]["capture_active"]

    @classmethod
    def video(cls, tid):
        status, raw, _ = cls.owner.get(f"/api/targets/{tid}/health")
        assert status == 200, raw
        return Client.as_json(raw)["video"]

    def wait_for_video(self, tid, state, timeout):
        seen = []
        ok = self.wait_until(lambda: seen.append(self.video(tid)) or seen[-1]["state"] == state, timeout)
        self.assertTrue(ok, f"video never became {state!r}; last: {seen[-1]}")
        return seen[-1]

    @staticmethod
    def wait_until(cond, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if cond():
                return True
            time.sleep(0.2)
        return cond()

    def frames_received(self, tid, seconds):
        """How many MJPEG frames one viewer of the target's stream gets in `seconds`."""
        sock = self.owner.connect(timeout=seconds)
        try:
            sock.sendall(f"GET /api/targets/{tid}/stream HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                         f"{self.owner.cookie_header()}\r\n".encode())
            data = b""
            deadline = time.time() + seconds
            while (left := deadline - time.time()) > 0:
                sock.settimeout(left)
                try:
                    chunk = sock.recv(65536)
                except socket.timeout:
                    break
                if not chunk:
                    break
                data += chunk
            self.assertTrue(data.startswith(b"HTTP/1.1 200"), data[:80])
            return data.count(b"--frame")
        finally:
            sock.close()

    def unplug_file(self, name):
        path = Path(self.server.tmpdir.name) / name
        self.addCleanup(lambda: path.unlink(missing_ok=True))
        return path

    def test_video_flows_while_plugged_in(self):
        tid = self.make_target(f"?unplugged={self.unplug_file('never')}")
        self.assertTrue(self.wait_until(lambda: self.capture_active(tid), timeout=5))
        self.assertGreater(self.frames_received(tid, 1.5), 10)

    def test_unplugged_at_start_is_absent_until_plugged_in(self):
        flag = self.unplug_file("absent")
        flag.touch()
        tid = self.make_target(f"?unplugged={flag}")
        dev = re.escape(self.device[tid])
        self.server.wait_for_output(rf"cannot open {dev}\?unplugged=.*trying again in 1 s")
        video = self.wait_for_video(tid, "absent", timeout=5)
        self.assertFalse(self.capture_active(tid))
        self.assertEqual(self.target(tid)["status"]["video_state"], "absent")
        self.assertIsNotNone(video["retry_in_ms"])
        # Health is for anyone who can see the target: no device paths.
        self.assertNotIn(self.server.tmpdir.name, json.dumps(video))
        self.assertIn("the capture device", video["last_error"])

        flag.unlink()
        # Retries are 1, 2, 4 s apart: it's back within the next one.
        self.wait_for_video(tid, "good", timeout=10)
        self.assertTrue(self.capture_active(tid))
        self.assertGreater(self.frames_received(tid, 1.5), 10)

    def test_pulling_it_out_and_back_in(self):
        flag = self.unplug_file("pulled")
        tid = self.make_target(f"?unplugged={flag}")
        self.assertEqual(self.wait_for_video(tid, "good", timeout=5)["reconnects"], 0)

        # A viewer who stays connected through it all.
        frames = []
        stop = time.time() + 12
        def watch():
            sock = self.owner.connect(timeout=2)
            sock.sendall(f"GET /api/targets/{tid}/stream HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                         f"{self.owner.cookie_header()}\r\n".encode())
            while time.time() < stop:
                try:
                    chunk = sock.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                frames.extend([time.time()] * chunk.count(b"--frame"))
            sock.close()
        viewer = threading.Thread(target=watch)
        viewer.start()

        time.sleep(1)
        flag.touch()
        dev = re.escape(self.device[tid])
        self.server.wait_for_output(rf"lost {dev}\?.*VIDIOC_DQBUF: No such device.*reconnecting")
        video = self.wait_for_video(tid, "reconnecting", timeout=5)
        self.assertFalse(self.capture_active(tid))
        self.assertEqual(self.target(tid)["status"]["video_state"], "reconnecting")
        time.sleep(2)
        pulled_out = time.time()
        flag.unlink()
        video = self.wait_for_video(tid, "good", timeout=10)
        back = time.time()
        self.assertEqual(video["reconnects"], 1)
        self.assertIn("No such device", video["last_error"])
        self.server.wait_for_output(rf"{dev}\?\S* is back \(reconnect 1\)")
        viewer.join()

        # The same connection carried on once the device was back.
        self.assertTrue(any(t > back for t in frames), "the open stream got no frames after the reconnect")
        self.assertLess(back - pulled_out, 6)

    def test_retries_back_off(self):
        flag = self.unplug_file("backoff")
        flag.touch()
        tid = self.make_target(f"?unplugged={flag}")
        name = re.escape(self.device[tid]) + r"\?unplugged=\S*"
        for wait in (1, 2, 4):
            self.server.wait_for_output(rf"cannot open {name}.*trying again in {wait} s", timeout=10)
        self.assertLessEqual(self.video(tid)["retry_in_ms"], 8000)

    def test_no_signal(self):
        flag = self.unplug_file("nosignal")
        tid = self.make_target(f"?nosignal={flag}")
        self.wait_for_video(tid, "good", timeout=5)
        flag.touch()
        self.wait_for_video(tid, "no_signal", timeout=6)
        # Still open: nothing to reconnect.
        self.assertTrue(self.capture_active(tid))
        self.assertEqual(self.video(tid)["reconnects"], 0)
        flag.unlink()
        self.wait_for_video(tid, "good", timeout=3)

    def test_a_dongle_that_resumes_by_itself_is_not_restarted(self):
        flag = self.unplug_file("resumes")
        tid = self.make_target(f"?nosignal={flag}")
        self.wait_for_video(tid, "good", timeout=5)
        flag.touch()
        self.wait_for_video(tid, "no_signal", timeout=6)
        flag.unlink()
        self.wait_for_video(tid, "good", timeout=3)
        name = re.escape(self.device[tid])
        self.assertFalse(any(re.search(rf"{name}\S* still sends no picture", line)
                             for line in self.server.output))

    def test_a_stalled_dongle_is_restarted_once_its_cable_is_back(self):
        # Like a dongle that stops streaming when its HDMI cable is pulled
        # and stays stopped after it's plugged back in.
        flag = self.unplug_file("stall")
        tid = self.make_target(f"?stall={flag}")
        self.wait_for_video(tid, "good", timeout=5)
        flag.touch()
        self.wait_for_video(tid, "no_signal", timeout=6)
        flag.unlink()                      # the cable is back...
        time.sleep(1.5)
        self.assertEqual(self.video(tid)["state"], "no_signal")   # ...but the dongle won't say so
        name = re.escape(self.device[tid])
        # Restarted about 10 s after the picture went: and it's back.
        self.server.wait_for_output(rf"{name}\S* still sends no picture; restarting it "
                                    r"\(next in 20 s", timeout=15)
        self.wait_for_video(tid, "good", timeout=5)
        self.server.wait_for_output(rf"{name}\S* has a picture again after restarting it", timeout=2)
        self.assertEqual(self.video(tid)["reconnects"], 0)   # it never went away
        self.assertGreater(self.frames_received(tid, 1), 10)

    def test_a_dark_target_stays_no_signal_while_it_is_retried(self):
        # The cable stays out (or the target is off): restarting changes
        # nothing anyone watching sees.
        flag = self.unplug_file("dark")
        tid = self.make_target(f"?stall={flag}")
        self.wait_for_video(tid, "good", timeout=5)
        flag.touch()
        self.wait_for_video(tid, "no_signal", timeout=6)
        name = re.escape(self.device[tid])
        self.server.wait_for_output(rf"{name}\S* still sends no picture; restarting it", timeout=15)
        seen = set()
        self.wait_until(lambda: seen.add(self.video(tid)["state"]) and False, timeout=3)
        self.assertEqual(seen, {"no_signal"})
        self.assertTrue(self.capture_active(tid))

    def test_other_targets_carry_on(self):
        flag = self.unplug_file("neighbour")
        faulty = self.make_target(f"?unplugged={flag}")
        healthy = self.make_target("")
        self.wait_for_video(faulty, "good", timeout=5)
        self.wait_for_video(healthy, "good", timeout=5)
        before = self.frames_received(healthy, 2)
        flag.touch()
        self.wait_for_video(faulty, "reconnecting", timeout=5)
        during = self.frames_received(healthy, 2)
        flag.unlink()
        self.wait_for_video(faulty, "good", timeout=10)
        # 30 fps for 2 s: within a few frames of each other.
        self.assertGreater(during, before * 0.8, (before, during))


if __name__ == "__main__":
    unittest.main()
