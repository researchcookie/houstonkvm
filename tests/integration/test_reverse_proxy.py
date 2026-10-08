"""
test_reverse_proxy.py — HoustonKVM behind nginx doing TLS, set up exactly as
the README says: the server started with its proxy options, and nginx with
the `location /` block copied from README.md's nginx sketch, so the README
can't drift from what works.

Checks sign-in and the Secure cookie, the Origin check, X-Forwarded-For
reaching the audit log, a WebSocket upgrade, and (with synthetic capture) an
slow MJPEG viewer, all through the proxy.

Needs nginx: $HOUSTONKVM_NGINX, or nginx on the PATH or in /usr/sbin. It
needn't be installed, only unpacked (nginx-core's RPM, via rpm2cpio).
Skips without it.
"""
import http.client
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import Client, HoustonKVMServer, TestCA, free_port  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OWNER = ("owner", "ownerpassword1")
OPERATOR = ("operator1", "operatorpassword1")
README_OPTIONS = ["--bind=127.0.0.1", "--tls=off", "--secure-cookies", "--trusted-proxy=127.0.0.1"]


def find_nginx():
    for candidate in (os.environ.get("HOUSTONKVM_NGINX"), shutil.which("nginx"), "/usr/sbin/nginx"):
        if candidate and os.access(candidate, os.X_OK):
            return candidate
    return None


def readme_nginx_block():
    """The ```nginx block in README.md, as written."""
    with open(os.path.join(REPO, "README.md")) as f:
        match = re.search(r"```nginx\n(.*?)```", f.read(), re.S)
    assert match, "README.md has no nginx sketch"
    return match.group(1)


class TestBehindNginx(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        nginx = find_nginx()
        if not nginx:
            raise unittest.SkipTest("no nginx (set HOUSTONKVM_NGINX)")
        cls.server = HoustonKVMServer(extra_args=README_OPTIONS, https=False)
        cls.server.start()
        try:
            cls._start_nginx(nginx)
        except BaseException:
            cls.server.stop()
            raise

    @classmethod
    def _start_nginx(cls, nginx):
        cls.dir = tempfile.TemporaryDirectory()
        d = cls.dir.name
        cls.ca = TestCA(os.path.join(d, "ca"))
        _, _, crt, key = cls.ca.issue("proxy")
        cls.port = free_port()
        location = readme_nginx_block().replace("127.0.0.1:8080", f"127.0.0.1:{cls.server.http_port}")
        conf = os.path.join(d, "nginx.conf")
        with open(conf, "w") as f:
            f.write(f"""
daemon off;
master_process off;
pid {d}/nginx.pid;
error_log {d}/error.log warn;
events {{ worker_connections 64; }}
http {{
    access_log off;
    client_body_temp_path {d}/body;
    proxy_temp_path {d}/proxy;
    fastcgi_temp_path {d}/fastcgi;
    uwsgi_temp_path {d}/uwsgi;
    scgi_temp_path {d}/scgi;
    server {{
        listen 127.0.0.1:{cls.port} ssl;
        server_name localhost;
        ssl_certificate {crt};
        ssl_certificate_key {key};
{location}
    }}
}}
""")
        cls.nginx = subprocess.Popen([nginx, "-p", d, "-c", conf, "-e", f"{d}/error.log"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.time() + 10
        while True:
            try:
                socket.create_connection(("127.0.0.1", cls.port), timeout=1).close()
                break
            except OSError:
                if cls.nginx.poll() is not None or time.time() > deadline:
                    raise RuntimeError("nginx didn't start: " + cls.nginx.stderr.read().decode())
                time.sleep(0.1)

        status, raw, _ = cls.server.setup_owner(cls.client(), *OWNER)
        assert status == 200, raw

    @classmethod
    def tearDownClass(cls):
        cls.nginx.terminate()
        cls.nginx.wait(10)
        cls.server.stop()
        cls.dir.cleanup()

    @classmethod
    def client(cls):
        """A browser, as far as the server can tell: HTTPS to nginx, and
        the Origin a page served from there sends."""
        return Client(cls.port, tls=cls.ca.context())

    @property
    def origin(self):
        return {"Origin": f"https://127.0.0.1:{self.port}"}

    def sign_in(self, user, headers=None):
        c = self.client()
        status, raw, resp = c.post("/api/login", {"username": user[0], "password": user[1]},
                                   headers={**self.origin, **(headers or {})})
        self.assertEqual(status, 200, raw)
        return c, resp

    def test_sign_in_with_a_secure_cookie(self):
        c, resp = self.sign_in(OWNER)
        self.assertIn("Secure", resp.getheader("Set-Cookie"))
        status, raw, _ = c.get("/api/me", headers=self.origin)
        self.assertEqual(status, 200, raw)
        self.assertEqual(Client.as_json(raw)["username"], OWNER[0])

    def test_the_origin_check_still_works(self):
        c, _ = self.sign_in(OWNER)
        status, raw, _ = c.post("/api/tokens", {"label": "x"}, headers={"Origin": "https://evil.example"})
        self.assertEqual(status, 403, raw)
        status, raw, _ = c.post("/api/tokens", {"label": "x"}, headers=self.origin)
        self.assertIn(status, (200, 201), raw)

    def test_the_visitors_own_address_is_the_one_seen(self):
        # A visitor at 127.0.0.2 claiming another address: nginx appends the
        # one it saw, and the server believes only that, the hop next to the
        # trusted proxy.
        conn = http.client.HTTPSConnection("127.0.0.1", self.port, timeout=10, context=self.ca.context(),
                                           source_address=("127.0.0.2", 0))
        conn.request("POST", "/api/login", body=json.dumps({"username": OWNER[0], "password": "wrong-password"}),
                     headers={**self.origin, "Content-Type": "application/json",
                              "X-Forwarded-For": "198.51.100.7"})
        self.assertEqual(conn.getresponse().status, 401)
        conn.close()
        c, _ = self.sign_in(OWNER)
        status, raw, _ = c.get("/api/audit/events?type=auth.login", headers=self.origin)
        self.assertEqual(status, 200, raw)
        refused = [e["ip"] for e in Client.as_json(raw)["events"] if e["outcome"] != "ok"]
        self.assertEqual(refused, ["127.0.0.2"])

    def test_websocket_upgrade(self):
        owner, _ = self.sign_in(OWNER)
        status, raw, _ = owner.post("/api/users", {"username": OPERATOR[0], "password": OPERATOR[1],
                                                   "role": "operator"}, headers=self.origin)
        self.assertIn(status, (200, 201, 409), raw)
        op, _ = self.sign_in(OPERATOR)
        ws = op.ws("/api/input/ws", headers=self.origin)
        try:
            self.assertTrue(ws.upgraded, ws.status_line)
            ws.send_json({"type": "mousemove", "x": 0.5, "y": 0.5})
            self.assertEqual(ws.recv_json(), {"ok": False, "reason": "not_driver"})
        finally:
            ws.close()

    def test_a_slow_viewer_isnt_spooled_to_disk(self):
        # A buffering proxy takes every frame off the server's hands, so the
        # server never skips any for a viewer that can't keep up
        # (test_stream_backpressure.py), and spools them to disk for it:
        # hence proxy_buffering off. Even then, the proxy's socket from the
        # server holds what the kernel lets it (tcp_rmem, megabytes), which
        # the server can't see, so a stalled viewer gets that backlog.
        fps, stall = 5, 4
        owner, _ = self.sign_in(OWNER)
        status, raw, _ = owner.post("/api/targets", {
            "name": "proxied", "enabled": True,
            "v4l2_device": "synthetic:desktop/proxied",
            "capture_width": 1280, "capture_height": 720, "capture_fps": fps,
            "qmp_socket": os.path.join(self.dir.name, "no-qemu.sock"),
        }, headers=self.origin)
        self.assertEqual(status, 201, raw)
        tid = Client.as_json(raw)["id"]
        deadline = time.time() + 5
        while owner.get(f"/api/targets/{tid}/snapshot")[0] != 200:
            if time.time() > deadline:
                self.skipTest("this server has no synthetic capture (build with -DHOUSTONKVM_LOADTEST=ON)")
            time.sleep(0.2)

        raw_sock = socket.socket()
        raw_sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16 * 1024)
        raw_sock.connect(("127.0.0.1", self.port))
        sock = owner.wrap(raw_sock)
        try:
            sock.sendall((f"GET /api/targets/{tid}/stream HTTP/1.1\r\nHost: 127.0.0.1:{self.port}\r\n"
                          f"{owner.cookie_header()}\r\n").encode())
            data = b""
            sock.settimeout(3)
            while b"--frame" not in data:
                chunk = sock.recv(1 << 20)
                self.assertTrue(chunk, "the stream closed")
                data += chunk
            self.assertIn(b"200", data.split(b"\r\n", 1)[0])

            time.sleep(stall)
            # What piled up, then live frames: read for a while.
            backlog, until = bytearray(), time.time() + 3
            while time.time() < until:
                chunk = sock.recv(1 << 20)
                self.assertTrue(chunk, "the stream closed")
                backlog += chunk
            self.assertIn(b"--frame", backlog)
            with open(os.path.join(self.dir.name, "error.log")) as f:
                self.assertNotIn("buffered to a temporary file", f.read())
        finally:
            sock.close()

if __name__ == "__main__":
    unittest.main()
