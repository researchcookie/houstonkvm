"""
test_webrtc_signaling.py — low-latency (WebRTC) video must never hold up the
rest of the server, and a target only encodes while someone watches it.

TestSlowGathering runs on any build. TestEncodeOnDemand drives a real WebRTC
viewer with HoustonKVM-loadgen and a synthetic video source, so it needs a
server built with -DHOUSTONKVM_LOADTEST=ON (the dev build and CI build it that
way) and skips itself otherwise:

    cmake -B build-loadtest -DHOUSTONKVM_LOADTEST=ON && cmake --build build-loadtest
    HOUSTONKVM_BINARY=build-loadtest/HoustonKVM python3 -m unittest tests/integration/test_webrtc_signaling.py
"""
import json
import os
import subprocess
import sys
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import Client, HoustonKVMServer, has_route_to, ice_gives_up_by_itself  # noqa: E402

OWNER = ("owner", "ownerpassword1")
# TEST-NET-1 (RFC 5737): routed nowhere, so a STUN request to it is never
# answered and ICE gathering waits for as long as it's allowed to.
UNREACHABLE_STUN = "stun:192.0.2.1:3478"


class SignalingTestCase(unittest.TestCase):
    extra_args = ()
    https = None   # see HoustonKVMServer

    @classmethod
    def setUpClass(cls):
        cls.server = HoustonKVMServer(extra_args=cls.extra_args, https=cls.https)
        cls.server.start()
        # unittest skips tearDownClass when setUpClass raises (a skip
        # included), which would leave the server running.
        try:
            bootstrap = Client(cls.server.port)
            status, raw, _ = cls.server.setup_owner(bootstrap, *OWNER)
            assert status == 200, raw
            cls.owner = Client(cls.server.port)
            status, raw, _ = cls.owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})
            assert status == 200, raw
            cls.setUpServer()
        except BaseException:
            cls.server.stop()
            raise

    @classmethod
    def setUpServer(cls):
        pass

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    @staticmethod
    def wait_until(cond, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if cond():
                return True
            time.sleep(0.2)
        return cond()

    @classmethod
    def target(cls, tid):
        status, raw, _ = cls.owner.get("/api/targets")
        assert status == 200, raw
        return next(t for t in Client.as_json(raw)["targets"] if t["id"] == tid)

    def subscribe(self, tid, query=""):
        """(status, body, seconds taken)."""
        t0 = time.monotonic()
        status, raw, _ = self.owner.post(f"/api/targets/{tid}/webrtc/subscribe{query}")
        return status, raw, time.monotonic() - t0


class TestSlowGathering(SignalingTestCase):
    extra_args = (f"--ice-server={UNREACHABLE_STUN}",)

    @classmethod
    def setUpServer(cls):
        status, raw, _ = cls.owner.get("/api/targets")
        cls.tid = next(t["id"] for t in Client.as_json(raw)["targets"] if t["default"])
        # With loopback as the only interface (a mock/rpmbuild chroot) there
        # are no host candidates, and libdatachannel 0.19 fails gathering
        # outright once a STUN server is set: no publisher, nothing to test.
        m = cls.server.wait_for_output(r"SfuPublisher: (connecting to Sfu as|Sfu produced no offer for) \[target\]")
        if "no offer" in m.group(0):
            raise unittest.SkipTest("ICE gathering can't work here (no network interface but loopback)")

    def test_subscribing_does_not_hold_up_other_requests(self):
        if not has_route_to("192.0.2.1", 3478):
            self.skipTest("no route to the test STUN address here (only loopback), so it can't go unanswered")
        result = {}
        subscriber = threading.Thread(target=lambda: result.update(r=self.subscribe(self.tid)))
        subscriber.start()
        time.sleep(0.2)   # well into the subscription's gathering
        worst = 0.0
        while subscriber.is_alive():
            t0 = time.monotonic()
            status, _, _ = Client(self.server.port).get("/api/status")
            worst = max(worst, time.monotonic() - t0)
            self.assertEqual(status, 200)
            time.sleep(0.05)
        subscriber.join()
        status, raw, took = result["r"]
        if took < 1:
            self.skipTest(f"gathering wasn't slow here ({took:.2f} s, status {status}); nothing to measure")
        # Other requests don't wait for the gathering.
        self.assertLess(worst, 0.5, f"/api/status took {worst:.2f} s while a subscription gathered")
        # With libjuice, gathering never completes against an unreachable
        # STUN server: the server goes ahead after 2 s with its own addresses,
        # which is all a viewer on the LAN needs. libnice stops waiting by
        # itself at about the same time.
        self.assertEqual(status, 200, raw)
        self.assertIn("a=candidate:", Client.as_json(raw)["sdp"])
        self.assertLess(took, 4)
        if not ice_gives_up_by_itself(self.server.binary):
            self.server.wait_for_output(r"going ahead with \d+ ICE candidate\(s\)")

    def test_candidates_on_answer_needs_no_gathering_to_offer(self):
        # The offer leaves out the server's candidates, so it doesn't wait for
        # them, and an unreachable STUN server doesn't delay it.
        status, raw, took = self.subscribe(self.tid, "?candidates=on-answer")
        self.assertEqual(status, 200, raw)
        self.assertLess(took, 1)
        self.assertNotIn("a=candidate:", Client.as_json(raw)["sdp"])

    def test_a_viewer_who_hangs_up_while_waiting(self):
        sock = self.owner.connect()
        sock.sendall(f"POST /api/targets/{self.tid}/webrtc/subscribe HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                     f"{self.owner.cookie_header()}Content-Length: 0\r\n\r\n".encode())
        time.sleep(0.3)
        sock.close()
        # The offer, when it comes, has nobody to go to; the server carries on.
        time.sleep(6)
        self.assertEqual(self.owner.get("/api/status")[0], 200)
        self.assertIsNone(self.server.proc.poll())

    def test_bad_ice_server_option(self):
        out = subprocess.run([str(self.server.binary), "--ice-server=example.com"],
                             capture_output=True, text=True, timeout=10)
        self.assertEqual(out.returncode, 2)
        self.assertIn("Invalid ICE server", out.stderr)
        out = subprocess.run([str(self.server.binary), "--ice-server=none", f"--ice-server={UNREACHABLE_STUN}"],
                             capture_output=True, text=True, timeout=10)
        self.assertEqual(out.returncode, 2)
        self.assertIn("can't be combined", out.stderr)


class TestEncodeOnDemand(SignalingTestCase):
    # No STUN: connecting on loopback needs nothing else, and the test mustn't
    # depend on reaching the Internet.
    extra_args = ("--ice-server=none",)

    @classmethod
    def setUpServer(cls):
        cls.loadgen = Path(cls.server.binary).with_name("HoustonKVM-loadgen")
        cls.tls_flag = ["--tls"] if cls.server.tls else []
        if not cls.loadgen.exists():
            raise unittest.SkipTest("no HoustonKVM-loadgen next to the server (build with -DHOUSTONKVM_LOADTEST=ON)")
        status, raw, _ = cls.owner.post("/api/tokens", {"label": "webrtc-test"})
        assert status in (200, 201), raw
        cls.token = Client.as_json(raw)["token"]
        status, raw, _ = cls.owner.post("/api/targets", {
            "name": "synthetic", "enabled": True, "v4l2_device": "synthetic:desktop/1",
            "capture_width": 320, "capture_height": 240, "capture_fps": 30,
            "qmp_socket": os.path.join(cls.server.tmpdir.name, "no-qemu.sock"),
        })
        assert status == 201, raw
        cls.tid = Client.as_json(raw)["id"]
        if not cls.wait_until(lambda: cls.target(cls.tid)["status"]["capture_active"], timeout=5):
            raise unittest.SkipTest("this server has no synthetic capture (build with -DHOUSTONKVM_LOADTEST=ON)")

    # The encoder's state changes are logged, and flushed: other stdout
    # lines reach a pipe in blocks, too late to tell when they happened.
    def started_encoding(self):
        return sum("WebRTC viewer watching, encoding" in l for l in self.server.output)

    def stopped_encoding(self):
        return sum("no WebRTC viewers, encoder idle" in l for l in self.server.output)

    def watch(self, seconds, *extra):
        """One WebRTC viewer for `seconds`; returns loadgen's report of it."""
        out = subprocess.run(
            [str(self.loadgen), f"--port={self.server.port}", f"--token={self.token}", *self.tls_flag,
             f"--target={self.tid}", "--webrtc=1", "--warmup=2", f"--seconds={seconds}", *extra],
            capture_output=True, text=True, timeout=seconds + 30)
        self.assertEqual(out.returncode, 0, out.stderr)
        return json.loads(out.stdout.strip().splitlines()[-1])["webrtc"][0]

    def test_encodes_only_while_watched(self):
        # Nobody watching (any viewer of another test in this class has
        # left): capture runs, the encoder doesn't.
        self.assertTrue(self.wait_until(lambda: self.target(self.tid)["status"]["webrtc_viewers"] == 0
                                        and self.started_encoding() == self.stopped_encoding(), timeout=15))
        started, stopped = self.started_encoding(), self.stopped_encoding()
        time.sleep(2)
        self.assertEqual(self.started_encoding(), started)

        seen = []
        poller = threading.Thread(target=lambda: self.wait_until(
            lambda: seen.append(self.target(self.tid)["status"]["webrtc_viewers"]) or seen[-1] == 1,
            timeout=15))
        poller.start()
        viewer = self.watch(3)
        poller.join()
        self.assertTrue(viewer["connected"], viewer)
        self.assertGreater(viewer["fps"], 20, viewer)
        self.assertIn(1, seen)
        self.assertEqual(self.started_encoding(), started + 1)

        # The viewer is gone: the encoder stops, and stays stopped.
        self.assertTrue(self.wait_until(lambda: self.stopped_encoding() == stopped + 1, timeout=15))
        self.assertTrue(self.wait_until(lambda: self.target(self.tid)["status"]["webrtc_viewers"] == 0,
                                        timeout=5))
        time.sleep(2)
        self.assertEqual((self.started_encoding(), self.stopped_encoding()), (started + 1, stopped + 1))

    def test_a_late_viewer_gets_a_keyframe_at_once(self):
        def viewer(seconds):
            return subprocess.Popen(
                [str(self.loadgen), f"--port={self.server.port}", f"--token={self.token}", *self.tls_flag,
                 f"--target={self.tid}", "--webrtc=1", "--warmup=1", f"--seconds={seconds}"],
                stdout=subprocess.PIPE, text=True)
        def report(proc):
            out, _ = proc.communicate(timeout=60)
            self.assertEqual(proc.returncode, 0)
            return json.loads(out.strip().splitlines()[-1])["webrtc"][0]

        first = viewer(9)
        time.sleep(4)
        second = report(viewer(3))
        first = report(first)
        # A browser shows nothing until its first keyframe. Before, the
        # request for one was lost and a late viewer waited for the encoder's
        # own, up to 10 s.
        self.assertGreater(second["first_keyframe_ms"], 0, second)
        self.assertLess(second["first_keyframe_ms"], 1500, second)
        # Keyframes on demand only, not one a second: over 10 s the first
        # viewer sees its own, the second viewer's (and a follow-up 2 s later).
        self.assertLessEqual(first["keyframes"], 4, first)

    def test_the_offer_carries_candidates_only_when_asked_to(self):
        status, raw, _ = self.subscribe(self.tid)
        self.assertEqual(status, 200, raw)
        self.assertIn("a=candidate:", Client.as_json(raw)["sdp"])
        status, raw, _ = self.subscribe(self.tid, "?candidates=on-answer")
        self.assertEqual(status, 200, raw)
        self.assertNotIn("a=candidate:", Client.as_json(raw)["sdp"])

    def test_the_original_signaling_still_connects(self):
        # loadgen uses ?candidates=on-answer unless told otherwise, as the
        # web UI does; clients written against the original flow still work.
        viewer = self.watch(2, "--legacy-signaling")
        self.assertTrue(viewer["connected"], viewer)
        self.assertGreater(viewer["fps"], 20, viewer)

    def test_an_unanswered_subscription_is_closed(self):
        status, raw, _ = self.subscribe(self.tid)
        self.assertEqual(status, 200, raw)
        sub_id = Client.as_json(raw)["subscriberId"]
        self.server.wait_for_output(rf"sub \[{sub_id}\] never connected — closed", timeout=40)
        status, raw, _ = self.owner.post(f"/api/targets/{self.tid}/webrtc/answer",
                                         {"subscriberId": sub_id, "sdp": "v=0"})
        self.assertEqual(status, 404, raw)


if __name__ == "__main__":
    unittest.main()
