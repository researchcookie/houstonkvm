"""Password hashing off the event loop, and what a password change does to
the account's other sessions.

Argon2 runs on worker threads (PasswordHasher), so these check the
consequences: the loop keeps answering while hashes are in flight,
overlapping attempts still can't beat the sign-in lockout, and racing setup
requests still create only one Owner. They also check that a password change
signs out the account's other sessions (and takes control away from them),
and that expired sessions are purged.
"""
import sqlite3
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import Client, HoustonKVMServer, ServerTestCase
from test_control import KEY_A, ControlTestBase

PASSWORD = "correct horse battery"


def parallel(n, fn):
    """Runs fn(i) for i in range(n) at (as near as possible) the same time."""
    start = threading.Barrier(n)

    def run(i):
        start.wait()
        return fn(i)

    with ThreadPoolExecutor(max_workers=n) as pool:
        return list(pool.map(run, range(n)))


class TestSetupRace(ServerTestCase):
    def test_racing_setups_create_exactly_one_owner(self):
        code = self.server.setup_code
        statuses = parallel(8, lambda i: Client(self.server.port).post(
            "/api/setup", {"username": f"owner{i}", "password": PASSWORD, "setup_code": code})[0])
        self.assertEqual(sorted(statuses), [200] + [409] * 7, statuses)
        with sqlite3.connect(self.server.db_path) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM users").fetchone()[0], 1)


class TestLoginOffTheLoop(ServerTestCase):
    """Sign-ins from many addresses (through a trusted proxy, so each gets
    its own rate-limit bucket) keep the hashing threads busy."""

    @classmethod
    def setUpClass(cls):
        cls.server = HoustonKVMServer(extra_args=["--trusted-proxy=127.0.0.1"])
        cls.server.start()
        cls.owner = Client(cls.server.port)
        status, raw, _ = cls.server.setup_owner(cls.owner, "owner", PASSWORD)
        assert status == 200, raw

    def login_from(self, addr, password):
        return Client(self.server.port).post(
            "/api/login", {"username": "owner", "password": password},
            headers={"X-Forwarded-For": addr})[0]

    def test_the_loop_answers_while_hashes_are_in_flight(self):
        burst_done = threading.Event()
        burst_took = []

        def burst():
            t0 = time.monotonic()
            parallel(16, lambda i: self.login_from(f"198.51.100.{i + 1}", "wrong password"))
            burst_took.append(time.monotonic() - t0)
            burst_done.set()

        worker = threading.Thread(target=burst)
        worker.start()
        time.sleep(0.05)   # let the burst reach the hashing queue
        probes = []
        while not burst_done.is_set():
            t0 = time.monotonic()
            status, _, _ = self.owner.get("/api/me")
            self.assertEqual(status, 200)
            probes.append(time.monotonic() - t0)
        worker.join()
        # Hashing inline, a request arriving mid-burst waited behind every
        # queued hash; now it's answered while they run.
        self.assertTrue(probes, "the burst finished before a probe could be sent")
        self.assertLess(max(probes), burst_took[0] / 2,
                        f"slowest probe {max(probes):.3f}s vs burst {burst_took[0]:.3f}s")

    def test_parallel_guesses_cannot_beat_the_lockout(self):
        statuses = parallel(20, lambda i: self.login_from("203.0.113.7", "wrong password"))
        self.assertLessEqual(statuses.count(401), 5, statuses)
        self.assertEqual(set(statuses) - {401, 429}, set(), statuses)
        # And the address is locked now, even with the right password.
        self.assertEqual(self.login_from("203.0.113.7", PASSWORD), 429)

    def test_right_password_still_signs_in(self):
        self.assertEqual(self.login_from("192.0.2.44", PASSWORD), 200)


class TestPasswordChange(ServerTestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        status, raw, _ = cls.server.setup_owner(Client(cls.server.port), "owner", PASSWORD)
        assert status == 200, raw

    def signed_in(self, password=PASSWORD):
        c = self.new_client()
        status, raw, _ = c.post("/api/login", {"username": "owner", "password": password})
        self.assertEqual(status, 200, raw)
        return c

    def change(self, client, current, new):
        return client.post("/api/account/password",
                           {"current_password": current, "new_password": new})

    def test_01_signs_out_other_sessions_and_keeps_this_one(self):
        here, laptop, phone = self.signed_in(), self.signed_in(), self.signed_in()
        status, _, _ = laptop.post("/api/tokens", {"label": "ci"})
        self.assertEqual(status, 200)
        token = laptop.as_json(laptop.get("/api/tokens")[1])[0]

        status, raw, _ = self.change(here, PASSWORD, "a whole new password")
        self.assertEqual(status, 200, raw)
        # laptop, phone, and the session setUpClass's setup left behind.
        self.assertEqual(here.as_json(raw)["signed_out_sessions"], 3)

        self.assertEqual(here.get("/api/me")[0], 200)
        self.assertEqual(laptop.get("/api/me")[0], 401)
        self.assertEqual(phone.get("/api/me")[0], 401)
        # API tokens are separate credentials: kept.
        self.assertEqual([t["id"] for t in here.as_json(here.get("/api/tokens")[1])], [token["id"]])
        # The old password is gone, the new one works.
        self.assertEqual(self.new_client().post(
            "/api/login", {"username": "owner", "password": PASSWORD})[0], 401)
        self.signed_in("a whole new password")

    def test_02_wrong_current_password_changes_nothing(self):
        here, other = self.signed_in("a whole new password"), self.signed_in("a whole new password")
        status, _, _ = self.change(here, "not my password", "yet another password")
        self.assertEqual(status, 401)
        self.assertEqual(other.get("/api/me")[0], 200)

    def test_03_new_password_must_be_8_characters(self):
        here = self.signed_in("a whole new password")
        self.assertEqual(self.change(here, "a whole new password", "short")[0], 400)


class TestPasswordChangeReleasesControl(ControlTestBase):
    def test_a_signed_out_session_loses_control_at_once(self):
        chip = self.chips["A"]
        laptop = self.new_operator("pw_op")
        self.acquire(laptop, self.A())
        self.key(laptop, self.A(), "KeyA")
        self.wait_held(chip, {KEY_A}, "A")

        here = self.new_client()
        status, raw, _ = here.post("/api/login", {"username": "pw_op", "password": "operatorpassword9"})
        self.assertEqual(status, 200, raw)
        status, raw, _ = here.post("/api/account/password", {
            "current_password": "operatorpassword9", "new_password": "operatorpassword10"})
        self.assertEqual(status, 200, raw)

        # Released by the password change itself, not the next periodic check.
        self.assertEqual(self.status_of(here, self.A())["controlled"], False)
        self.wait_held(chip, set(), "A after the password change")


class TestExpiredSessionPurge(ServerTestCase):
    def test_a_new_session_clears_out_expired_ones(self):
        owner = self.new_client()
        status, raw, _ = self.server.setup_owner(owner, "owner", PASSWORD)
        self.assertEqual(status, 200, raw)
        with sqlite3.connect(self.server.db_path) as db:
            uid = db.execute("SELECT id FROM users").fetchone()[0]
            db.executemany(
                "INSERT INTO sessions (token_hash, user_id, expires_at) VALUES (?, ?, ?)",
                [(f"stale{i}", uid, "2000-01-01 00:00:00") for i in range(3)])

        self.assertEqual(self.new_client().post(
            "/api/login", {"username": "owner", "password": PASSWORD})[0], 200)
        with sqlite3.connect(self.server.db_path) as db:
            stale = db.execute("SELECT COUNT(*) FROM sessions WHERE token_hash LIKE 'stale%'").fetchone()[0]
            live = db.execute("SELECT COUNT(*) FROM sessions").fetchone()[0]
        self.assertEqual(stale, 0)
        self.assertEqual(live, 2)   # setup's session and the new sign-in


class TestExpiredSessionPurgeAtStartup(ServerTestCase):
    def test_startup_clears_out_expired_sessions(self):
        status, raw, _ = self.server.setup_owner(self.new_client(), "owner", PASSWORD)
        self.assertEqual(status, 200, raw)
        with sqlite3.connect(self.server.db_path) as db:
            db.execute("INSERT INTO sessions (token_hash, user_id, expires_at) "
                       "SELECT 'stale', id, '2000-01-01 00:00:00' FROM users")
        # Restart on the same database (stop() would delete it).
        self.server.proc.terminate()
        self.server.proc.wait(timeout=5)
        self.server._drainer.join(timeout=5)
        self.server.start()
        self.server.wait_for_output(r"Removed 1 expired sessions")
        with sqlite3.connect(self.server.db_path) as db:
            self.assertEqual(db.execute(
                "SELECT COUNT(*) FROM sessions WHERE token_hash = 'stale'").fetchone()[0], 0)
