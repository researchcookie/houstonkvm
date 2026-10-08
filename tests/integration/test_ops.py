"""What an operations team runs the server with: API tokens that expire and
can be limited to reading, GET /metrics for Prometheus, and JSON logs.
"""
import json
import re
import sqlite3
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import Client, HoustonKVMServer
from test_control import FakeChip
from test_health import HealthTestBase

# One sample line of Prometheus's text format: name, optional labels, value.
SAMPLE = re.compile(r'^([a-zA-Z_:][a-zA-Z0-9_:]*)(\{(?:[a-zA-Z_][a-zA-Z0-9_]*="(?:[^"\\]|\\.)*",?)*\})? '
                    r'(-?[0-9.e+-]+|\+Inf|NaN)$')


class OpsTestBase(HealthTestBase):
    def token(self, client, expect=200, **body):
        status, raw, _ = client.post("/api/tokens", body)
        self.assertEqual(status, expect, raw)
        return client.as_json(raw) if status == 200 else raw

    def bearer(self, token):
        bot = Client(self.server.port)
        bot.bearer = token
        return bot

    def expire(self, token_id):
        # Expiry is days away at the least, so the test moves it instead.
        with sqlite3.connect(self.server.db_path) as db:
            db.execute("UPDATE api_tokens SET expires_at = datetime('now', '-1 minute') WHERE id = ?",
                       (token_id,))

    def metrics(self, client=None):
        status, raw, _ = (client or self.owner).get("/metrics")
        self.assertEqual(status, 200, raw)
        return raw.decode()

    def sample(self, text, name, **labels):
        """The value of the sample with this name and at least these labels."""
        for line in text.splitlines():
            if not line.startswith(name + "{") and not line.startswith(name + " "):
                continue
            if all(f'{k}="{v}"' in line for k, v in labels.items()):
                return float(line.rsplit(" ", 1)[1])
        return None


class TestTokens(OpsTestBase):
    def test_expiry_defaults_to_90_days(self):
        t = self.token(self.owner, label="ci")
        self.assertEqual(t["scope"], "full")
        self.assertFalse(t["expired"])
        expires = time.mktime(time.strptime(t["expires_at"], "%Y-%m-%d %H:%M:%S"))
        days = (expires - time.mktime(time.gmtime())) / 86400
        self.assertAlmostEqual(days, 90, delta=1)
        listed = next(x for x in self.owner.as_json(self.owner.get("/api/tokens")[1]) if x["id"] == t["id"])
        self.assertEqual(listed["expires_at"], t["expires_at"])
        self.assertNotIn("token", listed)

    def test_never_and_custom_expiry(self):
        self.assertIsNone(self.token(self.owner, expires_in_days=None)["expires_at"])
        self.assertIsNotNone(self.token(self.owner, expires_in_days=1)["expires_at"])
        for bad in (0, -1, 3651, "30", 1.5):
            self.token(self.owner, expect=400, expires_in_days=bad)
        self.token(self.owner, expect=400, scope="admin")

    def test_an_expired_token_stops_working(self):
        t = self.token(self.owner)
        bot = self.bearer(t["token"])
        self.assertEqual(bot.get("/api/targets")[0], 200)
        self.expire(t["id"])
        status, raw, _ = bot.get("/api/targets")
        self.assertEqual(status, 401, raw)
        listed = next(x for x in self.owner.as_json(self.owner.get("/api/tokens")[1]) if x["id"] == t["id"])
        self.assertTrue(listed["expired"])

    def test_read_scope_can_look_but_not_touch(self):
        tid = self.target_with(FakeChip())
        self.wait_state(tid, "good")
        op_read = self.bearer(self.token(self.operator, scope="read")["token"])
        self.assertEqual(op_read.get(f"/api/targets/{tid}")[0], 200)
        self.assertEqual(op_read.get(f"/api/targets/{tid}/health")[0], 200)
        self.assertEqual(op_read.post(f"/api/targets/{tid}/control/acquire")[0], 403)

        # The Owner routes a token can reach: the audit log and metrics.
        owner_full = self.bearer(self.token(self.owner)["token"])
        owner_read = self.bearer(self.token(self.owner, scope="read")["token"])
        self.assertEqual(owner_read.get("/api/targets")[0], 200)
        self.assertEqual(owner_full.get("/api/audit/events")[0], 200)
        self.assertEqual(owner_read.get("/api/audit/events")[0], 403)
        self.assertEqual(owner_full.get("/metrics")[0], 200)
        self.assertEqual(owner_read.get("/metrics")[0], 403)

    def test_metrics_scope_reads_metrics_and_nothing_else(self):
        self.token(self.operator, expect=403, scope="metrics")
        self.token(self.viewer, expect=403, scope="metrics")
        bot = self.bearer(self.token(self.owner, scope="metrics", label="prometheus")["token"])
        status, raw, _ = bot.get("/metrics")
        self.assertEqual(status, 200, raw)
        for path in ("/api/targets", "/api/me", "/api/audit/events", "/api/control/status"):
            status, raw, _ = bot.get(path)
            self.assertEqual(status, 403, f"{path}: {raw}")

    def test_a_driver_whose_token_expires_loses_control(self):
        tid = self.target_with(FakeChip())
        self.wait_state(tid, "good")
        t = self.token(self.operator)
        bot = self.bearer(t["token"])
        self.assertEqual(bot.post(f"/api/targets/{tid}/control/acquire")[0], 200)
        self.expire(t["id"])
        self.wait_for(lambda: not self.listed(tid)["controlled"], timeout=10,
                      what="control to be released")

    def test_creation_is_audited_with_scope_and_expiry(self):
        t = self.token(self.owner, scope="read", expires_in_days=7, label="audited")
        events = self.owner.as_json(self.owner.get("/api/audit/events?type=token.create")[1])["events"]
        e = next(e for e in events if e["detail"].get("token_id") == t["id"])
        self.assertEqual(e["detail"]["scope"], "read")
        self.assertEqual(e["detail"]["expires_at"], t["expires_at"])
        self.assertNotIn(t["token"], json.dumps(events))


class TestMetrics(OpsTestBase):
    def test_who_may_read_them(self):
        self.assertEqual(Client(self.server.port).get("/metrics")[0], 401)
        self.assertEqual(self.viewer.get("/metrics")[0], 403)
        self.assertEqual(self.operator.get("/metrics")[0], 403)
        status, raw, resp = self.owner.get("/metrics")
        self.assertEqual(status, 200, raw)
        self.assertTrue(resp.getheader("content-type").startswith("text/plain; version=0.0.4"))

    def test_the_format_parses(self):
        self.target_with(FakeChip())
        text = self.metrics()
        types = {}
        for line in text.splitlines():
            if line.startswith("# TYPE "):
                _, _, name, kind = line.split(" ")
                self.assertNotIn(name, types, f"{name} declared twice")
                types[name] = kind
            elif not line.startswith("#"):
                m = SAMPLE.match(line)
                self.assertIsNotNone(m, f"not a sample: {line!r}")
                name = m.group(1)
                family = re.sub(r"_(bucket|sum|count)$", "", name) if name not in types else name
                self.assertIn(family, types, f"{name} has no TYPE")
        self.assertEqual(types["houstonkvm_event_loop_lag_seconds"], "histogram")
        self.assertEqual(types["houstonkvm_events_total"], "counter")

    def test_events_are_counted(self):
        def denied():
            return self.sample(self.metrics(), "houstonkvm_events_total",
                               type="auth.login", outcome="denied") or 0
        before = denied()
        Client(self.server.port).post("/api/login", {"username": "nobody", "password": "wrongpassword"})
        self.assertEqual(denied(), before + 1)

    def test_event_loop_lag_is_measured(self):
        time.sleep(0.5)
        text = self.metrics()
        self.assertGreater(self.sample(text, "houstonkvm_event_loop_lag_seconds_count"), 0)
        self.assertIsNotNone(self.sample(text, "houstonkvm_event_loop_lag_seconds_bucket", le="+Inf"))

    def test_targets_are_reported(self):
        tid = self.target_with(FakeChip())
        self.wait_state(tid, "good")
        text = self.metrics()
        self.assertEqual(self.sample(text, "houstonkvm_target_running", target_id=tid), 1)
        self.assertEqual(self.sample(text, "houstonkvm_target_input_health", target_id=tid, state="good"), 1)
        self.assertEqual(self.sample(text, "houstonkvm_target_input_health", target_id=tid, state="down"), 0)
        self.assertEqual(self.sample(text, "houstonkvm_target_controlled", target_id=tid), 0)
        self.assertEqual(self.sample(text, "houstonkvm_target_viewers", target_id=tid, transport="mjpeg"), 0)
        self.assertEqual(self.operator.post(f"/api/targets/{tid}/control/acquire")[0], 200)
        self.addCleanup(self.operator.post, f"/api/targets/{tid}/control/release")
        self.assertEqual(self.sample(self.metrics(), "houstonkvm_target_controlled", target_id=tid), 1)
        self.assertIsNotNone(self.sample(text, "process_resident_memory_bytes"))

    def test_target_names_are_escaped(self):
        tid = self.target_with(FakeChip())
        status, raw, _ = self.owner.put(f"/api/targets/{tid}", {"name": 'Rack "3" \\ left'})
        self.assertEqual(status, 200, raw)
        self.assertIn('target="Rack \\"3\\" \\\\ left"', self.metrics())


class TestJsonLogs(unittest.TestCase):
    def test_every_line_is_a_json_object(self):
        server = HoustonKVMServer(extra_args=["--log-format=json"])
        server.start()
        try:
            server.wait_for_output(r"listening on")
            server.wait_for_output(r"setup code")
        finally:
            server.stop()
        lines = [l for l in server.output if l.strip()]
        self.assertTrue(lines)
        for line in lines:
            entry = json.loads(line)
            self.assertEqual(set(entry) - {"component"}, {"ts", "level", "msg"}, line)
            self.assertRegex(entry["ts"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$")
            self.assertIn(entry["level"], ("info", "warning"))
        listening = next(json.loads(l) for l in lines if "listening on" in l)
        self.assertEqual(listening["level"], "info")
        code = next(json.loads(l) for l in lines if "setup code" in l)
        self.assertRegex(code["msg"], r"setup code: \S+$")


if __name__ == "__main__":
    unittest.main()
