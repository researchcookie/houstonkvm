"""
test_tls.py — HTTPS, which an Owner turns on at runtime
(tls/tls_manager.h): installing a certificate, confirm-or-revert, the
redirect, renewal by SIGHUP, cookies and HSTS under HTTPS, and the
configuration pinning it all (--tls, --tls-cert/--tls-key,
/etc/houstonkvm/tls/, --https-port).

Certificates come from a throwaway CA made with the openssl command; the
tests skip where there isn't one.
"""
import hashlib
import os
import shutil
import signal
import socket
import sqlite3
import ssl
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import DEFAULT_BINARY, Client, HoustonKVMServer, TestCA, free_port  # noqa: E402

OWNER = ("owner", "ownerpassword1")
OPERATOR = ("operator1", "operatorpassword1")
VIEWER = ("viewer1", "viewerpassword1")
WINDOW = 3   # --tls-confirm-seconds


def der_fingerprint(der):
    h = hashlib.sha256(der).hexdigest().upper()
    return ":".join(h[i:i + 2] for i in range(0, len(h), 2))


def pem_fingerprint(pem):
    return der_fingerprint(ssl.PEM_cert_to_DER_cert(pem))


def served_fingerprint(port):
    """The certificate a new connection to `port` is given (unverified)."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with socket.create_connection(("127.0.0.1", port), timeout=5) as raw, \
            ctx.wrap_socket(raw, server_hostname="localhost") as s:
        return der_fingerprint(s.getpeercert(binary_form=True))


def refuses_connections(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=2).close()
        return False
    except ConnectionRefusedError:
        return True


def run_binary(*args):
    """The server binary on its own, for options that stop it starting."""
    binary = os.environ.get("HOUSTONKVM_BINARY") or str(DEFAULT_BINARY)
    return subprocess.run([binary, *args], capture_output=True, text=True, timeout=15,
                          stdin=subprocess.DEVNULL)


class TlsTestCase(unittest.TestCase):
    """One plain-HTTP server per class, with a test CA and an Owner whose API
    token works on both sides (a browser session doesn't cross over)."""
    extra_args = ()

    @classmethod
    def setUpClass(cls):
        cls.https_port = free_port()
        cls.server = HoustonKVMServer(extra_args=[f"--tls-confirm-seconds={WINDOW}", *cls.extra_args],
                                      https=False)
        cls.server.start()
        try:
            cls.ca = TestCA(os.path.join(cls.server.tmpdir.name, "testca"))
            status, raw, _ = cls.server.setup_owner(Client(cls.server.http_port), *OWNER)
            assert status == 200, raw
            cls.owner = Client(cls.server.http_port)
            status, raw, _ = cls.owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})
            assert status == 200, raw
            status, raw, _ = cls.owner.post("/api/tokens", {"label": "tls tests"})
            assert status in (200, 201), raw
            cls.token = Client.as_json(raw)["token"]
            cls.seen = []   # every response body, to check none carries key material
        except BaseException:
            cls.server.stop()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()
        # No response ever carried any of the keys the tests used.
        secrets = [key.splitlines()[1].encode() for key in cls.ca.keys]
        for body in cls.seen:
            assert not any(secret in body.replace(b"\\n", b"\n") for secret in secrets), body

    def api(self, https=False):
        """The Owner by API token, over HTTP or HTTPS."""
        c = Client(self.https_port, tls=self.ca.context()) if https else Client(self.server.http_port)
        c.bearer = self.token
        return c

    def call(self, method, path, body=None, https=False, client=None):
        c = client or self.api(https)
        status, raw, resp = c.request(method, path, body=body)
        self.seen.append(raw)
        return status, raw, resp

    def tls_status(self, https=False):
        status, raw, _ = self.call("GET", "/api/tls", https=https)
        self.assertEqual(status, 200, raw)
        return Client.as_json(raw)

    def install(self, cert, key, **kw):
        return self.call("POST", "/api/tls/certificate", {"cert": cert, "key": key}, **kw)

    def install_ok(self, name="leaf", **issue):
        cert, key, _, _ = self.ca.issue(name, **issue)
        status, raw, _ = self.install(cert, key)
        self.assertEqual(status, 200, raw)
        return cert, Client.as_json(raw)

    def enable(self):
        status, raw, _ = self.call("POST", "/api/tls/enable")
        self.assertEqual(status, 200, raw)
        return Client.as_json(raw)

    def confirm(self, code):
        c = Client(self.https_port, tls=self.ca.context())
        status, raw, _ = c.post("/api/tls/confirm", {"code": code})
        self.seen.append(raw)
        return status, raw

    def tls_events(self, https=False):
        """Newest first."""
        status, raw, _ = self.call("GET", "/api/audit/events?type=tls.", https=https)
        self.assertEqual(status, 200, raw)
        return Client.as_json(raw)["events"]

    def wait_until(self, cond, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if cond():
                return True
            time.sleep(0.1)
        return cond()


class TestCertificates(TlsTestCase):

    def test_what_is_refused(self):
        fingerprint = lambda: (self.tls_status()["certificate"] or {}).get("fingerprint_sha256")
        before = fingerprint()
        good_cert, good_key, _, _ = self.ca.issue("good")
        other_cert, other_key, _, _ = self.ca.issue("other")
        expired, expired_key, _, _ = self.ca.issue("expired", not_before="20200101000000Z",
                                                   not_after="20200201000000Z")
        future, future_key, _, _ = self.ca.issue("future", not_before="20990101000000Z",
                                                 not_after="20990201000000Z")
        small, small_key, _, _ = self.ca.issue("small", key="rsa:1024")
        p521, p521_key, _, _ = self.ca.issue("p521", key="ec521")
        encrypted_key = subprocess.run(
            ["openssl", "pkey", "-aes256", "-passout", "pass:secret"],
            input=good_key, capture_output=True, text=True, check=True).stdout
        cases = {
            "not a certificate": ("hello", good_key, "isn't PEM"),
            "not a key": (good_cert, "hello", "isn't a PEM private key"),
            "someone else's key": (good_cert, other_key, "doesn't belong"),
            "a key with a passphrase": (good_cert, encrypted_key, "passphrase"),
            "expired": (expired, expired_key, "expired"),
            "not valid yet": (future, future_key, "isn't valid yet"),
            "RSA 1024": (small, small_key, "2048"),
            "P-521": (p521, p521_key, "P-256 or P-384"),
        }
        for name, (cert, key, why) in cases.items():
            with self.subTest(name):
                status, raw, _ = self.install(cert, key)
                self.assertEqual(status, 400, raw)
                self.assertIn(why, raw.decode())
        for body in ({"cert": good_cert}, {"key": good_key}, ["x"], "not json"):
            with self.subTest(body=str(body)[:20]):
                status, raw, _ = self.call("POST", "/api/tls/certificate", body)
                self.assertEqual(status, 400, raw)
        self.assertEqual(fingerprint(), before)   # nothing replaced it

    def test_installed_and_described_without_the_key(self):
        for key_type, label in (("rsa:3072", "RSA 3072"), ("ec384", "ECDSA P-384"), ("ec", "ECDSA P-256")):
            with self.subTest(key_type):
                cert, reply = self.install_ok(f"described-{key_type.replace(':', '')}", key=key_type)
                info = reply["certificate"]
                self.assertEqual(info["key_type"], label)
                self.assertEqual(info["names"], ["localhost", "127.0.0.1"])
                self.assertEqual(info["subject"], "CN=localhost")
                self.assertEqual(info["issuer"], "CN=HoustonKVM Test CA")
                self.assertFalse(info["self_signed"])
                self.assertIn(info["days_left"], (29, 30))
                self.assertEqual(info["fingerprint_sha256"], pem_fingerprint(cert))
                # Signed by a CA that isn't the system's, with no chain given.
                self.assertTrue(any("intermediate" in w for w in reply["warnings"]), reply)
                status = self.tls_status()
                self.assertEqual(status["certificate"]["fingerprint_sha256"], info["fingerprint_sha256"])
                self.assertTrue(status["certificate"]["covers_this_host"])
        tls_dir = os.path.join(self.server.tmpdir.name, "tls")
        self.assertEqual(os.stat(tls_dir).st_mode & 0o777, 0o700)
        self.assertEqual(os.stat(os.path.join(tls_dir, "key.pem")).st_mode & 0o777, 0o600)

    def test_a_chain_with_its_intermediate_gets_no_warning(self):
        cert, key, _, _ = self.ca.issue("chained")
        status, raw, _ = self.install(cert + self.ca.pem, key)
        self.assertEqual(status, 200, raw)
        self.assertEqual(Client.as_json(raw)["warnings"], [])

    def test_a_certificate_for_another_name_is_warned_about(self):
        cert, key, _, _ = self.ca.issue("elsewhere", names=("DNS:kvm.example.com",))
        status, raw, _ = self.install(cert, key)
        self.assertEqual(status, 200, raw)
        self.assertTrue(any("127.0.0.1" in w for w in Client.as_json(raw)["warnings"]), raw)
        self.assertFalse(self.tls_status()["certificate"]["covers_this_host"])
        self.install_ok()

    def test_hsts_needs_a_certificate_from_a_ca(self):
        cert, key, _, _ = self.ca.issue("selfsigned", self_signed=True)
        status, raw, _ = self.install(cert, key)
        self.assertEqual(status, 200, raw)
        self.assertTrue(self.tls_status()["certificate"]["self_signed"])
        status, raw, _ = self.call("PUT", "/api/tls/settings", {"hsts": True})
        self.assertEqual(status, 409, raw)
        self.assertIn("certificate authority", raw.decode())
        self.install_ok()

    def test_settings_are_checked(self):
        for body in ({"https_port": 0}, {"https_port": 70000}, {"https_port": "8443"},
                     {"https_port": self.server.http_port}, {"http_redirect": "yes"},
                     {"hsts": 1}, {"colour": "blue"}, ["x"]):
            with self.subTest(body=body):
                status, raw, _ = self.call("PUT", "/api/tls/settings", body)
                self.assertEqual(status, 400, raw)

    def test_only_an_owner(self):
        for who, role in ((OPERATOR, "operator"), (VIEWER, "viewer")):
            status, raw, _ = self.owner.post("/api/users", {"username": who[0], "password": who[1], "role": role})
            self.assertIn(status, (200, 201), raw)
            c = Client(self.server.http_port)
            c.post("/api/login", {"username": who[0], "password": who[1]})
            anon = Client(self.server.http_port)
            for method, path in (("GET", "/api/tls"), ("PUT", "/api/tls/settings"),
                                 ("POST", "/api/tls/certificate"), ("POST", "/api/tls/enable"),
                                 ("POST", "/api/tls/disable")):
                with self.subTest(who=who[0], path=path):
                    self.assertEqual(c.request(method, path, body={})[0], 403)
                    self.assertEqual(anon.request(method, path, body={})[0], 401)

    def test_changes_are_audited_without_key_material(self):
        cert, _ = self.install_ok("audited")
        events = [e for e in self.tls_events() if e["type"] == "tls.certificate_installed"]
        self.assertEqual(events[0]["detail"]["fingerprint_sha256"], pem_fingerprint(cert))
        self.assertEqual(events[0]["user"]["username"], OWNER[0])
        with sqlite3.connect(self.server.db_path) as db:
            stored = " ".join(r[0] for r in db.execute("SELECT detail FROM audit_events"))
        self.assertNotIn("BEGIN", stored)
        for key in self.ca.keys:
            self.assertNotIn(key.splitlines()[1], stored)


class TestSwitching(TlsTestCase):
    """Runs in order: HTTP -> a revert -> HTTPS -> back to HTTP."""

    def test_1_enabling_needs_a_certificate_and_a_free_port(self):
        status, raw, _ = self.call("POST", "/api/tls/enable")
        self.assertEqual(status, 409, raw)
        self.assertIn("certificate", raw.decode())
        self.install_ok()
        with socket.socket() as taken:
            taken.bind(("0.0.0.0", 0))
            taken.listen(1)
            status, raw, _ = self.call("PUT", "/api/tls/settings", {"https_port": taken.getsockname()[1]})
            self.assertEqual(status, 200, raw)
            status, raw, _ = self.call("POST", "/api/tls/enable")
            self.assertEqual(status, 409, raw)
            self.assertIn("Couldn't listen", raw.decode())
        self.assertEqual(self.tls_status()["mode"], "http")
        status, raw, _ = self.call("PUT", "/api/tls/settings", {"https_port": self.https_port})
        self.assertEqual(status, 200, raw)

    def test_2_unconfirmed_it_reverts(self):
        reply = self.enable()
        self.assertEqual(reply["expires_in"], WINDOW)
        self.assertEqual(reply["https_url"],
                         f"https://127.0.0.1:{self.https_port}/#tls-confirm={reply['confirm_code']}")
        self.assertEqual(self.tls_status()["mode"], "pending")
        # HTTPS answers with the certificate; plain HTTP still serves in full.
        self.assertEqual(served_fingerprint(self.https_port),
                         self.tls_status()["certificate"]["fingerprint_sha256"])
        self.assertEqual(Client(self.server.http_port).get("/api/status")[0], 200)
        # Confirming takes HTTPS: from plain HTTP it's refused.
        status, raw, _ = Client(self.server.http_port).post("/api/tls/confirm", {"code": reply["confirm_code"]})
        self.assertEqual(status, 409, raw)
        self.assertTrue(self.wait_until(lambda: refuses_connections(self.https_port), WINDOW + 3))
        # Logged at once, not when the server exits (stdout is a pipe here, as under systemd).
        self.server.wait_for_output(r"HTTPS: not confirmed within", timeout=2)
        status = self.tls_status()
        self.assertEqual(status["mode"], "http")
        # Admin says why, until the next try.
        self.assertEqual(status["last_revert"]["https_port"], self.https_port)
        self.assertLess(status["last_revert"]["seconds_ago"], 10)
        events = self.tls_events()
        self.assertEqual(events[0]["type"], "tls.reverted")
        self.assertEqual(events[0]["user"]["username"], OWNER[0])

    def test_3_confirmed_over_https_it_sticks(self):
        reply = self.enable()
        self.assertIsNone(self.tls_status()["last_revert"])
        status, raw = self.confirm("0" * 32)
        self.assertEqual(status, 403, raw)
        status, raw = self.confirm(reply["confirm_code"])
        self.assertEqual(status, 200, raw)
        time.sleep(WINDOW + 1)   # past the window: no revert now
        self.assertEqual(self.tls_status(https=True)["mode"], "https")
        status, raw = self.confirm(reply["confirm_code"])
        self.assertEqual(status, 409, raw)
        # Plain HTTP only redirects, keeping the path and query.
        conn = Client(self.server.http_port).connection()
        conn.request("GET", "/index.html?x=1")
        resp = conn.getresponse()
        resp.read()
        self.assertEqual(resp.status, 307)
        self.assertEqual(resp.getheader("Location"), f"https://127.0.0.1:{self.https_port}/index.html?x=1")
        event = next(e for e in self.tls_events(https=True) if e["type"] == "tls.enabled")
        self.assertEqual(event["user"]["username"], OWNER[0])
        self.assertEqual(event["detail"]["confirmed_from"], "127.0.0.1")
        status, raw, _ = self.call("POST", "/api/tls/enable", https=True)
        self.assertEqual(status, 409, raw)

    def test_4_cookies_and_origin_under_https(self):
        # The plain-HTTP session cookie isn't honoured over HTTPS.
        crossed = Client(self.https_port, tls=self.ca.context())
        crossed.cookie_name, crossed.cookie = "session", self.owner.cookie
        self.assertEqual(crossed.get("/api/me")[0], 401)

        browser = Client(self.https_port, tls=self.ca.context())
        status, raw, resp = browser.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})
        self.assertEqual(status, 200, raw)
        cookie = resp.getheader("Set-Cookie")
        self.assertTrue(cookie.startswith("__Host-session="), cookie)
        for flag in ("Path=/", "HttpOnly", "SameSite=Strict", "Secure"):
            self.assertIn(flag, cookie)
        self.assertEqual(browser.get("/api/me")[0], 200)
        origin = f"127.0.0.1:{self.https_port}"
        status, raw, _ = browser.put("/api/tls/settings", {}, headers={"Origin": f"http://{origin}"})
        self.assertEqual(status, 403, raw)
        status, raw, _ = browser.put("/api/tls/settings", {}, headers={"Origin": f"https://{origin}"})
        self.assertEqual(status, 200, raw)

    def test_5_the_input_socket_over_tls(self):
        owner = Client(self.https_port, tls=self.ca.context())
        self.assertEqual(owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})[0], 200)
        status, raw, _ = owner.post("/api/users", {"username": OPERATOR[0], "password": OPERATOR[1],
                                                   "role": "operator"})
        self.assertIn(status, (200, 201), raw)
        op = Client(self.https_port, tls=self.ca.context())
        self.assertEqual(op.post("/api/login", {"username": OPERATOR[0], "password": OPERATOR[1]})[0], 200)
        ws = op.ws("/api/input/ws")
        try:
            self.assertTrue(ws.upgraded, ws.status_line)
            ws.send_json({"type": "mousemove", "x": 0.5, "y": 0.5})
            self.assertEqual(ws.recv_json(), {"ok": False, "reason": "not_driver"})
        finally:
            ws.close()

    def test_6_settings_apply_live(self):
        # A new HTTPS port: new connections there, the old one closes.
        old, new = self.https_port, free_port()
        kept = Client(old, tls=self.ca.context()).connection()
        kept.request("GET", "/api/status")
        kept.getresponse().read()
        status, raw, _ = self.call("PUT", "/api/tls/settings", {"https_port": new}, https=True)
        self.assertEqual(status, 200, raw)
        type(self).https_port = new
        self.assertEqual(served_fingerprint(new), self.tls_status(https=True)["certificate"]["fingerprint_sha256"])
        self.assertTrue(refuses_connections(old))
        kept.request("GET", "/api/status")   # an open connection carries on
        self.assertEqual(kept.getresponse().status, 200)
        kept.close()

        # No redirect: nothing on the HTTP port.
        status, raw, _ = self.call("PUT", "/api/tls/settings", {"http_redirect": False}, https=True)
        self.assertEqual(status, 200, raw)
        self.assertTrue(refuses_connections(self.server.http_port))
        status, raw, _ = self.call("PUT", "/api/tls/settings", {"http_redirect": True}, https=True)
        self.assertEqual(status, 200, raw)
        self.assertEqual(Client(self.server.http_port).get("/api/status")[0], 307)

        # HSTS only when asked for, and only over HTTPS.
        def hsts():
            _, _, resp = Client(self.https_port, tls=self.ca.context()).get("/")
            return resp.getheader("Strict-Transport-Security")
        self.assertIsNone(hsts())
        status, raw, _ = self.call("PUT", "/api/tls/settings", {"hsts": True}, https=True)
        self.assertEqual(status, 200, raw)
        self.assertEqual(hsts(), "max-age=31536000")
        status, raw, _ = self.call("PUT", "/api/tls/settings", {"hsts": False}, https=True)
        self.assertEqual(status, 200, raw)
        self.assertIsNone(hsts())
        changes = [e["detail"]["changes"] for e in self.tls_events(https=True) if e["type"] == "tls.settings"]
        self.assertIn({"hsts": {"from": False, "to": True}}, changes)

    def test_7_a_restart_comes_back_on_https(self):
        self.server.proc.send_signal(signal.SIGTERM)
        self.server.proc.wait(timeout=15)
        self.server._drainer.join(timeout=5)
        self.server.start()
        self.assertEqual(self.tls_status(https=True)["mode"], "https")
        self.assertEqual(Client(self.server.http_port).get("/api/status")[0], 307)

    def test_8_turned_off_again(self):
        status, raw, _ = self.call("POST", "/api/tls/disable", https=True)
        self.assertEqual(status, 200, raw)
        self.assertTrue(self.wait_until(lambda: refuses_connections(self.https_port), 3))
        self.assertEqual(self.tls_status()["mode"], "http")
        self.assertEqual(Client(self.server.http_port).get("/api/status")[0], 200)
        self.assertEqual(self.tls_events()[0]["type"], "tls.disabled")
        status, raw, _ = self.call("POST", "/api/tls/disable")
        self.assertEqual(status, 409, raw)


class TestConfigurationsCertificate(unittest.TestCase):
    """--tls=on with --tls-cert/--tls-key: HTTPS from the start, the
    certificate the configuration's, renewed by SIGHUP."""

    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.ca = TestCA(os.path.join(cls.dir.name, "ca"))
        cls.cert, cls.key = os.path.join(cls.dir.name, "kvm.crt"), os.path.join(cls.dir.name, "kvm.key")
        cls.first = cls.write_pair("first")
        cls.https_port = free_port()
        cls.server = HoustonKVMServer(extra_args=["--tls=on", f"--tls-cert={cls.cert}",
                                                  f"--tls-key={cls.key}", f"--https-port={cls.https_port}"],
                                      https=False)
        cls.server.port = cls.https_port
        cls.server.tls = cls.ca.context()
        cls.server.start()
        try:
            cls.owner = Client(cls.https_port, tls=cls.ca.context())
            status, raw, _ = cls.server.setup_owner(cls.owner, *OWNER)
            assert status == 200, raw
            status, raw, _ = cls.owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})
            assert status == 200, raw
        except BaseException:
            cls.server.stop()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()
        cls.dir.cleanup()

    @classmethod
    def write_pair(cls, name, key_from=None):
        cert, key, _, _ = cls.ca.issue(name)
        with open(cls.cert, "w") as f:
            f.write(cert)
        with open(cls.key, "w") as f:
            f.write(key_from or key)
        return cert, key

    def status(self):
        status, raw, _ = self.owner.get("/api/tls")
        self.assertEqual(status, 200, raw)
        return Client.as_json(raw)

    def reload(self, expect):
        self.server.proc.send_signal(signal.SIGHUP)
        self.server.wait_for_output(expect)

    def test_the_api_cant_change_it(self):
        status = self.status()
        self.assertEqual(status["mode"], "https")
        self.assertEqual(status["pinned"], "on")
        self.assertTrue(status["certificate_managed_by_config"])
        self.assertTrue(status["https_port_managed_by_config"])
        cert, key, _, _ = self.ca.issue("ignored")
        status, raw, _ = self.owner.post("/api/tls/certificate", {"cert": cert, "key": key})
        self.assertEqual(status, 409, raw)
        self.assertIn("configuration", raw.decode())
        for path in ("/api/tls/enable", "/api/tls/disable"):
            self.assertEqual(self.owner.post(path)[0], 409)
        status, raw, _ = self.owner.put("/api/tls/settings", {"https_port": free_port()})
        self.assertEqual(status, 409, raw)
        for path in ("/api/tls/generate", "/api/tls/csr"):
            status, raw, _ = self.owner.post(path, {"names": ["kvm.example.com"]})
            self.assertEqual(status, 409, raw)
        self.assertEqual(Client(self.server.http_port).get("/api/status")[0], 307)

    def test_sighup_swaps_the_certificate_for_new_connections(self):
        kept = self.owner.connection()
        kept.request("GET", "/api/status")
        kept.getresponse().read()
        before = der_fingerprint(kept.sock.getpeercert(binary_form=True))

        second, second_key = self.write_pair("second")
        self.reload(r"certificate reloaded \(" + pem_fingerprint(second))
        self.assertEqual(served_fingerprint(self.https_port), pem_fingerprint(second))
        kept.request("GET", "/api/status")   # the open connection keeps its certificate
        self.assertEqual(kept.getresponse().status, 200)
        self.assertEqual(der_fingerprint(kept.sock.getpeercert(binary_form=True)), before)
        kept.close()

        # A pair that doesn't match keeps the one in use.
        self.write_pair("third", key_from=second_key)
        self.reload(r"reload failed, keeping the certificate in use")
        self.assertEqual(served_fingerprint(self.https_port), pem_fingerprint(second))
        self.assertIn("The last reload failed", self.status()["certificate_error"])
        status, raw, _ = self.owner.get("/api/audit/events?type=tls.reloaded")
        outcomes = [e["outcome"] for e in Client.as_json(raw)["events"]]
        self.assertEqual(outcomes[:2], ["failed", "ok"])
        self.write_pair("fourth")
        self.reload(r"certificate reloaded")


class TestOptions(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.db = os.path.join(self.dir.name, "x.db")

    def test_https_that_cant_be_served_stops_the_server_starting(self):
        ca = TestCA(os.path.join(self.dir.name, "ca"))
        cert, key, crt_path, key_path = ca.issue("leaf")
        _, other_key, _, other_key_path = ca.issue("other")
        encrypted = os.path.join(self.dir.name, "encrypted.key")
        subprocess.run(["openssl", "pkey", "-aes256", "-passout", "pass:secret", "-in", key_path,
                        "-out", encrypted], check=True)
        port = str(free_port())
        cases = {
            "--tls=on, no certificate": ([], "no certificate"),
            "a missing file": ([f"--tls-cert={self.dir.name}/nope.crt", f"--tls-key={key_path}"], "nope.crt"),
            "the wrong key": ([f"--tls-cert={crt_path}", f"--tls-key={other_key_path}"], "doesn't belong"),
            "a key with a passphrase": ([f"--tls-cert={crt_path}", f"--tls-key={encrypted}"], "passphrase"),
        }
        for name, (args, why) in cases.items():
            with self.subTest(name):
                r = run_binary(f"--port={port}", f"--db={self.db}", "--tls=on", f"--https-port={free_port()}", *args)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn(why, r.stderr)

    def test_bad_options(self):
        for args in (["--tls-cert=a.crt"], ["--tls-key=a.key"], ["--https-port=0"],
                     ["--port=9000", "--https-port=9000"], ["--tls=maybe"], ["--tls-confirm-seconds=0"]):
            with self.subTest(args=args):
                r = run_binary(f"--db={self.db}", *args)
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)

    def test_etc_houstonkvm_tls(self):
        ca = TestCA(os.path.join(self.dir.name, "ca"))
        etc = os.path.join(self.dir.name, "etc")
        os.mkdir(etc)
        _, _, crt_path, key_path = ca.issue("leaf")
        shutil.copy(crt_path, os.path.join(etc, "houstonkvm.crt"))
        # Half a pair: refuses to start.
        r = run_binary(f"--port={free_port()}", f"--db={self.db}", f"--tls-config-dir={etc}")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("houstonkvm.key is missing", r.stderr)
        # Both: the configuration's certificate, HTTPS still the Owner's choice.
        shutil.copy(key_path, os.path.join(etc, "houstonkvm.key"))
        server = HoustonKVMServer(extra_args=[f"--tls-config-dir={etc}"], https=False)
        server.start()
        self.addCleanup(server.stop)
        owner = Client(server.http_port)
        server.setup_owner(owner, *OWNER)
        owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})
        status = Client.as_json(owner.get("/api/tls")[1])
        self.assertEqual(status["mode"], "http")
        self.assertIsNone(status["pinned"])
        self.assertTrue(status["certificate_managed_by_config"])
        self.assertEqual(status["certificate"]["names"], ["localhost", "127.0.0.1"])
        cert, key, _, _ = ca.issue("other")
        self.assertEqual(owner.post("/api/tls/certificate", {"cert": cert, "key": key})[0], 409)

    def test_tls_off_pins_plain_http(self):
        server = HoustonKVMServer(extra_args=["--tls=off"], https=False)
        server.start()
        self.addCleanup(server.stop)
        owner = Client(server.http_port)
        server.setup_owner(owner, *OWNER)
        owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})
        self.assertEqual(Client.as_json(owner.get("/api/tls")[1])["pinned"], "off")
        status, raw, _ = owner.post("/api/tls/enable")
        self.assertEqual(status, 409, raw)
        self.assertIn("--tls", raw.decode())

    def test_tls_off_as_the_way_back_in_leaves_https_off(self):
        server = HoustonKVMServer(https=False)
        server.start()
        self.addCleanup(server.stop)
        owner = Client(server.http_port)
        server.setup_owner(owner, *OWNER)

        def stop():   # unlike server.stop(), keeps the database
            server.proc.send_signal(signal.SIGTERM)
            server.proc.wait(timeout=15)
            server._drainer.join(timeout=5)
            server.proc.stdout.close()

        def start(*args):
            server.extra_args = list(args)
            server.start()
            owner.post("/api/login", {"username": OWNER[0], "password": OWNER[1]})

        def saved_enabled():
            with sqlite3.connect(server.db_path) as db:
                value = db.execute("SELECT tls_enabled FROM server_settings").fetchone()[0]
            db.close()
            return value

        # HTTPS on, saved, with a certificate nobody can reach (say).
        stop()
        with sqlite3.connect(server.db_path) as db:
            db.execute("UPDATE server_settings SET tls_enabled = 1")
        db.close()

        start("--tls=off")
        server.wait_for_output("turned off by --tls=off")
        self.assertEqual(saved_enabled(), 0)
        event = Client.as_json(owner.get("/api/audit/events?type=tls.")[1])["events"][0]
        self.assertEqual((event["type"], event["user"]["username"]), ("tls.disabled", "(--tls=off)"))

        # Without the option again: still plain HTTP, until an Owner says.
        stop()
        start()
        status = Client.as_json(owner.get("/api/tls")[1])
        self.assertEqual((status["mode"], status["pinned"]), ("http", None))
        self.assertEqual(saved_enabled(), 0)


class TestOwnCa(TlsTestCase):
    """HoustonKVM's own small CA: made on first use, issues the server
    certificate; trusting it once is enough."""

    def generate(self, body=None, expect=200):
        status, raw, _ = self.call("POST", "/api/tls/generate", body if body is not None else {})
        self.assertEqual(status, expect, raw)
        return Client.as_json(raw) if expect == 200 else raw.decode()

    def download_ca(self, client):
        status, raw, resp = client.get("/api/tls/ca.crt")
        return status, raw.decode(), resp

    def test_1_one_click_certificate_from_its_own_ca(self):
        status = self.tls_status()
        self.assertIsNone(status["ca"])
        self.assertIn("127.0.0.1", status["suggested_names"])
        self.assertEqual(status["suggested_names"][0], "127.0.0.1")   # what the browser is using, first
        self.assertTrue(status["auto_renew"])

        reply = self.generate()
        info = reply["certificate"]
        self.assertTrue(info["issued_by_houstonkvm_ca"])
        self.assertEqual(info["key_type"], "ECDSA P-256")
        self.assertIn(info["days_left"], (396, 397))
        self.assertEqual(info["names"][:1], ["127.0.0.1"])
        self.assertIn("localhost", info["names"])
        self.assertEqual(reply["warnings"], [])
        ca = self.tls_status()["ca"]
        self.assertTrue(ca["subject"].startswith("CN=HoustonKVM CA on "), ca)
        self.assertEqual(info["issuer"], ca["subject"].replace("CN=", "CN=", 1))
        events = [e["type"] for e in self.tls_events()]
        self.assertIn("tls.ca_created", events)

        # Any signed-in user can fetch the CA; nobody else.
        status, pem, resp = self.download_ca(self.owner)
        self.assertEqual(status, 200)
        self.assertIn("BEGIN CERTIFICATE", pem)
        self.assertIn("houstonkvm-ca.crt", resp.getheader("Content-Disposition"))
        self.assertEqual(pem_fingerprint(pem), ca["fingerprint_sha256"])
        self.assertEqual(Client(self.server.http_port).get("/api/tls/ca.crt")[0], 401)
        status, raw, _ = self.owner.post("/api/users", {"username": VIEWER[0], "password": VIEWER[1], "role": "viewer"})
        self.assertIn(status, (200, 201), raw)
        viewer = Client(self.server.http_port)
        viewer.post("/api/login", {"username": VIEWER[0], "password": VIEWER[1]})
        self.assertEqual(self.download_ca(viewer)[0], 200)
        type(self).ca_pem = pem

    def test_2_trusting_the_ca_is_all_a_client_needs(self):
        self.assertEqual(self.call("PUT", "/api/tls/settings", {"https_port": self.https_port})[0], 200)
        reply = self.enable()
        c = Client(self.https_port, tls=ssl.create_default_context(cadata=self.ca_pem))
        status, raw, _ = c.post("/api/tls/confirm", {"code": reply["confirm_code"]})
        self.assertEqual(status, 200, raw)
        c.bearer = self.token
        self.assertEqual(c.post("/api/tls/disable")[0], 200)
        self.assertTrue(self.wait_until(lambda: refuses_connections(self.https_port), 3))

    def test_3_another_certificate_comes_from_the_same_ca(self):
        before = self.tls_status()
        reply = self.generate({"names": ["kvm.example.com", "192.0.2.10"], "key_type": "rsa-3072"})
        info = reply["certificate"]
        self.assertEqual(info["names"], ["kvm.example.com", "192.0.2.10"])
        self.assertEqual(info["subject"], "CN=kvm.example.com")
        self.assertEqual(info["key_type"], "RSA 3072")
        self.assertTrue(any("127.0.0.1" in w for w in reply["warnings"]))   # not the name in use
        after = self.tls_status()
        self.assertEqual(after["ca"], before["ca"])
        self.assertNotEqual(after["certificate"]["fingerprint_sha256"], before["certificate"]["fingerprint_sha256"])
        self.assertEqual(self.generate({"key_type": "ecdsa-p384"})["certificate"]["key_type"], "ECDSA P-384")

    def test_4_names_and_key_types_are_checked(self):
        for body in ({"names": []}, {"names": ["bad name"]}, {"names": ["a.example.com", "A.example.com"]},
                     {"names": ["x.example.com,IP:192.0.2.1"]}, {"names": ["-x.example.com"]},
                     {"names": [f"h{i}.example.com" for i in range(21)]}, {"names": "kvm.example.com"},
                     {"names": [5]}, {"key_type": "dsa"}, {"key_type": "rsa-1024"}, ["x"]):
            with self.subTest(body=str(body)[:40]):
                self.generate(body, expect=400)
        self.generate({"names": ["*.example.com", "2001:db8::1"]})

    def test_5_a_csr_for_the_owners_own_ca(self):
        self.assertEqual(self.call("GET", "/api/tls/csr")[0], 404)
        cert, _, _, _ = self.ca.issue("no-request")
        status, raw, _ = self.call("POST", "/api/tls/certificate", {"cert": cert})
        self.assertEqual(status, 400, raw)
        self.assertIn("Give the key too", raw.decode())

        for body in ({"names": ["kvm.example.com"], "country": "USA"},
                     {"names": ["kvm.example.com"], "organization": "x" * 65},
                     {"names": ["kvm.example.com"], "unit": 5}, {"names": []}):
            with self.subTest(body=str(body)[:40]):
                self.assertEqual(self.call("POST", "/api/tls/csr", body)[0], 400)

        status, raw, _ = self.call("POST", "/api/tls/csr", {
            "names": ["kvm.example.com", "127.0.0.1"], "key_type": "ecdsa-p384",
            "organization": "Example Co", "unit": "IT", "country": "US"})
        self.assertEqual(status, 200, raw)
        csr = Client.as_json(raw)["csr"]
        self.assertNotIn("PRIVATE KEY", csr)
        status, raw, resp = self.call("GET", "/api/tls/csr")
        self.assertEqual((status, raw.decode()), (200, csr))
        self.assertTrue(self.tls_status()["pending_csr"])
        described = subprocess.run(["openssl", "req", "-noout", "-subject", "-verify", "-text"],
                                   input=csr, capture_output=True, text=True, check=True)
        out = described.stdout + described.stderr
        for part in ("C=US", "O=Example Co", "OU=IT", "CN=kvm.example.com", "DNS:kvm.example.com",
                     "IP Address:127.0.0.1", "verify OK", "384 bit"):
            self.assertIn(part, out)

        # Signed by the Owner's CA (the test CA here), then installed alone.
        d = self.ca.dir
        (d / "req.csr").write_text(csr)
        subprocess.run(["openssl", "x509", "-req", "-in", d / "req.csr", "-CA", self.ca.crt,
                        "-CAkey", self.ca.key, "-CAcreateserial", "-days", "60", "-copy_extensions", "copyall",
                        "-out", d / "signed.crt"], check=True, capture_output=True)
        signed = (d / "signed.crt").read_text()
        status, raw, _ = self.call("POST", "/api/tls/certificate", {"cert": cert})   # not for this request
        self.assertEqual(status, 400, raw)
        self.assertIn("wasn't issued for the pending signing request", raw.decode())
        status, raw, _ = self.call("POST", "/api/tls/certificate", {"cert": signed + self.ca.pem})
        self.assertEqual(status, 200, raw)
        info = Client.as_json(raw)["certificate"]
        self.assertEqual(info["fingerprint_sha256"], pem_fingerprint(signed))
        self.assertFalse(info["issued_by_houstonkvm_ca"])
        self.assertEqual(self.call("GET", "/api/tls/csr")[0], 404)   # used up
        self.assertFalse(self.tls_status()["pending_csr"])
        sources = [e["detail"].get("source") for e in self.tls_events() if e["type"] == "tls.certificate_installed"]
        self.assertEqual(sources[0], "csr")
        self.assertIn("tls.csr_created", [e["type"] for e in self.tls_events()])

    def test_6_a_csr_can_be_discarded(self):
        self.assertEqual(self.call("POST", "/api/tls/csr", {"names": ["kvm.example.com"]})[0], 200)
        self.assertEqual(self.call("DELETE", "/api/tls/csr")[0], 200)
        self.assertEqual(self.call("GET", "/api/tls/csr")[0], 404)
        self.assertEqual(self.call("DELETE", "/api/tls/csr")[0], 404)

    def test_7_only_an_owner_generates(self):
        status, raw, _ = self.owner.post("/api/users", {"username": OPERATOR[0], "password": OPERATOR[1],
                                                        "role": "operator"})
        self.assertIn(status, (200, 201), raw)
        op = Client(self.server.http_port)
        op.post("/api/login", {"username": OPERATOR[0], "password": OPERATOR[1]})
        for method, path in (("POST", "/api/tls/generate"), ("POST", "/api/tls/csr"),
                             ("GET", "/api/tls/csr"), ("DELETE", "/api/tls/csr")):
            with self.subTest(path=path, method=method):
                self.assertEqual(op.request(method, path, body={})[0], 403)


class TestAutoRenewal(TlsTestCase):
    """The own CA's certificates renew themselves before they expire, unless
    the Owner says not to. Issued for 10 days here, so one is always due."""
    extra_args = ("--tls-cert-days=10", "--tls-renew-check-seconds=1")

    def fingerprint(self):
        return self.tls_status()["certificate"]["fingerprint_sha256"]

    def test_1_off_when_the_owner_says_so(self):
        self.assertEqual(self.call("PUT", "/api/tls/settings", {"auto_renew": False})[0], 200)
        self.assertEqual(self.call("POST", "/api/tls/generate", {})[0], 200)
        first = self.fingerprint()
        time.sleep(2.5)
        self.assertEqual(self.fingerprint(), first)

    def test_2_on_by_default_and_keeps_the_names_and_the_ca(self):
        before = self.tls_status()
        self.assertEqual(self.call("PUT", "/api/tls/settings", {"auto_renew": True})[0], 200)
        self.assertTrue(self.wait_until(lambda: self.fingerprint() != before["certificate"]["fingerprint_sha256"], 5))
        after = self.tls_status()
        self.assertEqual(after["certificate"]["names"], before["certificate"]["names"])
        self.assertEqual(after["ca"], before["ca"])
        renewal = next(e for e in self.tls_events() if e["type"] == "tls.certificate_installed")
        self.assertEqual(renewal["user"]["username"], "(auto-renew)")

    def test_3_never_someone_elses_certificate(self):
        self.assertEqual(self.call("PUT", "/api/tls/settings", {"auto_renew": False})[0], 200)
        cert, key, _, _ = self.ca.issue("theirs", not_after=time.strftime("%Y%m%d%H%M%SZ",
                                                                             time.gmtime(time.time() + 5 * 86400)))
        self.assertEqual(self.install(cert, key)[0], 200)
        self.assertEqual(self.call("PUT", "/api/tls/settings", {"auto_renew": True})[0], 200)
        time.sleep(2.5)
        self.assertEqual(self.fingerprint(), pem_fingerprint(cert))


REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class TestCertbotDeployHook(TlsTestCase):
    """docs/examples/certbot-deploy-hook-api.sh, run as certbot would."""

    def setUp(self):
        if not shutil.which("curl"):
            self.skipTest("no curl")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.token_file = os.path.join(self.tmp.name, "token")
        with open(self.token_file, "w") as f:
            f.write(self.token)

    def lineage(self, domain, name):
        """What certbot leaves in /etc/letsencrypt/live/<domain>/."""
        cert, key, _, _ = self.ca.issue(name)
        live = os.path.join(self.tmp.name, "live", domain)
        os.makedirs(live, exist_ok=True)
        with open(os.path.join(live, "fullchain.pem"), "w") as f:
            f.write(cert + self.ca.pem)
        with open(os.path.join(live, "privkey.pem"), "w") as f:
            f.write(key)
        return live, cert

    def hook(self, live, url):
        env = dict(os.environ, RENEWED_LINEAGE=live, HOUSTONKVM_DOMAIN="localhost", HOUSTONKVM_URL=url,
                   HOUSTONKVM_TOKEN_FILE=self.token_file, CURL_CA_BUNDLE=str(self.ca.crt))
        return subprocess.run([os.path.join(REPO, "docs/examples/certbot-deploy-hook-api.sh")],
                              env=env, capture_output=True, text=True, timeout=30)

    def test_it_installs_renewals_over_http_then_https(self):
        # The first certificate, while HTTPS is off: plain HTTP on this machine.
        live, cert = self.lineage("localhost", "certbot1")
        run = self.hook(live, f"http://127.0.0.1:{self.server.http_port}")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.tls_status()["certificate"]["fingerprint_sha256"], pem_fingerprint(cert))

        self.assertEqual(self.call("PUT", "/api/tls/settings", {"https_port": self.https_port})[0], 200)
        status, raw = self.confirm(self.enable()["confirm_code"])
        self.assertEqual(status, 200, raw)

        # A renewal, over HTTPS, checked against the trusted CAs.
        live, cert = self.lineage("localhost", "certbot2")
        run = self.hook(live, f"https://localhost:{self.https_port}")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(served_fingerprint(self.https_port), pem_fingerprint(cert))

        # Another domain's renewal is none of its business.
        other, _ = self.lineage("example.org", "certbot3")
        run = self.hook(other, f"https://localhost:{self.https_port}")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(served_fingerprint(self.https_port), pem_fingerprint(cert))

        # A refusal fails the hook, so certbot reports it.
        with open(os.path.join(live, "privkey.pem"), "w") as f:
            f.write(self.ca.issue("certbot4")[1])
        run = self.hook(live, f"https://localhost:{self.https_port}")
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("doesn't belong", run.stdout + run.stderr)
        self.assertEqual(served_fingerprint(self.https_port), pem_fingerprint(cert))


if __name__ == "__main__":
    unittest.main()
