"""
test_ice_servers.py — the STUN/TURN servers for low-latency video belong to the
Owner: none by default, set under Admin -> Network (/api/admin/ice-servers),
or fixed by --ice-server in the server's configuration, which Admin then
shows read-only. The server and every viewer's browser use the same list.
"""
import json
import os
import signal
import sqlite3
import subprocess
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from server_harness import Client, HoustonKVMServer, has_route_to, ice_gives_up_by_itself  # noqa: E402

OWNER = ("owner", "ownerpassword1")
OPERATOR = ("operator1", "operatorpassword1")
VIEWER = ("viewer1", "viewerpassword1")
# TEST-NET-1 (RFC 5737): routed nowhere, so a STUN request to it is never
# answered and ICE gathering waits for as long as it's allowed to.
UNREACHABLE_STUN = "stun:192.0.2.1:3478"
TURN = {"url": "turn:turn.example.com:3478?transport=tcp", "username": "alice", "credential": "s3cret!"}


def login(client, who):
    status, raw, _ = client.post("/api/login", {"username": who[0], "password": who[1]})
    assert status == 200, raw


class IceTestCase(unittest.TestCase):
    extra_args = ()

    @classmethod
    def setUpClass(cls):
        cls.server = HoustonKVMServer(extra_args=cls.extra_args)
        cls.server.start()
        try:
            status, raw, _ = cls.server.setup_owner(Client(cls.server.port), *OWNER)
            assert status == 200, raw
            cls.owner = Client(cls.server.port)
            login(cls.owner, OWNER)
            status, raw, _ = cls.owner.get("/api/targets")
            cls.tid = next(t["id"] for t in Client.as_json(raw)["targets"] if t["default"])
        except BaseException:
            cls.server.stop()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def get(self, client=None):
        status, raw, _ = (client or self.owner).get("/api/admin/ice-servers")
        self.assertEqual(status, 200, raw)
        return Client.as_json(raw)

    def put(self, servers, client=None):
        status, raw, _ = (client or self.owner).put("/api/admin/ice-servers", {"ice_servers": servers})
        return status, raw.decode()

    def browser_ice_servers(self):
        """What a browser is told to use, from a real subscription."""
        status, raw, _ = self.owner.post(f"/api/targets/{self.tid}/webrtc/subscribe?candidates=on-answer")
        if status == 503 and b"publisher not available" in raw:
            # Loopback as the only interface (a mock/rpmbuild chroot): the
            # target's own WebRTC link has no address to connect over.
            self.skipTest("low-latency video can't run here (no network interface but loopback)")
        self.assertEqual(status, 200, raw)
        return Client.as_json(raw)["iceServers"]


class TestOwnerSetsThem(IceTestCase):

    def tearDown(self):
        self.assertEqual(self.put([])[0], 200)

    def test_none_by_default_for_the_server_and_the_browser(self):
        self.assertEqual(self.get(), {"ice_servers": [], "managed_by_config": False})
        self.assertEqual(self.browser_ice_servers(), [])

    def test_the_owner_sets_them_and_browsers_get_them(self):
        stun = {"url": "stun:stun.example.com:3478", "username": "", "credential": ""}
        status, raw = self.put([stun, TURN])
        self.assertEqual(status, 200, raw)
        self.assertEqual(self.get()["ice_servers"], [stun, TURN])
        self.assertEqual(self.browser_ice_servers(), [
            {"urls": "stun:stun.example.com:3478"},
            {"urls": TURN["url"], "username": "alice", "credential": "s3cret!"},
        ])

    def test_they_survive_a_restart(self):
        self.assertEqual(self.put([TURN])[0], 200)
        self.server.proc.send_signal(signal.SIGTERM)
        self.server.proc.wait(timeout=15)
        self.server._drainer.join(timeout=5)
        self.server.start()
        login(self.owner, OWNER)
        self.assertEqual(self.get()["ice_servers"], [TURN])

    def test_the_server_end_uses_them_too(self):
        # An unanswered STUN server keeps the server's own ICE gathering
        # waiting until it goes ahead without it, 2 s in: proof that the
        # server, not just the browser, took the new list. Removing it makes
        # gathering quick again.
        def full_subscribe():
            t0 = time.monotonic()
            status, raw, _ = self.owner.post(f"/api/targets/{self.tid}/webrtc/subscribe")
            return status, time.monotonic() - t0

        if not has_route_to("192.0.2.1", 3478):
            self.skipTest("no route to the test STUN address here (only loopback), so it can't go unanswered")
        status, took = full_subscribe()
        if status != 200:
            self.skipTest("ICE gathering can't work here (no network interface but loopback)")
        self.assertLess(took, 1.5)
        self.assertEqual(self.put([{"url": UNREACHABLE_STUN}])[0], 200)
        status, took = full_subscribe()
        self.assertEqual(status, 200)
        self.assertGreater(took, 1.5)
        self.assertLess(took, 4)
        if not ice_gives_up_by_itself(self.server.binary):
            self.server.wait_for_output(r"going ahead with \d+ ICE candidate\(s\)")
        self.assertEqual(self.put([])[0], 200)
        status, took = full_subscribe()
        self.assertEqual(status, 200)
        self.assertLess(took, 1.5)

    def test_what_is_refused(self):
        cases = {
            "no scheme": [{"url": "stun.example.com"}],
            "http scheme": [{"url": "http://stun.example.com"}],
            "no host": [{"url": "stun:"}],
            "bad port": [{"url": "stun:stun.example.com:70000"}],
            "empty port": [{"url": "stun:stun.example.com:"}],
            "space": [{"url": "stun:stun example.com"}],
            "credentials in url": [{"url": "turn:alice:pw@turn.example.com", "username": "a", "credential": "b"}],
            "turn without login": [{"url": "turn:turn.example.com"}],
            "stun with login": [{"url": "stun:stun.example.com", "username": "a", "credential": "b"}],
            "transport on stun": [{"url": "stun:stun.example.com?transport=tcp"}],
            "odd transport": [dict(TURN, url="turn:turn.example.com?transport=sctp")],
            "duplicate": [{"url": "stun:a.example.com"}, {"url": "stun:a.example.com"}],
            "too many": [{"url": f"stun:s{i}.example.com"} for i in range(9)],
            "not a list": {"url": "stun:stun.example.com"},
            "not text": [{"url": 5}],
        }
        for name, servers in cases.items():
            with self.subTest(name):
                status, raw = self.put(servers)
                self.assertEqual(status, 400, raw)
        self.assertEqual(self.get()["ice_servers"], [])
        status, raw, _ = self.owner.put("/api/admin/ice-servers", {"servers": []})
        self.assertEqual(status, 400, raw)

    def test_addresses_that_are_accepted(self):
        for url in ("stun:198.51.100.7", "stun:[2001:db8::1]:3478", "stun:stun-1.example.com:19302"):
            with self.subTest(url):
                status, raw = self.put([{"url": url}])
                self.assertEqual(status, 200, raw)

    def test_only_an_owner_in_a_browser_changes_them(self):
        status, raw, _ = self.owner.post("/api/users", {"username": OPERATOR[0], "password": OPERATOR[1],
                                                        "role": "operator"})
        self.assertIn(status, (200, 201), raw)
        status, raw, _ = self.owner.post("/api/users", {"username": VIEWER[0], "password": VIEWER[1],
                                                        "role": "viewer"})
        self.assertIn(status, (200, 201), raw)
        for who in (OPERATOR, VIEWER):
            c = Client(self.server.port)
            login(c, who)
            self.assertEqual(c.get("/api/admin/ice-servers")[0], 403)
            self.assertEqual(self.put([{"url": "stun:x.example.com"}], c)[0], 403)
        self.assertEqual(Client(self.server.port).get("/api/admin/ice-servers")[0], 401)

        # An Owner's API token may read them; changing them takes a browser
        # session, like the rest of Admin.
        status, raw, _ = self.owner.post("/api/tokens", {"label": "ice"})
        token = Client.as_json(raw)["token"]
        api = Client(self.server.port)
        api.bearer = token
        self.assertEqual(self.get(api)["managed_by_config"], False)
        self.assertEqual(self.put([{"url": "stun:x.example.com"}], api)[0], 401)
        self.assertEqual(self.get()["ice_servers"], [])

    def test_changes_are_audited_without_passwords(self):
        def events():
            status, raw, _ = self.owner.get("/api/audit/events?type=server.")
            self.assertEqual(status, 200, raw)
            return Client.as_json(raw)["events"]

        seen = {e["id"] for e in events()}   # other tests' changes
        self.assertEqual(self.put([TURN])[0], 200)
        self.assertEqual(self.put([TURN])[0], 200)   # no change, no event
        new = [e for e in events() if e["id"] not in seen]
        self.assertEqual([e["type"] for e in new], ["server.ice_servers"], new)
        latest = new[0]
        self.assertEqual(latest["user"]["username"], OWNER[0])
        self.assertEqual(latest["detail"]["changes"]["ice_servers"], {"from": [], "to": [TURN["url"]]})
        with sqlite3.connect(self.server.db_path) as db:
            stored = " ".join(r[0] for r in db.execute("SELECT detail FROM audit_events"))
        self.assertNotIn(TURN["credential"], stored)


class TestFixedByConfiguration(IceTestCase):
    extra_args = ("--ice-server=turn:alice:pa:ss@word@turn.example.com:3478",
                  "--ice-server=stun:stun.example.com")

    def test_admin_shows_them_read_only(self):
        self.assertEqual(self.get(), {
            "ice_servers": [
                {"url": "turn:turn.example.com:3478", "username": "alice", "credential": "pa:ss@word"},
                {"url": "stun:stun.example.com", "username": "", "credential": ""},
            ],
            "managed_by_config": True,
        })
        status, raw = self.put([])
        self.assertEqual(status, 409, raw)
        self.assertIn("--ice-server", raw)

    def test_the_browser_gets_the_login_separately(self):
        self.assertEqual(self.browser_ice_servers()[0],
                         {"urls": "turn:turn.example.com:3478", "username": "alice", "credential": "pa:ss@word"})

    def test_bad_options_stop_the_server_starting(self):
        for arg in ("turn:turn.example.com", "stun:alice:pw@stun.example.com", "stun:host:99999",
                    "stun.example.com"):
            with self.subTest(arg):
                out = subprocess.run([str(self.server.binary), f"--ice-server={arg}"],
                                     capture_output=True, text=True, timeout=10)
                self.assertEqual(out.returncode, 2, out.stderr)
                self.assertIn("Invalid ICE server", out.stderr)


class TestNoneByConfiguration(IceTestCase):
    extra_args = ("--ice-server=none",)

    def test_none_and_read_only(self):
        self.assertEqual(self.get(), {"ice_servers": [], "managed_by_config": True})
        self.assertEqual(self.put([{"url": "stun:stun.example.com"}])[0], 409)
        self.assertEqual(self.browser_ice_servers(), [])


if __name__ == "__main__":
    unittest.main()
