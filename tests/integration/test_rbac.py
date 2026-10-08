"""Role-based access control: Viewer < Operator < Owner.

The input-control tests below assume /dev/hidg0 (the USB HID gadget device)
isn't present — true on any normal dev/CI machine, since that only exists
on an ARM board in USB-OTG gadget mode. That's what lets them tell apart
"blocked by role" (403, checked first by every route) from "blocked by
missing hardware" (503, checked only after the role check passes) — see
routes_input.cpp's ordering of requireRole() before the input-backend
readiness check.
"""
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import Client, ServerTestCase


class TestRoleBasedAccess(ServerTestCase):
    # /api/setup only ever succeeds once per server (first user wins), so
    # the owner is created once in setUpClass and every test method logs
    # into that same account rather than re-running setup itself.
    OWNER_USERNAME = "owner"
    OWNER_PASSWORD = "ownerpassword1"

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        bootstrap = Client(cls.server.port)
        status, raw, _ = cls.server.setup_owner(bootstrap, cls.OWNER_USERNAME, cls.OWNER_PASSWORD)
        assert status == 200, raw

    def setUp(self):
        super().setUp()
        self.owner = self.new_client()
        status, raw, _ = self.owner.post(
            "/api/login", {"username": self.OWNER_USERNAME, "password": self.OWNER_PASSWORD}
        )
        assert status == 200, raw

        # Per-test-unique usernames since every test method shares the one
        # class-level server/db — short hashed tag rather than the raw
        # method name, since isValidUsername() caps usernames at 64 chars
        # and some of these test method names alone are longer than that.
        self.viewer = self.new_client()
        self.operator = self.new_client()
        self._create_user(self.owner, self._viewer_username(), "viewerpassword1", "viewer")
        self._create_user(self.owner, self._operator_username(), "operatorpassword1", "operator")
        self.viewer.post(
            "/api/login", {"username": self._viewer_username(), "password": "viewerpassword1"}
        )
        self.operator.post(
            "/api/login", {"username": self._operator_username(), "password": "operatorpassword1"}
        )

    def _tag(self):
        return hashlib.sha1(self._testMethodName.encode()).hexdigest()[:10]

    def _viewer_username(self):
        return "viewer_" + self._tag()

    def _operator_username(self):
        return "operator_" + self._tag()

    def _create_user(self, actor, username, password, role):
        status, raw, _ = actor.post(
            "/api/users", {"username": username, "password": password, "role": role}
        )
        self.assertEqual(status, 200, raw)
        return actor.as_json(raw)

    def test_admin_settings_forbidden_for_viewer_and_operator(self):
        for client in (self.viewer, self.operator):
            status, _, _ = client.get("/api/admin/settings")
            self.assertEqual(status, 403)
            status, _, _ = client.put("/api/admin/settings", {"v4l2_device": "/dev/video0"})
            self.assertEqual(status, 403)

    def test_video_settings_and_capabilities_forbidden_for_viewer(self):
        status, _, _ = self.viewer.get("/api/video/settings")
        self.assertEqual(status, 403)
        status, _, _ = self.viewer.put("/api/video/settings", {"capture_width": 1280})
        self.assertEqual(status, 403)
        status, _, _ = self.viewer.get("/api/video/capabilities")
        self.assertEqual(status, 403)

    def test_video_settings_readable_by_operator_and_owner(self):
        # Read-only here for the same reason as test_admin_settings_readable_
        # by_owner below: a PUT queues a StreamManager rebuild that's slow
        # enough to make this class's rapid-fire shared-server pattern flaky.
        for client in (self.operator, self.owner):
            status, raw, _ = client.get("/api/video/settings")
            self.assertEqual(status, 200, raw)
            body = client.as_json(raw)
            self.assertIn("capture_width", body)
            self.assertIn("capture_height", body)
            self.assertIn("capture_fps", body)
            # active reports the live negotiated mode (null when no capture
            # is running, e.g. the configured device doesn't exist) rather
            # than echoing the saved request back — see routes_settings.cpp.
            self.assertIn("active", body)

            status, raw, _ = client.get("/api/video/capabilities")
            self.assertEqual(status, 200, raw)
            caps = client.as_json(raw)
            self.assertIn("modes", caps)
            self.assertIsInstance(caps["modes"], list)

    def test_admin_settings_readable_by_owner(self):
        # Read-only on purpose — the PUT round-trip lives in
        # TestAdminSettingsRebuild below, on its own dedicated server. A
        # PUT here queues a StreamManager rebuild (new SfuPublisher, input
        # backend, capture device) that's documented as blocking "up to
        # ~10s" but was observed taking substantially longer under this
        # suite's rapid-fire request pattern — sharing one server across
        # ~13 test methods in this class, that turned into flaky timeouts
        # on completely unrelated later requests.
        status, raw, _ = self.owner.get("/api/admin/settings")
        self.assertEqual(status, 200, raw)
        current = self.owner.as_json(raw)
        self.assertIn("v4l2_device", current)

    def test_viewer_cannot_acquire_control(self):
        status, _, _ = self.viewer.post("/api/control/acquire")
        self.assertEqual(status, 403)

    def test_owner_cannot_acquire_control(self):
        # requireExactRole(Role::Operator) — Owner does not auto-qualify as
        # a driver, unlike every other Owner-vs-lower-role check in this
        # API (which is "at least"). Piloting the target is deliberately a
        # separate capability from administering the server.
        status, raw, _ = self.owner.post("/api/control/acquire")
        self.assertEqual(status, 403, raw)

    def test_operator_acquire_fails_on_missing_hardware_not_role(self):
        # Exact-role check passes (Operator is exactly Operator), so this
        # gets past requireExactRole and hits the hardware-readiness check
        # next — 503, not 403, proving the role gate isn't what's blocking it.
        status, raw, _ = self.operator.post("/api/control/acquire")
        self.assertEqual(status, 503, raw)

    def test_operator_input_without_control_is_forbidden(self):
        status, _, _ = self.operator.post("/api/input", {"type": "mousemove", "x": 0.5, "y": 0.5})
        self.assertEqual(status, 403)

    def test_viewer_input_forbidden_by_role_before_driver_check(self):
        status, _, _ = self.viewer.post("/api/input", {"type": "mousemove", "x": 0.5, "y": 0.5})
        self.assertEqual(status, 403)

    def test_owner_input_forbidden_by_role_before_driver_check(self):
        status, _, _ = self.owner.post("/api/input", {"type": "mousemove", "x": 0.5, "y": 0.5})
        self.assertEqual(status, 403)

    # /api/input/ws mirrors /api/input's role gate, just moved to upgrade
    # time instead of per-request: same exact-Operator-role check
    # (requireExactRole), so Viewer/Owner never reach 101 Switching
    # Protocols. See server_harness.WsClient for the hand-rolled RFC6455
    # client this drives it with (no pip dependency for a WS client here).

    def test_viewer_input_ws_upgrade_forbidden(self):
        ws = self.viewer.ws("/api/input/ws")
        try:
            self.assertFalse(ws.upgraded)
            self.assertEqual(ws.status, 403)
        finally:
            ws.close()

    def test_owner_input_ws_upgrade_forbidden(self):
        ws = self.owner.ws("/api/input/ws")
        try:
            self.assertFalse(ws.upgraded)
            self.assertEqual(ws.status, 403)
        finally:
            ws.close()

    def test_anonymous_input_ws_upgrade_forbidden(self):
        anon = self.new_client()
        ws = anon.ws("/api/input/ws")
        try:
            self.assertFalse(ws.upgraded)
            self.assertEqual(ws.status, 401)
        finally:
            ws.close()

    def test_operator_input_ws_upgrades_but_message_forbidden_without_control(self):
        # Same story as test_operator_input_without_control_is_forbidden's
        # HTTP 403, moved to the WS transport: the upgrade itself only
        # checks role, so it succeeds — but no /api/control/acquire ever
        # happened (and can't on a hardware-less test box, see this file's
        # module docstring), so TargetRuntime::driverToken is empty and the
        # per-message check must still reject the mousemove.
        ws = self.operator.ws("/api/input/ws")
        try:
            self.assertTrue(ws.upgraded, ws.status_line)
            ws.send_json({"type": "mousemove", "x": 0.5, "y": 0.5})
            reply = ws.recv_json()
            self.assertEqual(reply, {"ok": False, "reason": "not_driver"})
        finally:
            ws.close()

    def test_operator_input_ws_malformed_json_does_not_drop_connection(self):
        ws = self.operator.ws("/api/input/ws")
        try:
            self.assertTrue(ws.upgraded, ws.status_line)
            # The driver check runs before JSON parsing (same order as the
            # HTTP path implicitly has, via requireExactRole before onData),
            # so a non-driver's malformed payload still reports not_driver
            # rather than bad_json — this asserts that ordering holds and,
            # more importantly, that the connection survives either way.
            ws.send_text_raw("{not valid json")
            reply = ws.recv_json()
            self.assertIn(reply["reason"], ("not_driver", "bad_json"))
            self.assertFalse(reply["ok"])
        finally:
            ws.close()

    def test_control_status_visible_to_viewer(self):
        status, raw, _ = self.viewer.get("/api/control/status")
        self.assertEqual(status, 200, raw)
        body = self.viewer.as_json(raw)
        self.assertIn("controlled", body)
        self.assertIn("is_driver", body)

    def test_user_management_forbidden_for_non_owners(self):
        for client in (self.viewer, self.operator):
            status, _, _ = client.get("/api/users")
            self.assertEqual(status, 403)
            status, _, _ = client.post(
                "/api/users", {"username": "nope", "password": "irrelevant1", "role": "viewer"}
            )
            self.assertEqual(status, 403)

    def test_owner_can_list_and_manage_users(self):
        status, raw, _ = self.owner.get("/api/users")
        self.assertEqual(status, 200, raw)
        usernames = {u["username"] for u in self.owner.as_json(raw)}
        self.assertIn(self._viewer_username(), usernames)
        self.assertIn(self._operator_username(), usernames)

    def test_sole_owner_cannot_demote_self(self):
        status, raw, _ = self.owner.get("/api/me")
        my_id = self.owner.as_json(raw)["id"]

        status, raw, _ = self.owner.put(f"/api/users/{my_id}/role", {"role": "viewer"})
        self.assertEqual(status, 409, raw)

    def test_sole_owner_cannot_delete_self(self):
        status, raw, _ = self.owner.get("/api/me")
        my_id = self.owner.as_json(raw)["id"]

        status, raw, _ = self.owner.delete(f"/api/users/{my_id}")
        self.assertEqual(status, 400, raw)

    def test_demoting_a_non_last_owner_succeeds(self):
        extra = self._create_user(
            self.owner, "extra_owner_" + self._tag(), "extraownerpass1", "owner"
        )
        status, raw, _ = self.owner.put(f"/api/users/{extra['id']}/role", {"role": "viewer"})
        self.assertEqual(status, 200, raw)

    def test_owner_can_delete_a_non_owner_user(self):
        status, raw, _ = self.owner.get("/api/users")
        target = next(
            u for u in self.owner.as_json(raw) if u["username"] == self._viewer_username()
        )
        status, raw, _ = self.owner.delete(f"/api/users/{target['id']}")
        self.assertEqual(status, 200, raw)

        status, raw, _ = self.owner.get("/api/users")
        usernames = {u["username"] for u in self.owner.as_json(raw)}
        self.assertNotIn(self._viewer_username(), usernames)


class TestAdminSettingsRebuild(ServerTestCase):
    """PUT /api/admin/settings on its own server, isolated from every other
    RBAC test — see the comment on test_admin_settings_readable_by_owner
    above for why sharing a server with a settings-changing PUT caused
    flaky timeouts elsewhere."""

    def test_put_admin_settings_persists(self):
        status, raw, _ = self.server.setup_owner(self.client, "owner", "ownerpassword1")
        self.assertEqual(status, 200, raw)

        status, raw, _ = self.client.put(
            "/api/admin/settings",
            {
                "v4l2_device": "/dev/video7",
                "capture_width": 1920,
                "capture_height": 1080,
                "capture_fps": 24,
                "qmp_socket": "",
                "serial_device": "",
                "serial_baud": 115200,
                "webrtc_bitrate_kbps": 3500,
            },
        )
        self.assertEqual(status, 200, raw)

        status, raw, _ = self.client.get("/api/admin/settings")
        updated = self.client.as_json(raw)
        self.assertEqual(updated["v4l2_device"], "/dev/video7")
        self.assertEqual(updated["capture_fps"], 24)
        self.assertEqual(updated["webrtc_bitrate_kbps"], 3500)
