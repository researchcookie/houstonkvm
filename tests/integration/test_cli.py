"""The command line: --version, --help, rejected options, and --bind."""
import http.client
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import DEFAULT_BINARY, HoustonKVMServer, REPO_ROOT, tls_for


def binary():
    b = os.environ.get("HOUSTONKVM_BINARY") or str(DEFAULT_BINARY)
    if not os.path.isfile(b):
        raise unittest.SkipTest(f"HoustonKVM binary not found at {b}")
    return b


def run(*args):
    return subprocess.run([binary(), *args], capture_output=True, text=True, timeout=15)


class TestCommandLine(unittest.TestCase):
    def test_version_matches_the_version_the_project_declares(self):
        r = run("--version")
        self.assertEqual(r.returncode, 0)
        declared = re.search(r"project\(HoustonKVM VERSION ([0-9.]+)", (REPO_ROOT / "CMakeLists.txt").read_text()).group(1)
        # A build may name itself after the version (HOUSTONKVM_BUILD_ID).
        self.assertRegex(r.stdout.strip(), rf"^HoustonKVM {re.escape(declared)}( \(\S+\))?$")
        spec = re.search(r"(?m)^Version:\s+(\S+)", (REPO_ROOT / "houstonkvm.spec").read_text()).group(1)
        self.assertEqual(spec, declared, "houstonkvm.spec Version: and CMakeLists.txt disagree")

    def test_help_lists_the_options_and_exits_cleanly(self):
        r = run("--help")
        self.assertEqual(r.returncode, 0)
        for opt in ("--port", "--bind", "--db", "--secure-cookies", "--version"):
            self.assertIn(opt, r.stdout)

    def test_a_mistyped_option_is_rejected_instead_of_silently_ignored(self):
        r = run("--prot=9000")
        self.assertEqual(r.returncode, 2)
        self.assertIn("Unknown option: --prot=9000", r.stderr)

    def test_a_bad_port_is_rejected(self):
        for bad in ("--port=abc", "--port=0", "--port=70000", "--port=80x"):
            with self.subTest(arg=bad):
                r = run(bad)
                self.assertEqual(r.returncode, 2)
                self.assertIn("Invalid port", r.stderr)


class TestBind(unittest.TestCase):
    def status(self, host, port):
        tls = tls_for(port)
        conn = (http.client.HTTPSConnection(host, port, timeout=2, context=tls) if tls
                else http.client.HTTPConnection(host, port, timeout=2))
        try:
            conn.request("GET", "/api/status")
            return conn.getresponse().status
        finally:
            conn.close()

    def test_bind_limits_which_address_answers(self):
        # All of 127.0.0.0/8 is loopback on Linux, so a server bound to
        # 127.0.0.2 must answer there and refuse on 127.0.0.1.
        server = HoustonKVMServer(extra_args=["--bind=127.0.0.2"], host="127.0.0.2")
        server.start()
        self.addCleanup(server.stop)
        self.assertEqual(self.status("127.0.0.2", server.port), 200)
        with self.assertRaises(OSError):
            self.status("127.0.0.1", server.port)

    def test_failing_to_listen_exits_instead_of_idling(self):
        # Under systemd a process that is up but not listening looks healthy.
        import socket
        import tempfile
        with tempfile.TemporaryDirectory() as tmp, socket.socket() as taken:
            taken.bind(("127.0.0.1", 0))
            taken.listen(1)
            port = taken.getsockname()[1]
            for extra in (["--bind=127.0.0.1"], ["--bind=203.0.113.99"]):   # port in use; not our address
                with self.subTest(args=extra):
                    r = run(f"--port={port}", f"--db={tmp}/x.db", *extra)
                    self.assertEqual(r.returncode, 1)
                    self.assertIn("Failed to bind", r.stderr)


if __name__ == "__main__":
    unittest.main()
