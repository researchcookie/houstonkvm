"""Browser-facing defences: the Content-Security-Policy on UI pages, the
Origin check on cookie-authenticated requests, and whose address the
sign-in rate limiter counts when the server sits behind a proxy."""
import re
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import REPO_ROOT, Client, HoustonKVMServer, ServerTestCase
from test_control import ControlTestBase

OWNER = ("owner", "ownerpassword1")
OPERATOR = ("pilot", "pilotpassword1")
EVIL = "http://127.0.0.1:1"   # same site as the server, different port


def login(client, creds):
    status, raw, _ = client.post("/api/login", {"username": creds[0], "password": creds[1]})
    assert status == 200, raw


class TestContentSecurityPolicy(ServerTestCase):
    def test_ui_pages_allow_only_their_own_scripts_and_styles(self):
        for path in ("/", "/index.html", "/js/auth.js"):
            with self.subTest(path=path):
                status, _, resp = self.client.get(path)
                self.assertEqual(status, 200)
                csp = resp.getheader("Content-Security-Policy")
                self.assertIn("script-src 'self'", csp)
                self.assertIn("style-src 'self'", csp)
                self.assertIn("frame-ancestors 'none'", csp)
                self.assertNotIn("unsafe-inline", csp)

    def test_index_has_no_inline_script_or_style(self):
        # The policy above would block them, silently breaking the UI.
        html = (REPO_ROOT / "ui" / "index.html").read_text()
        self.assertEqual(re.findall(r"<script(?![^>]*\bsrc=)[^>]*>", html), [])
        self.assertNotIn("<style", html)
        self.assertIsNone(re.search(r"\sstyle=", html))
        self.assertIsNone(re.search(r"\son[a-z]+=", html))


class TestOriginCheck(ServerTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.owner = Client(cls.server.port)
        status, raw, _ = cls.server.setup_owner(cls.owner, *OWNER)
        assert status == 200, raw
        status, raw, _ = cls.owner.post(
            "/api/users", {"username": OPERATOR[0], "password": OPERATOR[1], "role": "operator"})
        assert status == 200, raw
        cls.own = f"{'https' if cls.server.tls else 'http'}://127.0.0.1:{cls.server.port}"

    def operator(self):
        c = self.new_client()
        login(c, OPERATOR)
        return c

    def test_cookie_requests_from_another_origin_are_refused(self):
        op = self.operator()
        for method, path in [
            ("POST", "/api/targets/1/control/acquire"),
            ("POST", "/api/targets/1/input"),      # skips authenticate() on its fast path
            ("POST", "/api/tokens"),
            ("POST", "/api/logout"),              # would release the driver's control
            ("POST", "/api/login"),
        ]:
            for origin in (EVIL, "null"):
                with self.subTest(path=path, origin=origin):
                    status, raw, _ = op.request(method, path, body={}, headers={"Origin": origin})
                    self.assertEqual(status, 403, raw)
                    self.assertIn(b"Cross-origin", raw)
        status, _, _ = op.get("/api/me")
        self.assertEqual(status, 200, "the refused logout must not have signed us out")

    def test_websocket_upgrade_from_another_origin_is_refused(self):
        op = self.operator()
        ws = op.ws("/api/targets/1/input/ws", headers={"Origin": EVIL})
        self.addCleanup(ws.close)
        self.assertEqual(ws.status, 403, ws.status_line)

    def test_same_origin_and_originless_requests_pass(self):
        op = self.operator()
        for headers in ({"Origin": self.own}, {"Origin": self.own.upper()}, {}):
            with self.subTest(headers=headers):
                status, raw, _ = op.post("/api/targets/1/control/acquire", headers=headers)
                self.assertNotEqual(status, 403, raw)
                op.post("/api/targets/1/control/release")
        # A read is harmless whatever page asked for it (CORS keeps the answer
        # from that page).
        status, _, _ = op.get("/api/me", headers={"Origin": EVIL})
        self.assertEqual(status, 200)

    def test_bearer_tokens_are_not_origin_checked(self):
        # No ambient credential: a page can only send a token it already has.
        status, raw, _ = self.owner.post("/api/tokens", {"label": "bot"})
        self.assertEqual(status, 200, raw)
        bot = self.new_client()
        bot.bearer = self.owner.as_json(raw)["token"]
        status, raw, _ = bot.put("/api/targets/1", {"description": "from a bot"}, headers={"Origin": EVIL})
        self.assertNotEqual(status, 403, raw)


class TestOriginWhileDriving(ControlTestBase):
    """The case that matters: someone is driving, and a page on another port
    of the same host tries to type into their machine with their cookie."""

    def test_input_from_another_origin_is_refused_while_driving(self):
        self.acquire(self.op1, self.A())
        key = {"type": "key", "code": "KeyA", "pressed": True}
        status, raw, _ = self.op1.post(f"/api/targets/{self.A()}/input", key, headers={"Origin": EVIL})
        self.assertEqual(status, 403, raw)
        status, raw, _ = self.op1.post(f"/api/targets/{self.A()}/input", key,
                                       headers={"Origin": f"{'https' if self.server.tls else 'http'}://"
                                                          f"127.0.0.1:{self.server.port}"})
        self.assertEqual(status, 200, raw)


class TestRequestBodyLimit(ServerTestCase):
    """A request body over the per-route cap must be answered 413 once and
    stop there: no second write into the finished response, and the route
    handler must not run on the bytes received before the limit tripped."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.owner = Client(cls.server.port, tls=cls.server.tls)
        status, raw, _ = cls.server.setup_owner(cls.owner, *OWNER)
        assert status == 200, raw

    def _send_split_oversized_login(self):
        """POST /api/login whose body crosses the 4096-byte cap mid-way, sent
        in three writes so the server sees them as separate chunks. The
        overflow is JSON whitespace, so the bytes kept before and after it
        still form a VALID login (nonexistent user, short password) — if the
        handler wrongly ran, it would record a failed attempt against this IP.
        Returns the raw bytes the server wrote back."""
        prefix = b'{"username":"nobody","password":"wrongpw"'
        head = prefix + b" " * (4000 - len(prefix))   # 4000 bytes, under the cap
        overflow = b" " * 200                           # pushes cumulative over 4096
        tail = b"}"                                     # head+tail alone is valid JSON
        content_length = len(head) + len(overflow) + len(tail)

        sock = self.new_client().connect(timeout=5)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        request = (
            f"POST /api/login HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{self.server.port}\r\n"
            f"Content-Type: application/json\r\n"
            f"Content-Length: {content_length}\r\n\r\n"
        ).encode()
        sock.sendall(request + head)
        time.sleep(0.2)
        sock.sendall(overflow)   # cumulative now over the cap -> 413
        time.sleep(0.2)
        sock.sendall(tail)       # last chunk; must not resurrect the handler
        sock.settimeout(1)
        data = b""
        try:
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
        sock.close()
        return data

    def test_oversized_body_gives_one_413_and_runs_no_handler(self):
        for _ in range(6):
            data = self._send_split_oversized_login()
            self.assertIn(b"413", data.split(b"\r\n", 1)[0], data)
            # The 413 is the whole response: nothing follows the header
            # terminator. A handler running after end() would leak its body
            # (e.g. "Invalid credentials") here.
            headers, _, trailing = data.partition(b"\r\n\r\n")
            self.assertEqual(trailing, b"", data)

        # Six over-limit logins for a nonexistent user: had the handler run on
        # each, this IP would be locked out (5 failures). A correct owner login
        # proves none were recorded.
        status, raw, _ = self.new_client().post("/api/login",
                                                 {"username": OWNER[0], "password": OWNER[1]})
        self.assertEqual(status, 200, raw)


class TestAllowedOrigin(ServerTestCase):
    """--allowed-origin, for a proxy that doesn't pass the browser's Host on."""

    @classmethod
    def setUpClass(cls):
        cls.server = HoustonKVMServer(extra_args=["--allowed-origin=https://KVM.example.com/"])
        cls.server.start()

    def test_the_named_origin_is_accepted_and_others_still_refused(self):
        c = self.new_client()
        status, raw, _ = c.post("/api/login", {"username": "x", "password": "y"},
                                headers={"Origin": "https://kvm.example.com"})
        self.assertEqual(status, 401, raw)   # got past the Origin check
        status, raw, _ = c.post("/api/login", {"username": "x", "password": "y"},
                                headers={"Origin": "https://evil.example.com"})
        self.assertEqual(status, 403, raw)

    def test_a_malformed_origin_option_is_rejected(self):
        s = HoustonKVMServer(extra_args=["--allowed-origin=kvm.example.com"])
        try:
            with self.assertRaisesRegex(RuntimeError, "Invalid origin"):
                s.start()
        finally:
            s.stop()


def fail_login(client, forwarded_for=None):
    headers = {"X-Forwarded-For": forwarded_for} if forwarded_for else {}
    status, _, _ = client.post("/api/login", {"username": "nobody", "password": "wrong"}, headers=headers)
    return status


class TestRateLimitBehindProxy(ServerTestCase):
    """With --trusted-proxy, each visitor behind the proxy is locked out on
    their own; the header is ignored from anyone else."""

    @classmethod
    def setUpClass(cls):
        cls.server = HoustonKVMServer(extra_args=["--trusted-proxy=127.0.0.1"])
        cls.server.start()

    def test_one_visitor_locked_out_does_not_lock_out_the_next(self):
        c = self.new_client()
        # A client-supplied hop left of the proxy's own is not believed.
        statuses = [fail_login(c, f"198.51.100.{i}, 203.0.113.7") for i in range(6)]
        self.assertEqual(statuses[-1], 429, statuses)
        self.assertEqual(fail_login(c, "203.0.113.8"), 401)


class TestRateLimitWithoutProxy(ServerTestCase):
    def test_forwarded_for_from_an_untrusted_peer_is_ignored(self):
        c = self.new_client()
        statuses = [fail_login(c, f"203.0.113.{i}") for i in range(6)]
        self.assertEqual(statuses[-1], 429, statuses)
