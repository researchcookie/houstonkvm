"""The QEMU QMP input backend (src/hid/qemu_inject.cpp), which until now had
no integration coverage. A fake QMP server on a unix socket stands in for
QEMU: it speaks the greeting + qmp_capabilities handshake and records the
input-send-event commands the server writes. Two things are checked: a key
reaches QMP as an input event, and a socket that connects but never speaks
QMP is given up on (the 2 s handshake receive timeout) instead of hanging the
target's build thread for good.
"""
import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import Client, HoustonKVMServer
from test_targets import OWNER, login


class FakeQmp:
    """A unix-socket QMP server. silent=True accepts the connection but sends
    nothing, to exercise the handshake timeout."""

    def __init__(self, silent=False):
        self.silent = silent
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "qmp.sock")
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(self.path)
        self.sock.listen(1)
        self.commands = []            # every decoded command object received
        self._lock = threading.Lock()
        self._stop = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        self.sock.settimeout(0.5)
        conns = []
        while not self._stop:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            conns.append(conn)
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()
        for c in conns:
            try: c.close()
            except OSError: pass

    def _serve(self, conn):
        if self.silent:
            # Connect, say nothing: the server's handshake read must time out.
            while not self._stop:
                time.sleep(0.1)
            return
        conn.sendall(b'{"QMP":{"version":{},"capabilities":[]}}\r\n')
        buf = b""
        conn.settimeout(0.5)
        while not self._stop:
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    cmd = json.loads(line)
                except ValueError:
                    continue
                with self._lock:
                    self.commands.append(cmd)
                conn.sendall(b'{"return":{}}\r\n')

    def input_events(self):
        """Every event object across all input-send-event commands seen."""
        with self._lock:
            cmds = list(self.commands)
        events = []
        for c in cmds:
            if c.get("execute") == "input-send-event":
                events += c.get("arguments", {}).get("events", [])
        return events

    def close(self):
        self._stop = True
        try: self.sock.close()
        except OSError: pass


class QmpTestBase(unittest.TestCase):
    SILENT = False

    @classmethod
    def setUpClass(cls):
        cls.qmp = FakeQmp(silent=cls.SILENT)
        cls.server = HoustonKVMServer()
        cls.server.start()
        cls.owner = Client(cls.server.port)
        assert cls.server.setup_owner(cls.owner, *OWNER)[0] == 200
        assert cls.owner.post("/api/users",
            {"username": "op", "password": "operatorpassword1", "role": "operator"})[0] == 200
        cls.op = Client(cls.server.port)
        login(cls.op, ("op", "operatorpassword1"))
        status, raw, _ = cls.owner.post("/api/targets",
            {"name": "VM", "qmp_socket": cls.qmp.path,
             "v4l2_device": "/dev/video241", "enabled": True})
        assert status == 201, raw
        cls.tid = cls.owner.as_json(raw)["id"]

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()
        cls.qmp.close()

    def target(self):
        return self.owner.as_json(self.owner.get(f"/api/targets/{self.tid}")[1])

    def wait_until(self, predicate, timeout, what):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate():
                return
            time.sleep(0.2)
        self.fail(f"timed out waiting for {what}")


class TestQmpInput(QmpTestBase):
    def test_a_key_reaches_qmp_as_an_input_event(self):
        self.wait_until(lambda: self.target()["status"]["input_ready"], 20, "QMP input ready")
        self.assertEqual(self.op.post(f"/api/targets/{self.tid}/control/acquire")[0], 200)
        self.assertEqual(self.op.post(f"/api/targets/{self.tid}/input",
                         {"type": "key", "code": "KeyA", "pressed": True})[0], 200)
        self.wait_until(
            lambda: any(e.get("type") == "key" and
                        e.get("data", {}).get("key", {}).get("data") == "a"
                        for e in self.qmp.input_events()),
            10, "the key to reach QMP")
        self.op.post(f"/api/targets/{self.tid}/control/release")


class TestQmpSilentHandshake(QmpTestBase):
    """A QMP socket that connects but never speaks. The handshake read times
    out (SO_RCVTIMEO) and the build thread moves on; without that timeout it
    would block forever, and the proof is that a later reconfigure to a
    working QMP socket is actually processed."""
    SILENT = True

    def test_build_thread_recovers_and_a_reconfigure_still_takes_effect(self):
        self.wait_until(lambda: self.target()["state"] == "running", 20, "target to build")
        self.assertFalse(self.target()["status"]["input_ready"])

        # Point the target at a QMP socket that does answer. If the build
        # thread were stuck in the silent socket's handshake, this rebuild
        # would sit unprocessed in the StreamManager queue and input would
        # never come up.
        working = FakeQmp()
        self.addCleanup(working.close)
        status, raw, _ = self.owner.put(f"/api/targets/{self.tid}", {"qmp_socket": working.path})
        self.assertEqual(status, 200, raw)
        self.wait_until(lambda: self.target()["status"]["input_ready"], 20,
                        "input to come up after reconfigure")


if __name__ == "__main__":
    unittest.main()
