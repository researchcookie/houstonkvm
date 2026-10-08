"""Multi-target support: the /api/targets directory (list/search/facets/CRUD),
default-target handling and its unscoped-route aliases, device-conflict
guards, target lifecycle, and the upgrade path from a single-target database.

Fixtures are created disabled (enabled=False) unless a test is specifically
about a running target: an enabled target spins up its capture, H.264 encode
and WebRTC pipeline, which none of the metadata tests need — and which would
make server shutdown at the end of the class slow.

The scoped-route error tests rely on no capture hardware being present (true
on any dev/CI box): a running target with no /dev/videoN answers a snapshot
with 503 "No capture device", which is how "the target exists and is running"
is told apart from "disabled" (503 "Target is disabled") or "unknown" (404).
"""
import sqlite3
import sys
import time
import unittest
from pathlib import Path
from urllib.parse import quote

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import Client, HoustonKVMServer, ServerTestCase

OWNER = ("owner", "ownerpassword1")


def login(client, creds):
    status, raw, _ = client.post("/api/login", {"username": creds[0], "password": creds[1]})
    assert status == 200, raw


class TargetTestBase(ServerTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        bootstrap = Client(cls.server.port)
        status, raw, _ = cls.server.setup_owner(bootstrap, *OWNER)
        assert status == 200, raw

        cls.owner = Client(cls.server.port)
        login(cls.owner, OWNER)
        cls.viewer = cls._make_user("viewer1", "viewerpassword1", "viewer")
        cls.operator = cls._make_user("operator1", "operatorpassword1", "operator")

    @classmethod
    def _make_user(cls, name, password, role):
        status, raw, _ = cls.owner.post("/api/users", {"username": name, "password": password, "role": role})
        assert status == 200, raw
        client = Client(cls.server.port)
        login(client, (name, password))
        return client

    # -- helpers ---------------------------------------------------------
    _seq = 0

    @classmethod
    def mk(cls, name, **extra):
        """Body for a new target with device paths no other fixture uses.
        Disabled by default (see module docstring)."""
        TargetTestBase._seq += 1
        n = 100 + TargetTestBase._seq
        body = {
            "name": name,
            "enabled": False,
            "v4l2_device": f"/dev/video{n}",
            "serial_device": f"/dev/ttyUSB{n}",
        }
        body.update(extra)
        return body

    def create(self, name, **extra):
        status, raw, _ = self.owner.post("/api/targets", self.mk(name, **extra))
        self.assertEqual(status, 201, raw)
        return self.owner.as_json(raw)

    def list_targets(self, client=None, query=""):
        client = client or self.owner
        status, raw, _ = client.get("/api/targets" + query)
        self.assertEqual(status, 200, raw)
        return client.as_json(raw)["targets"]

    def names(self, query="", client=None):
        return [t["name"] for t in self.list_targets(client, query)]

    def wait_for(self, predicate, timeout=40, what="condition"):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate():
                return
            time.sleep(0.3)
        self.fail(f"timed out waiting for {what}")


class TestTargets(TargetTestBase):
    # Sequential state (fixtures built up, then the default deleted at the
    # end), so order is load-bearing — see the numbering.

    def test_01_migrated_default_target_exists(self):
        targets = self.list_targets()
        self.assertEqual(len(targets), 1)
        t = targets[0]
        self.assertEqual(t["name"], "Default target")
        self.assertTrue(t["default"])
        self.assertTrue(t["enabled"])
        self.assertEqual(t["state"], "running")
        self.assertEqual(t["config"]["v4l2_device"], "/dev/video0")

    def test_02_roles_and_config_visibility(self):
        anon = self.new_client()
        self.assertEqual(anon.get("/api/targets")[0], 401)

        for client in (self.viewer, self.operator):
            targets = self.list_targets(client)
            self.assertEqual(len(targets), 1)
            # Device paths are Owner-browser-only.
            self.assertNotIn("config", targets[0])
            self.assertEqual(client.post("/api/targets", self.mk("nope"))[0], 403)
            self.assertEqual(client.put("/api/targets/1", {"name": "nope"})[0], 403)
            self.assertEqual(client.delete("/api/targets/1")[0], 403)

        # An Owner's API token can list (bot discovery) but never sees wiring
        # and can't mutate — same bar as the admin routes' cookie-only rule.
        status, raw, _ = self.owner.post("/api/tokens", {"label": "bot"})
        bot = self.new_client()
        bot.bearer = self.owner.as_json(raw)["token"]
        targets = self.list_targets(bot)
        self.assertNotIn("config", targets[0])
        self.assertEqual(bot.post("/api/targets", self.mk("nope"))[0], 401)

    def test_03_create_normalizes_tags_and_get(self):
        t = self.create("Web Server 1", group="Rack 3", description="front door",
                        tags=[" Prod", "LINUX", "prod"])
        self.assertEqual(t["tags"], ["linux", "prod"])
        self.assertFalse(t["default"])
        self.assertEqual(t["state"], "disabled")

        status, raw, _ = self.viewer.get(f"/api/targets/{t['id']}")
        self.assertEqual(status, 200, raw)
        got = self.viewer.as_json(raw)
        self.assertEqual(got["name"], "Web Server 1")
        self.assertEqual(got["group"], "Rack 3")
        self.assertEqual(got["description"], "front door")
        self.assertEqual(self.viewer.get("/api/targets/99999")[0], 404)
        self.assertEqual(self.viewer.get("/api/targets/abc")[0], 400)

    def test_04_group_spelling_snaps_to_existing(self):
        t = self.create("DB Server", group="rack 3", tags=["prod", "windows"])
        self.assertEqual(t["group"], "Rack 3")  # not a second, differently-cased group
        groups = self.owner.as_json(self.owner.get("/api/targets/facets")[1])["groups"]
        self.assertEqual([g for g in groups if g["name"].lower() == "rack 3"],
                         [{"name": "Rack 3", "count": 2}])

    def test_05_conflicts_are_409(self):
        base = self.mk("Conflict probe")
        status, raw, _ = self.owner.post("/api/targets", {**base, "name": "web server 1"})
        self.assertEqual((status, b"name" in raw), (409, True), raw)  # case-insensitive

        existing = self.list_targets()
        web = next(t for t in existing if t["name"] == "Web Server 1")["config"]
        status, raw, _ = self.owner.post("/api/targets", {**base, "serial_device": web["serial_device"]})
        self.assertEqual((status, b"serial" in raw), (409, True), raw)
        status, raw, _ = self.owner.post("/api/targets", {**base, "v4l2_device": web["v4l2_device"]})
        self.assertEqual((status, b"capture device" in raw), (409, True), raw)
        status, raw, _ = self.owner.post(
            "/api/targets", {**base, "qmp_socket": "/run/q1.sock"})
        self.assertEqual(status, 201, raw)  # first user of that socket is fine
        status, raw, _ = self.owner.post(
            "/api/targets", {**base, "name": "Conflict probe 2", "qmp_socket": "/run/q1.sock",
                             "v4l2_device": "/dev/video190", "serial_device": ""})
        self.assertEqual((status, b"QEMU" in raw), (409, True), raw)

        # The default target already uses the local USB-gadget fallback (no
        # serial device, no QMP socket); a second one would fight over
        # /dev/hidg0 and /dev/hidg1.
        status, raw, _ = self.owner.post(
            "/api/targets", {**base, "name": "Gadget 2", "v4l2_device": "/dev/video191",
                             "serial_device": ""})
        self.assertEqual((status, b"gadget" in raw), (409, True), raw)

        self.assertEqual(self.names().count("Conflict probe"), 1)
        # A rejected create must not have left a half-written row behind.
        self.assertNotIn("Conflict probe 2", self.names())
        self.assertNotIn("Gadget 2", self.names())

    def test_06_validation(self):
        def bad(body_overrides, expect=400):
            status, raw, _ = self.owner.post("/api/targets", {**self.mk("Bad"), **body_overrides})
            self.assertEqual(status, expect, (body_overrides, raw))

        bad({"name": ""})
        bad({"name": "x" * 65})
        bad({"name": "line\nbreak"})
        bad({"name": 5})
        bad({"tags": "prod"})
        bad({"tags": [1]})
        bad({"tags": ["has space"]})
        bad({"tags": ["-leading"]})
        bad({"tags": ["a" * 33]})
        bad({"tags": [f"t{i}" for i in range(17)]})
        bad({"default": False})
        bad({"capture_width": -5})
        bad({"capture_fps": 500})
        bad({"v4l2_device": ""})
        status, raw, _ = self.owner.post("/api/targets", "{not json")
        self.assertEqual(status, 400, raw)
        self.assertNotIn("Bad", self.names())

    def test_07_search(self):
        self.create("Lab Box 100%", description="test bench", group="Lab", tags=["dev"])
        self.create("Ungrouped Thing", tags=["dev"])

        self.assertCountEqual(self.names("?q=server"), ["Web Server 1", "DB Server"])
        # Every term must match, but each may match a different field.
        self.assertEqual(self.names("?q=" + quote("web rack")), ["Web Server 1"])
        self.assertEqual(self.names("?q=windows"), ["DB Server"])          # tag
        self.assertEqual(self.names("?q=bench"), ["Lab Box 100%"])          # description
        self.assertEqual(self.names("?q=SERVER"), self.names("?q=server"))  # case-insensitive
        # LIKE wildcards in the query are literal, not patterns.
        self.assertEqual(self.names("?q=" + quote("%")), ["Lab Box 100%"])
        self.assertEqual(self.names("?q=" + quote("_")), [])
        self.assertEqual(self.names("?q=nomatchatall"), [])

        self.assertCountEqual(self.names("?tag=prod"), ["Web Server 1", "DB Server"])
        self.assertCountEqual(self.names("?tag=PROD"), ["Web Server 1", "DB Server"])
        self.assertCountEqual(self.names("?group=" + quote("RACK 3")), ["Web Server 1", "DB Server"])
        self.assertEqual(self.names("?tag=prod&q=windows"), ["DB Server"])
        self.assertNotIn("Web Server 1", self.names("?enabled=1"))
        self.assertEqual(self.names("?enabled=1"), ["Default target"])

        # Viewers search too (the directory is for everyone who can view).
        self.assertEqual(self.names("?q=windows", client=self.viewer), ["DB Server"])

        # Grouped alphabetically, ungrouped last.
        ordered = self.names()
        self.assertEqual(ordered[-2:], ["Default target", "Ungrouped Thing"])
        self.assertLess(ordered.index("Lab Box 100%"), ordered.index("DB Server"))

    def test_08_facets(self):
        status, raw, _ = self.viewer.get("/api/targets/facets")
        self.assertEqual(status, 200, raw)
        facets = self.viewer.as_json(raw)
        tags = {t["name"]: t["count"] for t in facets["tags"]}
        self.assertEqual(tags["prod"], 2)
        self.assertEqual(tags["dev"], 2)
        self.assertEqual(facets["tags"][0]["count"], max(tags.values()))  # most-used first
        groups = {g["name"]: g["count"] for g in facets["groups"]}
        self.assertEqual(groups["Rack 3"], 2)
        self.assertEqual(groups["Lab"], 1)
        self.assertNotIn("", groups)

    def test_09_partial_update(self):
        web = next(t for t in self.list_targets() if t["name"] == "Web Server 1")
        status, raw, _ = self.owner.put(f"/api/targets/{web['id']}", {"description": "updated"})
        self.assertEqual(status, 200, raw)
        got = self.owner.as_json(raw)
        self.assertEqual(got["description"], "updated")
        self.assertEqual(got["tags"], ["linux", "prod"])   # untouched
        self.assertEqual(got["group"], "Rack 3")            # untouched
        self.assertEqual(got["config"]["serial_device"], web["config"]["serial_device"])

        status, raw, _ = self.owner.put(f"/api/targets/{web['id']}", {"tags": []})
        self.assertEqual(self.owner.as_json(raw)["tags"], [])
        self.owner.put(f"/api/targets/{web['id']}", {"tags": ["linux", "prod"]})

        status, raw, _ = self.owner.put(f"/api/targets/{web['id']}", {"name": "db server"})
        self.assertEqual(status, 409, raw)
        self.assertEqual(self.owner.put("/api/targets/99999", {"name": "x"})[0], 404)
        self.assertEqual(self.owner.put(f"/api/targets/{web['id']}", {"default": False})[0], 400)

    def test_10_designating_a_default_moves_the_unscoped_aliases(self):
        web = next(t for t in self.list_targets() if t["name"] == "Web Server 1")
        legacy = self.owner.as_json(self.owner.get("/api/admin/settings")[1])
        self.assertEqual(legacy["v4l2_device"], "/dev/video0")  # still the migrated default

        status, raw, _ = self.owner.put(f"/api/targets/{web['id']}", {"default": True})
        self.assertEqual(status, 200, raw)
        self.assertTrue(self.owner.as_json(raw)["default"])
        defaults = [t["name"] for t in self.list_targets() if t["default"]]
        self.assertEqual(defaults, ["Web Server 1"])  # exactly one

        legacy = self.owner.as_json(self.owner.get("/api/admin/settings")[1])
        self.assertEqual(legacy["v4l2_device"], web["config"]["v4l2_device"])
        # ...and it's disabled, so the unscoped media routes say so rather
        # than quietly serving some other machine.
        status, raw, _ = self.owner.get("/api/snapshot")
        self.assertEqual((status, raw), (503, b"Target is disabled"))

        first = next(t for t in self.list_targets() if t["name"] == "Default target")
        self.owner.put(f"/api/targets/{first['id']}", {"default": True})
        self.assertEqual([t["name"] for t in self.list_targets() if t["default"]], ["Default target"])

    def test_11_scoped_route_errors(self):
        anon = self.new_client()
        # Auth is checked before the target is resolved, so an
        # unauthenticated caller can't probe which ids exist.
        self.assertEqual(anon.get("/api/targets/99999/snapshot")[0], 401)
        self.assertEqual(anon.get("/api/targets/1/snapshot")[0], 401)

        self.assertEqual(self.viewer.get("/api/targets/99999/snapshot")[0], 404)
        self.assertEqual(self.viewer.get("/api/targets/abc/snapshot")[0], 400)
        self.assertEqual(self.viewer.post("/api/targets/99999/webrtc/subscribe")[0], 404)

        disabled = next(t for t in self.list_targets() if t["name"] == "DB Server")
        status, raw, _ = self.viewer.get(f"/api/targets/{disabled['id']}/snapshot")
        self.assertEqual((status, raw), (503, b"Target is disabled"))
        status, raw, _ = self.viewer.get(f"/api/targets/{disabled['id']}/stream")
        self.assertEqual((status, raw), (503, b"Target is disabled"))

        # The default target is running but has no capture hardware here.
        status, raw, _ = self.viewer.get("/api/targets/1/snapshot")
        self.assertEqual((status, raw), (503, b"No capture device"))

    def test_12_scoped_video_settings(self):
        t = self.create("Tunable", group="Lab")
        tid = t["id"]
        status, raw, _ = self.operator.get(f"/api/targets/{tid}/video/settings")
        self.assertEqual(status, 200, raw)
        got = self.operator.as_json(raw)
        self.assertEqual(got["capture_width"], 1280)
        self.assertIsNone(got["active"])  # disabled: nothing running

        status, raw, _ = self.operator.put(
            f"/api/targets/{tid}/video/settings",
            {"capture_width": 1920, "capture_height": 1080, "v4l2_device": "/dev/video7"})
        self.assertEqual(status, 200, raw)
        cfg = self.owner.as_json(self.owner.get(f"/api/targets/{tid}")[1])["config"]
        self.assertEqual((cfg["capture_width"], cfg["capture_height"]), (1920, 1080))
        # An Operator can retune the picture, never the wiring.
        self.assertEqual(cfg["v4l2_device"], t["config"]["v4l2_device"])

        self.assertEqual(self.viewer.get(f"/api/targets/{tid}/video/settings")[0], 403)
        self.assertEqual(self.operator.put(
            f"/api/targets/{tid}/video/settings", {"capture_fps": 9999})[0], 400)
        self.assertEqual(self.operator.get("/api/targets/99999/video/settings")[0], 404)

    def test_13_enable_disable_lifecycle(self):
        t = self.create("Toggle me")
        tid = t["id"]
        status, raw, _ = self.owner.put(f"/api/targets/{tid}", {"enabled": True})
        self.assertEqual(status, 200, raw)
        self.assertIn(self.owner.as_json(raw)["state"], ("running", "starting"))
        self.wait_for(lambda: self.viewer.get(f"/api/targets/{tid}/snapshot")[1] == b"No capture device",
                      what="target to start")

        # Metadata-only edit must not restart a running target.
        status, raw, _ = self.owner.put(f"/api/targets/{tid}", {"description": "still up"})
        self.assertEqual(self.owner.as_json(raw)["state"], "running")

        status, raw, _ = self.owner.put(f"/api/targets/{tid}", {"enabled": False})
        self.assertEqual(self.owner.as_json(raw)["state"], "disabled")
        self.assertEqual(self.viewer.get(f"/api/targets/{tid}/snapshot")[1], b"Target is disabled")

    def test_14_deleted_device_can_be_reused_immediately(self):
        # The old runtime is still shutting down (it can take seconds) when
        # the new target that wants its devices is created. The new one must
        # wait for the release rather than race it, open the device while
        # it's still held, and come up permanently without video.
        a = self.create("Swap A", enabled=True)
        devices = {k: a["config"][k] for k in ("v4l2_device", "serial_device")}
        self.assertEqual(self.owner.delete(f"/api/targets/{a['id']}")[0], 200)
        self.assertEqual(self.owner.get(f"/api/targets/{a['id']}")[0], 404)

        status, raw, _ = self.owner.post(
            "/api/targets", {"name": "Swap B", "enabled": True, **devices})
        self.assertEqual(status, 201, raw)
        b = self.owner.as_json(raw)
        self.wait_for(lambda: self.viewer.get(f"/api/targets/{b['id']}/snapshot")[1] == b"No capture device",
                      timeout=45, what="replacement target to start")
        # The retired target's id is gone for good.
        self.assertEqual(self.viewer.get(f"/api/targets/{a['id']}/snapshot")[0], 404)

    def test_15_deleting_the_default_leaves_unscoped_routes_failing_safe(self):
        self.assertEqual(self.owner.delete("/api/targets/99999")[0], 404)
        self.assertEqual(self.owner.delete("/api/targets/1")[0], 200)
        self.assertNotIn("Default target", self.names())

        # No default -> unscoped routes refuse instead of picking "some" target.
        for method, path in (("get", "/api/snapshot"), ("get", "/api/stream"),
                             ("post", "/api/webrtc/subscribe"), ("post", "/api/control/acquire"),
                             ("get", "/api/admin/settings")):
            client = self.owner if path.startswith("/api/admin") else self.operator
            status, raw, _ = getattr(client, method)(path)
            self.assertEqual((status, raw), (404, b"No default target configured"), path)
        # Probing a device the caller names needs no target: it's how the
        # admin UI fills in the first target's resolution/fps choices.
        status, raw, _ = self.operator.get("/api/video/capabilities?device=/dev/video99")
        self.assertEqual(status, 200, raw)
        self.assertEqual(self.operator.as_json(raw)["modes"], [])  # no such device here
        self.assertEqual(self.operator.get("/api/video/capabilities")[0], 404)  # no device named -> needs the default
        self.assertEqual(self.operator.get("/api/targets/99999/video/capabilities?device=/dev/video99")[0], 404)
        # The UI polls this every few seconds; "nobody's driving" is not an error.
        status, raw, _ = self.viewer.get("/api/control/status")
        self.assertEqual((status, self.viewer.as_json(raw)),
                          (200, {"controlled": False, "is_driver": False, "driver": "", "typing_remaining": 0}))

        # Explicit ids keep working, and re-designating restores the aliases.
        web = next(t for t in self.list_targets() if t["name"] == "Web Server 1")
        self.assertEqual(self.owner.put(f"/api/targets/{web['id']}", {"default": True})[0], 200)
        self.assertEqual(self.owner.get("/api/admin/settings")[0], 200)

    def test_16_first_target_after_deleting_all_becomes_default(self):
        for t in self.list_targets():
            self.assertEqual(self.owner.delete(f"/api/targets/{t['id']}")[0], 200)
        self.assertEqual(self.list_targets(), [])
        self.assertEqual(self.viewer.get("/api/targets/facets")[0], 200)

        t = self.create("Fresh start")
        self.assertTrue(t["default"])
        self.assertEqual(self.owner.get("/api/admin/settings")[0], 200)


class TestMigrationFromSingleTarget(unittest.TestCase):
    """A database written by the last single-target release (schema version
    4, one server_settings row) must come up with that configuration intact
    as the default target, so upgrading changes nothing an existing client
    or the UI can see."""

    OLD_SCHEMA = """
        CREATE TABLE users (
            id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT (datetime('now')),
            last_login_at TEXT, role TEXT NOT NULL DEFAULT 'owner');
        CREATE TABLE server_settings (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            v4l2_device TEXT NOT NULL DEFAULT '/dev/video0',
            capture_width INTEGER NOT NULL DEFAULT 1280,
            capture_height INTEGER NOT NULL DEFAULT 720,
            capture_fps INTEGER NOT NULL DEFAULT 30,
            qmp_socket TEXT NOT NULL DEFAULT '',
            serial_device TEXT NOT NULL DEFAULT '',
            serial_baud INTEGER NOT NULL DEFAULT 9600,
            webrtc_bitrate_kbps INTEGER NOT NULL DEFAULT 4000,
            updated_at TEXT NOT NULL DEFAULT (datetime('now')));
        INSERT INTO server_settings
            (id, v4l2_device, capture_width, capture_height, capture_fps,
             qmp_socket, serial_device, serial_baud, webrtc_bitrate_kbps)
            VALUES (1, '/dev/video7', 1920, 1080, 25, '', '/dev/ttyUSB3', 115200, 6500);
        PRAGMA user_version = 4;
    """

    def setUp(self):
        self.server = HoustonKVMServer()
        conn = sqlite3.connect(self.server.db_path)
        conn.executescript(self.OLD_SCHEMA)
        conn.close()
        self.server.start()
        self.addCleanup(self.server.stop)

        self.owner = Client(self.server.port)
        self.assertEqual(self.server.setup_owner(self.owner, *OWNER)[0], 200)
        login(self.owner, OWNER)

    def test_old_settings_become_the_default_target(self):
        status, raw, _ = self.owner.get("/api/targets")
        self.assertEqual(status, 200, raw)
        targets = self.owner.as_json(raw)["targets"]
        self.assertEqual(len(targets), 1)
        t = targets[0]
        self.assertTrue(t["default"])
        self.assertTrue(t["enabled"])
        self.assertEqual(t["config"], {
            "v4l2_device": "/dev/video7", "capture_width": 1920, "capture_height": 1080,
            "capture_fps": 25, "qmp_socket": "", "serial_device": "/dev/ttyUSB3",
            "serial_baud": 115200, "webrtc_bitrate_kbps": 6500, "audio_device": "auto",
        })
        # The pre-multi-target API answers exactly as before.
        legacy = self.owner.as_json(self.owner.get("/api/admin/settings")[1])
        self.assertEqual(legacy, t["config"])

    def test_migration_leaves_legacy_table_for_downgrade(self):
        # Stop the process but not the harness: stop() also deletes the
        # temp dir, and the db has to be read first.
        self.server.proc.terminate()
        self.server.proc.wait(timeout=15)
        conn = sqlite3.connect(self.server.db_path)
        try:
            row = conn.execute("SELECT v4l2_device FROM server_settings WHERE id = 1").fetchone()
            self.assertEqual(row[0], "/dev/video7")
            self.assertEqual(conn.execute("PRAGMA user_version").fetchone()[0], 10)
        finally:
            conn.close()


if __name__ == "__main__":
    unittest.main()
