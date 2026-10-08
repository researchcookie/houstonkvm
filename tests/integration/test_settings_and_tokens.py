"""Per-user preferences (/api/settings) and bot API tokens (/api/tokens,
Authorization: Bearer)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import Client, ServerTestCase


class TestSettingsAndTokens(ServerTestCase):
    # /api/setup only ever succeeds once per server, so the one account
    # these tests share is created here and each test method just logs
    # into it (self.client is a fresh Client per test, from the base
    # class's setUp — only the session cookie is per-test, not the user).
    USERNAME = "user"
    PASSWORD = "somepassword1"

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        bootstrap = Client(cls.server.port)
        status, raw, _ = cls.server.setup_owner(bootstrap, cls.USERNAME, cls.PASSWORD)
        assert status == 200, raw

    def setUp(self):
        super().setUp()
        status, raw, _ = self.client.post(
            "/api/login", {"username": self.USERNAME, "password": self.PASSWORD}
        )
        assert status == 200, raw

    # Numbered because these three share one account's settings row across
    # the whole class (see USERNAME/PASSWORD above) — test_01 needs to
    # observe fresh defaults before test_02 overwrites them, so the order
    # is load-bearing, not incidental. unittest runs methods within a class
    # in sorted-name order, which this makes explicit rather than relying
    # on alphabetical luck.
    def test_01_settings_defaults(self):
        status, raw, _ = self.client.get("/api/settings")
        self.assertEqual(status, 200, raw)
        body = self.client.as_json(raw)
        self.assertEqual(body["theme"], "dark")
        self.assertEqual(body["default_stream_mode"], "mjpeg")
        self.assertIsNone(body["webrtc_bitrate_kbps"])

    def test_02_settings_round_trip(self):
        status, raw, _ = self.client.put(
            "/api/settings",
            {"theme": "light", "default_stream_mode": "webrtc", "webrtc_bitrate_kbps": 6000},
        )
        self.assertEqual(status, 200, raw)

        status, raw, _ = self.client.get("/api/settings")
        body = self.client.as_json(raw)
        self.assertEqual(body["theme"], "light")
        self.assertEqual(body["default_stream_mode"], "webrtc")
        self.assertEqual(body["webrtc_bitrate_kbps"], 6000)

    def test_03_settings_rejects_invalid_theme(self):
        status, raw, _ = self.client.put(
            "/api/settings", {"theme": "purple", "default_stream_mode": "mjpeg"}
        )
        self.assertEqual(status, 400, raw)

    def test_create_list_and_revoke_token(self):
        status, raw, _ = self.client.post("/api/tokens", {"label": "automation-agent"})
        self.assertEqual(status, 200, raw)
        created = self.client.as_json(raw)
        self.assertIn("token", created)
        self.assertIn("id", created)

        status, raw, _ = self.client.get("/api/tokens")
        self.assertEqual(status, 200, raw)
        labels = {t["label"] for t in self.client.as_json(raw)}
        self.assertIn("automation-agent", labels)

        status, raw, _ = self.client.delete(f"/api/tokens/{created['id']}")
        self.assertEqual(status, 200, raw)

        status, raw, _ = self.client.get("/api/tokens")
        labels = {t["label"] for t in self.client.as_json(raw)}
        self.assertNotIn("automation-agent", labels)

    def test_bearer_token_authenticates_api_requests(self):
        status, raw, _ = self.client.post("/api/tokens", {"label": "bot"})
        token = self.client.as_json(raw)["token"]

        bot = self.new_client()
        bot.bearer = token
        # /api/control/status doesn't touch capture/input hardware, so it
        # behaves identically on a bare dev box and on a fully-equipped
        # rig — unlike /api/snapshot, whose result depends on whether
        # /dev/video0 happens to exist on the machine running the tests.
        status, raw, _ = bot.get("/api/control/status")
        self.assertEqual(status, 200, raw)
        self.assertIn("is_driver", bot.as_json(raw))

    def test_invalid_bearer_token_rejected(self):
        bot = self.new_client()
        bot.bearer = "0" * 64
        status, _, _ = bot.get("/api/control/status")
        self.assertEqual(status, 401)

    def test_revoked_token_no_longer_works(self):
        status, raw, _ = self.client.post("/api/tokens", {"label": "temp"})
        created = self.client.as_json(raw)

        bot = self.new_client()
        bot.bearer = created["token"]
        status, _, _ = bot.get("/api/control/status")
        self.assertEqual(status, 200)

        self.client.delete(f"/api/tokens/{created['id']}")

        status, _, _ = bot.get("/api/control/status")
        self.assertEqual(status, 401)
