"""Shared helpers for HoustonKVM HTTP integration tests: spins up a real
HoustonKVM binary against a throwaway sqlite db and gives tests a small
cookie/bearer-aware HTTP client to drive it with. Stdlib only (no pip
dependencies), so it runs anywhere python3 -Bx does — same bar as
cmake/embed_files.py already sets for this project's tooling.
"""
import base64
import hashlib
import http.client
import ipaddress
import json
import os
import re
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BINARY = REPO_ROOT / "build" / "HoustonKVM"

_WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# HOUSTONKVM_TEST_TLS=1 runs the whole suite over HTTPS: every server starts
# with --tls=on and a certificate from a throwaway CA, and every client below
# speaks HTTPS to it, trusting that CA. server.port is then the HTTPS port.
TEST_TLS = os.environ.get("HOUSTONKVM_TEST_TLS") == "1"
_tls_by_port = {}   # each HTTPS test server's port -> a context trusting its CA


class TestCA:
    """A throwaway certificate authority, made with the openssl command
    (skips the test where there isn't one)."""

    def __init__(self, directory):
        if not shutil.which("openssl"):
            raise unittest.SkipTest("no openssl command to make test certificates with")
        self.dir = Path(directory)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.crt, self.key = self.dir / "ca.crt", self.dir / "ca.key"
        self._openssl("req", "-x509", *self._newkey("ec"), "-nodes", "-keyout", self.key, "-out", self.crt,
                      "-days", "30", "-subj", "/CN=HoustonKVM Test CA",
                      "-addext", "basicConstraints=critical,CA:TRUE",
                      "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        self.pem = self.crt.read_text()
        self.keys = []   # every key issued, PEM

    @staticmethod
    def _openssl(*args):
        subprocess.run(["openssl", *map(str, args)], check=True, capture_output=True)

    @staticmethod
    def _newkey(key):
        """key: "ec" (P-256), "ec384", "ec521" or "rsa:BITS"."""
        if key.startswith("rsa:"):
            return ["-newkey", key]
        curve = {"ec": "P-256", "ec384": "P-384", "ec521": "P-521"}[key]
        return ["-newkey", "ec", "-pkeyopt", f"ec_paramgen_curve:{curve}"]

    def issue(self, name, names=("DNS:localhost", "IP:127.0.0.1"), key="ec", self_signed=False,
              not_before=None, not_after=None):
        """A certificate and key, as (cert PEM, key PEM, cert path, key path).
        Signed by this CA unless self_signed. not_before/not_after are
        openssl's YYYYMMDDHHMMSSZ."""
        crt, keyfile = self.dir / f"{name}.crt", self.dir / f"{name}.key"
        dates = (["-not_before", not_before] if not_before else []) + \
                (["-not_after", not_after] if not_after else [])
        san = "subjectAltName=" + ",".join(names) if names else None
        if self_signed:
            self._openssl("req", "-x509", *self._newkey(key), "-nodes", "-keyout", keyfile, "-out", crt,
                          "-subj", "/CN=localhost", *(["-addext", san] if san else []), *dates)
        else:
            csr, ext = self.dir / f"{name}.csr", self.dir / f"{name}.ext"
            self._openssl("req", "-new", *self._newkey(key), "-nodes", "-keyout", keyfile, "-out", csr,
                          "-subj", "/CN=localhost")
            ext.write_text((san or "") + "\n")
            self._openssl("x509", "-req", "-in", csr, "-CA", self.crt, "-CAkey", self.key,
                          "-CAcreateserial", "-out", crt, "-extfile", ext, *dates)
        self.keys.append(keyfile.read_text())
        return crt.read_text(), self.keys[-1], str(crt), str(keyfile)

    def context(self):
        """Trusts this CA, and nothing else."""
        ctx = ssl.create_default_context(cafile=str(self.crt))
        return ctx


def tls_for(port):
    """The context for talking to `port`, if a test server serves HTTPS there."""
    return _tls_by_port.get(port)


def ice_gives_up_by_itself(binary) -> bool:
    """Whether the binary's ICE library ends gathering on its own when a
    STUN server never answers. libnice (EPEL's libdatachannel) does, about
    2 s in; libjuice (the bundled one) waits until the server goes ahead
    without it, and logs that it did."""
    try:
        out = subprocess.run(["ldd", str(binary)], capture_output=True, text=True).stdout
    except OSError:
        return False
    return "libnice" in out


def has_route_to(host, port) -> bool:
    """Whether this machine has a route to host that leaves it. A build
    sandbox with only loopback has none, or (mock) a default route through
    loopback itself; either way packets to host never go out to be left
    unanswered, so it can't stand in for a server that never replies."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect((host, port))
        except OSError:
            return False
        local = ipaddress.ip_address(s.getsockname()[0])
        return not (local.is_loopback or local.is_unspecified)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class HoustonKVMServer:
    """Starts/stops a HoustonKVM instance against a fresh temp sqlite db."""

    def __init__(self, binary=None, extra_args=(), host="127.0.0.1", https=None):
        self.binary = str(binary or os.environ.get("HOUSTONKVM_BINARY") or DEFAULT_BINARY)
        self.extra_args = list(extra_args)
        self.host = host   # where the readiness probe knocks (differs from 127.0.0.1 under --bind)
        self.port = free_port()
        self.http_port = self.port
        self.tmpdir = tempfile.TemporaryDirectory()
        self.db_path = os.path.join(self.tmpdir.name, "test.db")
        # https: serve HTTPS only (--tls=on), with a certificate from a
        # throwaway CA (self.ca); self.port is then the HTTPS port, and
        # self.tls a context trusting the CA. None follows the suite
        # (HOUSTONKVM_TEST_TLS), unless the test sets --tls options itself.
        self.ca = self.tls = None
        if https is None:
            https = TEST_TLS and not any(a.startswith("--tls") for a in self.extra_args)
        if https:
            self.ca = TestCA(os.path.join(self.tmpdir.name, "ca"))
            # 127.0.0.2 too: test_cli binds there.
            _, _, crt, key = self.ca.issue("server", names=("DNS:localhost", "IP:127.0.0.1", "IP:127.0.0.2"))
            self.port = free_port()
            self.extra_args += ["--tls=on", f"--tls-cert={crt}", f"--tls-key={key}",
                                f"--https-port={self.port}"]
            self.tls = _tls_by_port[self.port] = self.ca.context()
        self.proc = None
        self.output = []   # the server's stdout+stderr lines, drained by a thread
        self._output_cond = threading.Condition()
        # Set by the CMake `valgrind` target (HOUSTONKVM_DEV_BUILD=ON only) so
        # this same harness doubles as the valgrind workload — see
        # CMakeLists.txt's dev-tooling section for why SIGTERM-triggered
        # shutdown makes exit-time leak checking meaningful here.
        self.valgrind = bool(os.environ.get("HOUSTONKVM_VALGRIND"))
        self.valgrind_log = os.path.join(self.tmpdir.name, "valgrind.log") if self.valgrind else None

    def start(self):
        if not os.path.isfile(self.binary):
            raise unittest.SkipTest(
                f"HoustonKVM binary not found at {self.binary} — build it first "
                "(cmake --build build) or set HOUSTONKVM_BINARY"
            )
        argv = [self.binary, f"--port={self.http_port}", f"--db={self.db_path}", *self.extra_args]
        if self.valgrind:
            argv = [
                "valgrind", "--leak-check=full", "--errors-for-leak-kinds=definite,indirect",
                "--error-exitcode=99", f"--log-file={self.valgrind_log}",
            ] + argv
        self.proc = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self._drainer = threading.Thread(target=self._drain, daemon=True)
        self._drainer.start()
        self._wait_ready(timeout=30 if self.valgrind else 10)

    def _drain(self):
        # Keeps the pipe from filling (which would block the server) and
        # lets tests read what it printed, e.g. the setup code.
        for line in self.proc.stdout:
            with self._output_cond:
                self.output.append(line)
                self._output_cond.notify_all()
        with self._output_cond:
            self._output_cond.notify_all()

    def wait_for_output(self, pattern, timeout=10):
        """First match of the regex `pattern` in the server's output."""
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        with self._output_cond:
            while True:
                for line in self.output:
                    m = rx.search(line)
                    if m:
                        return m
                left = deadline - time.time()
                if left <= 0 or self.proc.poll() is not None:
                    raise AssertionError(f"server never printed /{pattern}/:\n{''.join(self.output)}")
                self._output_cond.wait(left)

    @property
    def setup_code(self):
        return self.wait_for_output(r"setup code: (\S+)").group(1)

    def setup_owner(self, client, username, password):
        """POST /api/setup as a fresh install's first visitor, with the code."""
        return client.post("/api/setup", {"username": username, "password": password,
                                          "setup_code": self.setup_code})

    def _wait_ready(self, timeout=10):
        deadline = time.time() + timeout
        last_err = None
        while time.time() < deadline:
            if self.proc.poll() is not None:
                time.sleep(0.2)   # let the drain thread catch the last lines
                out = "".join(self.output)
                raise RuntimeError(
                    f"HoustonKVM exited early (code {self.proc.returncode}):\n{out}"
                )
            try:
                conn = (http.client.HTTPSConnection(self.host, self.port, timeout=1, context=self.tls)
                        if self.tls else http.client.HTTPConnection(self.host, self.port, timeout=1))
                conn.request("GET", "/api/status")
                resp = conn.getresponse()
                resp.read()
                conn.close()
                if resp.status in (200, 307):   # 307: HTTPS is on, and this is the redirect
                    return
            except OSError as e:
                last_err = e
            time.sleep(0.1)
        raise RuntimeError(f"HoustonKVM did not become ready on port {self.port}: {last_err}")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                # Valgrind's own instrumentation slows shutdown well beyond
                # the un-instrumented 5s budget, on top of needing time to
                # walk the heap for the leak report before it lets the
                # process exit.
                self.proc.wait(timeout=30 if self.valgrind else 5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.proc:
            self._drainer.join(timeout=5)
            self.proc.stdout.close()
        if self.tls and _tls_by_port.get(self.port) is self.tls:
            del _tls_by_port[self.port]   # the port may go to a plain server next
        if self.valgrind and self.valgrind_log and os.path.isfile(self.valgrind_log):
            log = open(self.valgrind_log).read()
            print(f"\n----- valgrind log ({self.valgrind_log}) -----\n{log}")
            if self.proc.returncode == 99:
                raise AssertionError("valgrind reported leaks/errors — see log above")
        self.tmpdir.cleanup()


class Client:
    """Minimal HTTP client with cookie-jar + bearer-token support, wrapping
    http.client so no third-party requests dependency is needed."""

    # PUT /api/admin/settings queues a rebuild (new SfuPublisher + input
    # backend + capture device) on StreamManager's worker thread and
    # returns immediately — the rebuild itself is documented as blocking
    # "up to ~10s" on ICE gathering, and was observed taking well beyond
    # that under this suite's rapid-fire request pattern. 5s was too tight
    # and produced flaky timeouts on requests unrelated to that rebuild;
    # this is a generous ceiling, not a claim that requests normally take
    # this long.
    DEFAULT_TIMEOUT = 20

    def __init__(self, port, tls=None):
        """tls: an SSLContext for HTTPS, False for plain HTTP, or None for
        whichever the test server on `port` speaks."""
        self.port = port
        self.tls = tls_for(port) if tls is None else (tls or None)
        self.cookie = None
        self.cookie_name = "__Host-session" if self.tls else "session"
        self.bearer = None

    def connection(self, timeout=None):
        timeout = timeout or self.DEFAULT_TIMEOUT
        if self.tls:
            return http.client.HTTPSConnection("127.0.0.1", self.port, timeout=timeout, context=self.tls)
        return http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)

    def connect(self, timeout=None):
        """A raw socket to the server, wrapped in TLS when it's HTTPS."""
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=timeout)
        return self.wrap(sock)

    def wrap(self, sock):
        return self.tls.wrap_socket(sock, server_hostname="localhost") if self.tls else sock

    def cookie_header(self):
        """The session cookie, as a raw request's header line."""
        return f"Cookie: {self.cookie_name}={self.cookie}\r\n" if self.cookie else ""

    def request(self, method, path, body=None, headers=None, use_cookie=True):
        conn = self.connection()
        hdrs = dict(headers or {})
        payload = None
        if body is not None:
            payload = json.dumps(body) if isinstance(body, (dict, list)) else body
            hdrs.setdefault("Content-Type", "application/json")
        if use_cookie and self.cookie:
            hdrs["Cookie"] = f"{self.cookie_name}={self.cookie}"
        if self.bearer:
            hdrs["Authorization"] = f"Bearer {self.bearer}"
        conn.request(method, path, body=payload, headers=hdrs)
        resp = conn.getresponse()
        raw = resp.read()
        set_cookie = resp.getheader("Set-Cookie")
        if set_cookie:
            # "session=<token>; Path=/; ..." on login, or "session=; ...
            # Max-Age=0" on logout — either way, track the current value.
            # (__Host-session over HTTPS.)
            name, value = set_cookie.split(";", 1)[0].split("=", 1)
            self.cookie_name = name
            self.cookie = value or None
        conn.close()
        return resp.status, raw, resp

    def get(self, path, **kw):
        return self.request("GET", path, **kw)

    def post(self, path, body=None, **kw):
        return self.request("POST", path, body=body, **kw)

    def put(self, path, body=None, **kw):
        return self.request("PUT", path, body=body, **kw)

    def delete(self, path, **kw):
        return self.request("DELETE", path, **kw)

    @staticmethod
    def as_json(raw: bytes):
        return json.loads(raw.decode()) if raw else None

    def ws(self, path, headers=None):
        """Opens a WsClient against `path`, reusing this client's current
        session cookie the same way request() does."""
        return WsClient(self.port, path, cookie=self.cookie, headers=headers, client=self)


class WsClient:
    """Minimal hand-rolled RFC6455 client for /api/input/ws — no
    third-party websocket dependency, matching Client's stdlib-only bar
    (see this module's docstring). Only handles what the input route
    actually needs: a single small JSON text frame per send/recv, well
    under the route's 256-byte payload cap."""

    def __init__(self, port, path, cookie=None, headers=None, client=None):
        client = client or Client(port)
        self.sock = client.connect(timeout=Client.DEFAULT_TIMEOUT)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{port}\r\n"
            f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
        )
        if cookie:
            req += f"Cookie: {client.cookie_name}={cookie}\r\n"
        for name, value in (headers or {}).items():
            req += f"{name}: {value}\r\n"
        req += "\r\n"
        self.sock.sendall(req.encode())

        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            data += chunk
        self.status_line = data.split(b"\r\n", 1)[0].decode()
        self.status = int(self.status_line.split(" ", 2)[1])
        self.upgraded = self.status == 101
        if self.upgraded:
            expected_accept = base64.b64encode(hashlib.sha1((key + _WS_GUID).encode()).digest()).decode()
            if expected_accept not in data.decode(errors="replace"):
                raise AssertionError(f"bad Sec-WebSocket-Accept in handshake response: {data!r}")

    def send_json(self, obj):
        self.send_text(json.dumps(obj))

    def send_text_raw(self, text):
        """Sends `text` verbatim as a WS text frame, without JSON-encoding
        it first — for exercising the server's malformed-payload handling."""
        self.send_text(text)

    def send_text(self, text):
        payload = text.encode()
        header = bytearray([0x81])  # FIN + text opcode
        mask = os.urandom(4)
        if len(payload) < 126:
            header.append(0x80 | len(payload))
        else:
            header.append(0x80 | 126)
            header += struct.pack(">H", len(payload))
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def recv_json(self):
        data = self.sock.recv(4096)
        length = data[1] & 0x7F
        idx = 2
        if length == 126:
            length = struct.unpack(">H", data[idx:idx + 2])[0]
            idx += 2
        return json.loads(data[idx:idx + length].decode())

    def close(self):
        self.sock.close()


class ServerTestCase(unittest.TestCase):
    """Base class that starts one HoustonKVM instance per test class (not per
    test method — that would be slow) and exposes self.client. Subclasses
    relying on sequential state (setup -> login -> ...) should name test
    methods test_01_..., test_02_... since unittest runs them in sorted
    order within a class."""

    server: HoustonKVMServer
    https = None   # True: this class's server is HTTPS-only (see HoustonKVMServer)

    @classmethod
    def setUpClass(cls):
        cls.server = HoustonKVMServer(https=cls.https)
        cls.server.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.stop()

    def setUp(self):
        self.client = Client(self.server.port, tls=self.server.tls)

    def new_client(self):
        return Client(self.server.port, tls=self.server.tls)
