"""
test_audio.py — a target's sound: where it comes from (audio_device), that it
reaches a WebRTC viewer as Opus, and only while someone watches.

The sound itself needs a server built with -DHOUSTONKVM_LOADTEST=ON for the
"synthetic:tone" source and HoustonKVM-loadgen (the dev build and CI build it
that way); those tests skip themselves otherwise. The settings tests run on
any build.
"""
import json
import os
import subprocess
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import Client  # noqa: E402
from test_webrtc_signaling import SignalingTestCase  # noqa: E402


class AudioTestCase(SignalingTestCase):
    extra_args = ("--ice-server=none",)
    _seq = 0

    @classmethod
    def new_target(cls, audio_device, **extra):
        AudioTestCase._seq += 1
        n = AudioTestCase._seq
        body = {
            "name": f"sound-{n}", "enabled": True, "v4l2_device": f"synthetic:desktop/{n}",
            "capture_width": 320, "capture_height": 240, "capture_fps": 30,
            "qmp_socket": os.path.join(cls.server.tmpdir.name, f"no-qemu-{n}.sock"),
            "audio_device": audio_device,
        }
        body.update(extra)
        return cls.owner.post("/api/targets", body)

    def audio_state(self, tid):
        return self.target(tid)["status"]["audio_state"]


class TestAudioSettings(AudioTestCase):
    def test_audio_device_defaults_to_auto_and_round_trips(self):
        status, raw, _ = self.owner.post("/api/targets", {
            "name": "defaults", "v4l2_device": "/dev/video97", "enabled": False,
            "qmp_socket": os.path.join(self.server.tmpdir.name, "defaults.sock")})
        self.assertEqual(status, 201, raw)
        tid = Client.as_json(raw)["id"]
        self.assertEqual(self.target(tid)["config"]["audio_device"], "auto")
        for value in ("", "plughw:CARD=Video,DEV=0", "hw:1,0", "sysdefault:CARD=Video", "auto"):
            status, raw, _ = self.owner.put(f"/api/targets/{tid}", {"audio_device": value})
            self.assertEqual(status, 200, raw)
            self.assertEqual(self.target(tid)["config"]["audio_device"], value)

    def test_only_plain_alsa_devices_are_accepted(self):
        status, raw, _ = self.owner.post("/api/targets", {
            "name": "strict", "v4l2_device": "/dev/video96", "enabled": False,
            "qmp_socket": os.path.join(self.server.tmpdir.name, "strict.sock")})
        tid = Client.as_json(raw)["id"]
        # ALSA plugins can read and write files; a device name must not name one.
        for bad in ("file:'/tmp/x'", "plughw:CARD=Video;rm", "default", "pulse", "hw:",
                    "plughw:" + "x" * 130, 5):
            status, raw, _ = self.owner.put(f"/api/targets/{tid}", {"audio_device": bad})
            self.assertEqual(status, 400, (bad, raw))
        self.assertEqual(self.target(tid)["config"]["audio_device"], "auto")

    def test_two_targets_cannot_share_a_sound_device(self):
        a = self.owner.post("/api/targets", {
            "name": "share-a", "v4l2_device": "/dev/video95", "enabled": False,
            "qmp_socket": os.path.join(self.server.tmpdir.name, "a.sock"),
            "audio_device": "plughw:CARD=Shared,DEV=0"})
        self.assertEqual(a[0], 201, a[1])
        status, raw, _ = self.owner.post("/api/targets", {
            "name": "share-b", "v4l2_device": "/dev/video94", "enabled": False,
            "qmp_socket": os.path.join(self.server.tmpdir.name, "b.sock"),
            "audio_device": "plughw:CARD=Shared,DEV=0"})
        self.assertEqual((status, raw), (409, b"That sound device is already used by another target"))
        # "auto" and "" aren't devices: any number of targets may use them.
        for name, dev in (("auto-a", "/dev/video93"), ("auto-b", "/dev/video92")):
            status, raw, _ = self.owner.post("/api/targets", {
                "name": name, "v4l2_device": dev, "enabled": False,
                "qmp_socket": os.path.join(self.server.tmpdir.name, name + ".sock")})
            self.assertEqual(status, 201, raw)

    def test_the_device_list_offers_sound_cards(self):
        status, raw, _ = self.owner.get("/api/admin/devices")
        self.assertEqual(status, 200, raw)
        body = Client.as_json(raw)
        self.assertIsInstance(body["audio_devices"], list)
        self.assertIsInstance(body["audio_supported"], bool)
        for d in body["audio_devices"]:
            self.assertTrue(d["device"].startswith("plughw:CARD="), d)
            self.assertIn("in_use_by", d)


class TestSound(AudioTestCase):
    @classmethod
    def setUpServer(cls):
        cls.loadgen = Path(cls.server.binary).with_name("HoustonKVM-loadgen")
        cls.tls_flag = ["--tls"] if cls.server.tls else []
        if not cls.loadgen.exists():
            raise unittest.SkipTest("no HoustonKVM-loadgen next to the server (build with -DHOUSTONKVM_LOADTEST=ON)")
        status, raw, _ = cls.owner.post("/api/tokens", {"label": "audio-test"})
        assert status in (200, 201), raw
        cls.token = Client.as_json(raw)["token"]
        status, raw, _ = cls.new_target("synthetic:tone/1")
        if status == 400:
            raise unittest.SkipTest("this server has no synthetic sound (build with -DHOUSTONKVM_LOADTEST=ON)")
        assert status == 201, raw
        cls.tone = Client.as_json(raw)["id"]
        status, raw, _ = cls.new_target("")
        assert status == 201, raw
        cls.silent = Client.as_json(raw)["id"]
        status, raw, _ = cls.new_target("auto")
        assert status == 201, raw
        cls.auto = Client.as_json(raw)["id"]
        for tid in (cls.tone, cls.silent, cls.auto):
            if not cls.wait_until(lambda: cls.target(tid)["status"]["capture_active"], timeout=5):
                raise unittest.SkipTest("this server has no synthetic capture (build with -DHOUSTONKVM_LOADTEST=ON)")
        if cls.target(cls.tone)["status"]["audio_state"] == "unsupported":
            raise unittest.SkipTest("this server was built without sound (-DHOUSTONKVM_AUDIO=OFF)")

    def watch(self, tid, seconds, on_start=None):
        """One WebRTC viewer on `tid` for `seconds`; loadgen's report of it."""
        proc = subprocess.Popen(
            [str(self.loadgen), f"--port={self.server.port}", f"--token={self.token}", *self.tls_flag,
             f"--target={tid}", "--webrtc=1", "--warmup=2", f"--seconds={seconds}"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if on_start:
            on_start()
        out, err = proc.communicate(timeout=seconds + 30)
        self.assertEqual(proc.returncode, 0, err)
        return json.loads(out.strip().splitlines()[-1])["webrtc"][0]

    def test_sound_reaches_a_webrtc_viewer_only_while_watched(self):
        self.assertTrue(self.wait_until(lambda: self.audio_state(self.tone) == "idle", timeout=10),
                        self.audio_state(self.tone))
        seen = []
        viewer = self.watch(self.tone, 3, on_start=lambda: self.wait_until(
            lambda: seen.append(self.audio_state(self.tone)) or seen[-1] == "good", timeout=15))
        self.assertTrue(viewer["connected"], viewer)
        # 20 ms Opus frames: 50 a second.
        self.assertGreater(viewer["audio_packets"], 3 * 50 * 0.8, viewer)
        self.assertLess(viewer["audio_packets"], 3 * 50 * 1.2, viewer)
        self.assertGreater(viewer["audio_kbps"], 30, viewer)    # a tone, not silence
        self.assertIn("good", seen)
        self.assertTrue(self.wait_until(lambda: self.audio_state(self.tone) == "idle", timeout=10))

    def test_a_target_with_sound_off_sends_none(self):
        self.assertEqual(self.audio_state(self.silent), "off")
        viewer = self.watch(self.silent, 2)
        self.assertTrue(viewer["connected"], viewer)
        self.assertGreater(viewer["frames"], 0, viewer)
        self.assertEqual(viewer["audio_packets"], 0, viewer)

    def test_auto_without_a_sound_card_says_why(self):
        self.assertTrue(self.wait_until(lambda: self.audio_state(self.auto) == "absent", timeout=10),
                        self.audio_state(self.auto))
        status, raw, _ = self.owner.get(f"/api/targets/{self.auto}/health")
        self.assertEqual(status, 200, raw)
        audio = Client.as_json(raw)["audio"]
        self.assertEqual(audio, {"state": "absent",
                                 "last_error": "A synthetic capture device has no sound card."})
        viewer = self.watch(self.auto, 2)
        self.assertTrue(viewer["connected"], viewer)   # video carries on regardless
        self.assertEqual(viewer["audio_packets"], 0, viewer)

    def test_turning_sound_on_and_off_takes_effect(self):
        status, raw, _ = self.owner.put(f"/api/targets/{self.silent}", {"audio_device": "synthetic:tone/2"})
        self.assertEqual(status, 200, raw)
        self.addCleanup(self.owner.put, f"/api/targets/{self.silent}", {"audio_device": ""})
        self.assertTrue(self.wait_until(lambda: self.audio_state(self.silent) == "idle", timeout=20))
        self.assertTrue(self.wait_until(lambda: self.target(self.silent)["status"]["capture_active"], timeout=20))
        viewer = self.watch(self.silent, 2)
        self.assertGreater(viewer["audio_packets"], 2 * 50 * 0.8, viewer)


if __name__ == "__main__":
    unittest.main()
