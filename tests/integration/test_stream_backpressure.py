"""
test_stream_backpressure.py — a viewer that can't keep up with /api/stream
skips frames instead of having them queued for it (Broadcaster::hasUnsentData).
A queue only grows on a slow link, and every frame in it is added delay: a
viewer that stalls for a few seconds should see the current picture when it
catches up, not those seconds replayed.

The viewer here stops reading for a few seconds with a small receive buffer,
then reads what had piled up. Skipping leaves a frame or two in flight;
queueing would leave every frame sent during the stall.

Needs a server built with -DHOUSTONKVM_LOADTEST=ON for its synthetic capture
(see test_capture_faults.py), and skips itself otherwise.
"""
import os
import socket
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import Client, ServerTestCase  # noqa: E402

OWNER = ("owner", "ownerpassword1")
FPS = 5           # frames far enough apart to tell the backlog from new ones
STALL = 4         # seconds the viewer stops reading: 20 frames at FPS


class TestSlowViewer(ServerTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        try:   # unittest skips tearDownClass when setUpClass raises
            cls._setUpServer()
        except BaseException:
            cls.server.stop()
            raise

    @classmethod
    def _setUpServer(cls):
        status, raw, _ = cls.server.setup_owner(Client(cls.server.port, tls=cls.server.tls), *OWNER)
        assert status == 200, raw
        cls.owner = Client(cls.server.port, tls=cls.server.tls)
        status, raw, _ = cls.owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})
        assert status == 200, raw
        status, raw, _ = cls.owner.post("/api/targets", {
            "name": "synthetic", "enabled": True,
            "v4l2_device": "synthetic:desktop/backpressure",
            "capture_width": 1280, "capture_height": 720, "capture_fps": FPS,
            # An input backend that never touches hardware.
            "qmp_socket": os.path.join(cls.server.tmpdir.name, "no-qemu.sock"),
        })
        assert status == 201, raw
        cls.tid = Client.as_json(raw)["id"]
        deadline = time.time() + 5
        while cls.owner.get(f"/api/targets/{cls.tid}/snapshot")[0] != 200:
            if time.time() > deadline:
                raise unittest.SkipTest("this server has no synthetic capture (build with -DHOUSTONKVM_LOADTEST=ON)")
            time.sleep(0.2)

    def open_stream(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 * 1024)   # before connect, to cap the window
        sock.connect(("127.0.0.1", self.server.port))
        sock = self.owner.wrap(sock)
        sock.sendall((f"GET /api/targets/{self.tid}/stream HTTP/1.1\r\n"
                      f"Host: 127.0.0.1:{self.server.port}\r\n"
                      f"{self.owner.cookie_header()}\r\n").encode())
        return sock

    def read_until(self, sock, marker, timeout=2.0):
        data = b""
        sock.settimeout(timeout)
        while marker not in data:
            chunk = sock.recv(1 << 20)
            self.assertTrue(chunk, "the stream closed")
            data += chunk
        return data

    @staticmethod
    def read_burst(sock, quiet):
        """Everything that arrives before `quiet` seconds pass with nothing."""
        data = b""
        sock.settimeout(quiet)
        while True:
            try:
                chunk = sock.recv(1 << 20)
            except socket.timeout:
                return data
            if not chunk:
                return data
            data += chunk

    def test_a_viewer_that_stalls_skips_frames_instead_of_queueing_them(self):
        sock = self.open_stream()
        try:
            first = self.read_until(sock, b"--frame")
            self.assertIn(b"200 OK", first.split(b"\r\n", 1)[0])

            time.sleep(STALL)
            # What piled up arrives back to back; after it, frames come one
            # per 1/FPS s, longer than the quiet gap that ends the burst.
            backlog = self.read_burst(sock, 0.5 / FPS)
            frames = backlog.count(b"--frame")
            self.assertLessEqual(frames, 3, f"{frames} frames were queued for a viewer that stalled "
                                            f"{STALL}s ({len(backlog)} bytes)")

            # And it's still a live stream.
            self.read_until(sock, b"--frame")
        finally:
            sock.close()



class TestSlowViewerOverTls(TestSlowViewer):
    # Under TLS the fd that hasUnsentData() asks the kernel about comes
    # from a TLS socket, whose native handle is its SSL*, not the fd.
    https = True


if __name__ == "__main__":
    unittest.main()
