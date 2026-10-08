"""First-run setup, login/logout, and session-cookie auth."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import ServerTestCase


class TestSetupAndAuth(ServerTestCase):
    def test_01_status_before_setup(self):
        status, raw, _ = self.client.get("/api/status")
        self.assertEqual(status, 200)
        body = self.client.as_json(raw)
        self.assertTrue(body["needs_setup"])
        self.assertFalse(body["authenticated"])

    def test_01a_setup_needs_the_printed_code(self):
        # Otherwise whoever reaches a fresh install first becomes its Owner.
        for body in ({"username": "mallory", "password": "whatever12345"},
                     {"username": "mallory", "password": "whatever12345", "setup_code": "AAAA-AAAA-AAAA"}):
            with self.subTest(body=body):
                status, raw, _ = self.new_client().post("/api/setup", body)
                self.assertEqual(status, 403, raw)
                self.assertIn(b"journalctl", raw)
        status, raw, _ = self.client.get("/api/status")
        self.assertTrue(self.client.as_json(raw)["needs_setup"])

    def test_01b_setup_needs_an_8_character_password(self):
        status, raw, _ = self.new_client().post(
            "/api/setup", {"username": "alice", "password": "short", "setup_code": self.server.setup_code})
        self.assertEqual(status, 400, raw)

    def test_02_setup_creates_owner_and_session(self):
        # Typed by hand: case and dashes don't matter.
        code = self.server.setup_code.replace("-", "").lower()
        status, raw, resp = self.client.post(
            "/api/setup", {"username": "alice", "password": "correct horse battery", "setup_code": code}
        )
        self.assertEqual(status, 200, raw)
        self.assertTrue(self.client.as_json(raw)["ok"])
        self.assertIsNotNone(self.client.cookie, "setup should Set-Cookie a session")

        status, raw, _ = self.client.get("/api/status")
        body = self.client.as_json(raw)
        self.assertFalse(body["needs_setup"])
        self.assertTrue(body["authenticated"])

    def test_03_setup_again_conflicts(self):
        other = self.new_client()
        status, raw, _ = other.post(
            "/api/setup", {"username": "mallory", "password": "whatever12345",
                           "setup_code": self.server.setup_code}
        )
        self.assertEqual(status, 409, raw)

    def test_04_me_returns_owner_role(self):
        # self.client is a fresh, unauthenticated Client per test method
        # (see ServerTestCase.setUp) — it doesn't inherit test_02's cookie,
        # so log in again rather than assuming session state carried over.
        self.client.post("/api/login", {"username": "alice", "password": "correct horse battery"})
        status, raw, _ = self.client.get("/api/me")
        self.assertEqual(status, 200, raw)
        body = self.client.as_json(raw)
        self.assertTrue(body["ok"])
        self.assertEqual(body["role"], "owner")

    def test_05_login_wrong_password_rejected(self):
        other = self.new_client()
        status, raw, _ = other.post(
            "/api/login", {"username": "alice", "password": "wrong password"}
        )
        self.assertEqual(status, 401, raw)
        self.assertIsNone(other.cookie)

    def test_06_login_correct_password_succeeds(self):
        other = self.new_client()
        status, raw, _ = other.post(
            "/api/login", {"username": "alice", "password": "correct horse battery"}
        )
        self.assertEqual(status, 200, raw)
        self.assertIsNotNone(other.cookie)

        status, raw, _ = other.get("/api/me")
        self.assertEqual(status, 200)
        self.assertEqual(other.as_json(raw)["role"], "owner")

    def test_07_login_unknown_user_rejected(self):
        other = self.new_client()
        status, _, _ = other.post(
            "/api/login", {"username": "nobody", "password": "irrelevant123"}
        )
        self.assertEqual(status, 401)

    def test_08_logout_clears_session(self):
        other = self.new_client()
        other.post("/api/login", {"username": "alice", "password": "correct horse battery"})
        status, raw, _ = other.get("/api/me")
        self.assertEqual(status, 200, raw)

        other.post("/api/logout")
        status, raw, _ = other.get("/api/me")
        self.assertEqual(status, 401, raw)

    def test_09_protected_endpoint_requires_auth(self):
        anon = self.new_client()
        for method, path in [
            ("GET", "/api/me"),
            ("GET", "/api/settings"),
            ("GET", "/api/stream"),
            ("GET", "/api/control/status"),
        ]:
            with self.subTest(path=path):
                status, _, _ = anon.request(method, path)
                self.assertEqual(status, 401)
