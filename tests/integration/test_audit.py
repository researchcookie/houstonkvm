"""The audit log: what gets recorded, who may read it, and how long it lasts.

Events (sign-ins, account/token/target changes, control) come through
GET /api/audit/events; each stretch of driving through
GET /api/audit/control-sessions, with how many keys were pressed and how
much mouse input was sent. Never which keys: nothing typed on a target is
kept anywhere. Only Owners may read either.
"""
import signal
import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import Client, HoustonKVMServer, ServerTestCase
from test_control import ControlTestBase
from test_targets import OWNER, TargetTestBase, login


class AuditReader:
    """Mixin: read the audit log as the Owner."""

    def events(self, **query):
        qs = "&".join(f"{k}={v}" for k, v in query.items())
        status, raw, _ = self.owner.get("/api/audit/events" + (f"?{qs}" if qs else ""))
        self.assertEqual(status, 200, raw)
        return self.owner.as_json(raw)

    def latest(self, type_, **query):
        rows = self.events(type=type_, **query)["events"]
        self.assertTrue(rows, f"no {type_} event recorded")
        return rows[0]

    def sessions(self, **query):
        qs = "&".join(f"{k}={v}" for k, v in query.items())
        status, raw, _ = self.owner.get("/api/audit/control-sessions" + (f"?{qs}" if qs else ""))
        self.assertEqual(status, 200, raw)
        return self.owner.as_json(raw)["sessions"]


class TestAccountEvents(AuditReader, TargetTestBase):
    def test_sign_ins_succeeded_and_failed(self):
        c = Client(self.server.port)
        self.assertEqual(c.post("/api/login", {"username": "viewer1", "password": "nope-nope"})[0], 401)
        e = self.latest("auth.login")
        self.assertEqual((e["outcome"], e["user"]["username"], e["detail"]["reason"]),
                         ("denied", "viewer1", "wrong_password"))
        self.assertEqual(e["ip"], "127.0.0.1")

        self.assertEqual(c.post("/api/login", {"username": "ghost", "password": "whatever1"})[0], 401)
        e = self.latest("auth.login")
        self.assertEqual((e["user"], e["detail"]["reason"]),
                         ({"id": None, "username": "ghost"}, "unknown_user"))

        login(c, ("viewer1", "viewerpassword1"))
        e = self.latest("auth.login")
        self.assertEqual((e["outcome"], e["user"]["username"], e["via"]), ("ok", "viewer1", "session"))
        self.assertIsInstance(e["user"]["id"], int)

        self.assertEqual(c.post("/api/logout")[0], 200)
        self.assertEqual(self.latest("auth.logout")["user"]["username"], "viewer1")

    def test_accounts_and_roles(self):
        status, raw, _ = self.owner.post("/api/users", {"username": "temp", "password": "temppassword1",
                                                        "role": "viewer"})
        self.assertEqual(status, 200, raw)
        uid = self.owner.as_json(raw)["id"]
        e = self.latest("user.create")
        self.assertEqual((e["user"]["username"], e["detail"]),
                         (OWNER[0], {"user_id": uid, "username": "temp", "role": "viewer"}))

        self.owner.put(f"/api/users/{uid}/role", {"role": "operator"})
        self.assertEqual(self.latest("user.role_change")["detail"],
                         {"user_id": uid, "username": "temp", "from": "viewer", "to": "operator"})
        # Setting the role it already has isn't a change.
        before = len(self.events(type="user.role_change")["events"])
        self.owner.put(f"/api/users/{uid}/role", {"role": "operator"})
        self.assertEqual(len(self.events(type="user.role_change")["events"]), before)

        self.owner.delete(f"/api/users/{uid}")
        self.assertEqual(self.latest("user.delete")["detail"]["username"], "temp")

    def test_password_changes_and_tokens(self):
        c = self._make_user("pwaudit", "pwauditpassword", "viewer")
        c.post("/api/account/password", {"current_password": "wrong one!", "new_password": "whatever12"})
        self.assertEqual(self.latest("auth.password_change")["outcome"], "denied")
        c.post("/api/account/password", {"current_password": "pwauditpassword", "new_password": "whatever12"})
        e = self.latest("auth.password_change")
        self.assertEqual((e["outcome"], e["user"]["username"]), ("ok", "pwaudit"))
        self.assertIn("signed_out_sessions", e["detail"])

        status, raw, _ = c.post("/api/tokens", {"label": "backup-job"})
        made = c.as_json(raw)
        tid = made["id"]
        self.assertEqual(self.latest("token.create")["detail"],
                         {"token_id": tid, "label": "backup-job", "scope": "full",
                          "expires_at": made["expires_at"]})
        c.delete(f"/api/tokens/{tid}")
        self.assertEqual(self.latest("token.revoke")["detail"], {"token_id": tid})

    def test_target_changes_name_what_changed(self):
        t = self.owner.as_json(self.owner.post("/api/targets", self.mk("Audited", group="Lab"))[1])
        e = self.latest("target.create")
        self.assertEqual((e["target"], e["detail"]["group"]), ({"id": t["id"], "name": "Audited"}, "Lab"))

        self.owner.put(f"/api/targets/{t['id']}", {"name": "Audited 2", "tags": ["prod"]})
        e = self.latest("target.update")
        self.assertEqual(e["detail"]["changes"], {
            "name": {"from": "Audited", "to": "Audited 2"},
            "tags": {"from": [], "to": ["prod"]},
        })
        self.owner.delete(f"/api/targets/{t['id']}")
        e = self.latest("target.delete")
        self.assertEqual(e["target"]["name"], "Audited 2")

    def test_filters_and_paging(self):
        for i in range(3):
            Client(self.server.port).post("/api/login", {"username": "nobody", "password": f"guess-{i}xx"})
        auth_only = self.events(type="auth.")["events"]
        self.assertTrue(auth_only)
        self.assertTrue(all(e["type"].startswith("auth.") for e in auth_only))

        page1 = self.events(type="auth.login", limit=2)
        self.assertEqual(len(page1["events"]), 2)
        page2 = self.events(type="auth.login", limit=2, before=page1["next_before"])
        self.assertTrue(page2["events"])
        self.assertLess(page2["events"][0]["id"], page1["events"][-1]["id"])

        for bad in ("limit=0", "user_id=abc", "before=-1"):
            with self.subTest(bad=bad):
                self.assertEqual(self.owner.get(f"/api/audit/events?{bad}")[0], 400)

    def test_only_owners_read_it_and_tokens_work(self):
        for path in ("/api/audit/events", "/api/audit/control-sessions"):
            with self.subTest(path=path):
                self.assertEqual(self.viewer.get(path)[0], 403)
                self.assertEqual(self.operator.get(path)[0], 403)
                self.assertEqual(Client(self.server.port).get(path)[0], 401)
        # A collector reads it with an Owner's API token.
        token = self.owner.as_json(self.owner.post("/api/tokens", {"label": "collector"})[1])["token"]
        status, raw, _ = Client(self.server.port).get(
            "/api/audit/events?type=token.", headers={"Authorization": f"Bearer {token}"}, use_cookie=False)
        self.assertEqual(status, 200, raw)


class TestControlSessions(AuditReader, ControlTestBase):
    def type_(self, client, keys):
        for code in keys:
            self.key(client, self.A(), code, True)
            self.key(client, self.A(), code, False)

    def test_a_session_counts_activity_and_says_how_it_ended(self):
        self.acquire(self.op1, self.A())
        self.type_(self.op1, ["KeyH", "KeyI"])
        ws = self.op1.ws(f"/api/targets/{self.A()}/input/ws")
        ws.send_json({"type": "key", "code": "Enter", "pressed": True})
        ws.send_json({"type": "key", "code": "Enter", "pressed": False})
        ws.send_json({"type": "mousemove", "x": 0.5, "y": 0.5})
        ws.send_json({"type": "mousebutton", "button": 1, "pressed": True})
        ws.close()
        time.sleep(0.3)
        self.release(self.op1, self.A())

        s = self.sessions(target_id=self.A())[0]
        self.assertEqual((s["user"]["username"], s["end_reason"], s["key_count"], s["mouse_count"]),
                         ("operator1", "released", 3, 2))
        self.assertIsNotNone(s["ended_at"])
        acquired = self.latest("control.acquire", target_id=self.A())
        released = self.latest("control.release", target_id=self.A())
        self.assertEqual(acquired["detail"]["control_session"], s["id"])
        self.assertEqual(released["detail"], {"control_session": s["id"], "reason": "released"})
        self.assertEqual(released["target"]["name"], "Chip A")

    def test_what_was_typed_is_kept_nowhere(self):
        # Keys nothing else in this suite sends, so finding them anywhere
        # means the audit log kept them. Codes of four characters or more:
        # a two-byte one like "F9" turns up by chance in the database file's
        # random bytes (hashes, salts) often enough to fail a run now and then.
        secret = ["KeyJ", "KeyV", "Numpad7", "ScrollLock"]
        self.acquire(self.op1, self.A())
        self.type_(self.op1, secret)
        self.release(self.op1, self.A())
        time.sleep(1.2)   # past a flush

        sessions = self.owner.get("/api/audit/control-sessions")[1]
        events = self.owner.get("/api/audit/events?limit=500")[1]
        self.assertEqual(self.owner.get("/api/audit/control-sessions/1")[0], 404)
        with sqlite3.connect(self.server.db_path) as db:
            tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type = 'table'")}
        self.assertNotIn("audit_keystrokes", tables)
        stored = b"".join(Path(self.server.db_path + suffix).read_bytes()
                          for suffix in ("", "-wal") if Path(self.server.db_path + suffix).exists())
        for code in secret:
            with self.subTest(code=code):
                self.assertNotIn(code.encode(), sessions)
                self.assertNotIn(code.encode(), events)
                self.assertNotIn(code.encode(), stored)

    def test_an_owner_override_is_the_owners_act(self):
        self.acquire(self.op2, self.A())
        self.type_(self.op2, ["KeyQ"])
        self.assertEqual(self.owner.post(f"/api/targets/{self.A()}/control/release")[0], 200)
        s = self.sessions(target_id=self.A())[0]
        self.assertEqual((s["user"]["username"], s["end_reason"]), ("operator2", "owner_override"))
        forced = self.latest("control.force_release")
        self.assertEqual((forced["user"]["username"], forced["detail"]),
                         (OWNER[0], {"driver": "operator2", "control_session": s["id"]}))

    def test_signing_out_ends_the_session(self):
        op = self.new_operator("audit_signout")
        self.acquire(op, self.A())
        op.post("/api/logout")
        s = self.sessions(target_id=self.A())[0]
        self.assertEqual((s["user"]["username"], s["end_reason"]), ("audit_signout", "signed_out"))

    def test_revoked_credentials_end_the_session(self):
        op = self.new_operator("audit_revoked")
        self.acquire(op, self.A())
        self.owner.put(f"/api/users/{self.user_id('audit_revoked')}/role", {"role": "viewer"})
        self.wait_for(lambda: self.sessions(target_id=self.A())[0]["end_reason"] == "credentials_revoked",
                      timeout=10, what="the demoted driver's session to end")

    def test_filter_by_driver(self):
        op = self.new_operator("audit_filter")
        self.acquire(op, self.B())
        self.release(op, self.B())
        uid = self.user_id("audit_filter")
        rows = self.sessions(user_id=uid)
        self.assertEqual({r["user"]["username"] for r in rows}, {"audit_filter"})


class TestShutdownAndCrash(AuditReader, ControlTestBase):
    """Last in the file: these stop the server."""

    def restart(self, sig):
        self.server.proc.send_signal(sig)
        self.server.proc.wait(timeout=15)
        self.server._drainer.join(timeout=5)

    def db(self):
        return sqlite3.connect(self.server.db_path)

    def test_1_a_crash_leaves_the_session_interrupted_with_its_counts(self):
        self.acquire(self.op1, self.A())
        self.key(self.op1, self.A(), "KeyZ", True)
        self.key(self.op1, self.A(), "KeyZ", False)
        time.sleep(1.5)   # counts are written once a second
        self.restart(signal.SIGKILL)
        self.server.start()
        with self.db() as db:
            reason, ended, keys = db.execute(
                "SELECT end_reason, ended_at, key_count FROM audit_control_sessions "
                "ORDER BY id DESC").fetchone()
        self.assertEqual((reason, ended, keys), ("interrupted", None, 1))

    def test_2_a_clean_stop_ends_open_sessions_with_their_counts(self):
        owner = Client(self.server.port)
        login(owner, OWNER)
        op = Client(self.server.port)
        login(op, ("operator1", "operatorpassword1"))
        # The chips' fake adapters came back with the server; wait for input.
        self._wait_ready(self.A())
        status, raw, _ = op.post(f"/api/targets/{self.A()}/control/acquire")
        self.assertEqual(status, 200, raw)
        op.post(f"/api/targets/{self.A()}/input", {"type": "key", "code": "KeyS", "pressed": True})
        self.restart(signal.SIGTERM)   # no wait: stop() must write the counts
        with self.db() as db:
            reason, keys = db.execute(
                "SELECT end_reason, key_count FROM audit_control_sessions ORDER BY id DESC").fetchone()
        self.assertEqual((reason, keys), ("server_stopped", 1))
        self.server.start()   # for tearDownClass


class TestRetention(ServerTestCase):
    def test_old_records_are_pruned_at_startup(self):
        server = HoustonKVMServer(extra_args=["--audit-retention-days=30"])
        server.start()
        self.addCleanup(server.stop)
        server.setup_owner(Client(server.port), "owner", "correct horse battery")
        server.proc.terminate()
        server.proc.wait(timeout=10)
        server._drainer.join(timeout=5)
        with sqlite3.connect(server.db_path) as db:
            db.execute("INSERT INTO audit_events (at, type, outcome) VALUES "
                       "('2001-01-01T00:00:00.000Z', 'auth.login', 'ok')")
            db.execute("INSERT INTO audit_control_sessions (id, target_id, started_at, ended_at, end_reason) "
                       "VALUES (500, 1, '2001-01-01T00:00:00.000Z', '2001-01-01T00:01:00.000Z', 'released')")
        server.start()
        with sqlite3.connect(server.db_path) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events WHERE at < '2002'").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_events").fetchone()[0], 1)   # setup
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_control_sessions").fetchone()[0], 0)

    def test_bad_retention_is_refused(self):
        import subprocess
        for bad in ("0", "abc", "36501", "7days"):
            with self.subTest(bad=bad):
                r = subprocess.run([self.server.binary, f"--audit-retention-days={bad}"],
                                   capture_output=True, text=True, timeout=10)
                self.assertEqual(r.returncode, 2, r.stderr)
