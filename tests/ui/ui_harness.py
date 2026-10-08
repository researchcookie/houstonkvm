"""Browser-test support: a real HoustonKVM server plus headless Chromium
(Playwright), with pty-backed fake CH9329s so targets have working keyboard/
mouse input and the tests can see the HID frames a real machine would receive.

These tests need Playwright and a Chromium build, which the stdlib-only
integration suite deliberately doesn't:

    pip install playwright && playwright install chromium

Set HOUSTONKVM_UI_BROWSER=firefox (or webkit, Safari's engine) to run the same
tests in another engine; the default is chromium.

Everything here skips (rather than fails) when either is missing, so the
plain integration suite (tests/integration) is unaffected. See
scripts/run-ui-tests.sh.

No capture hardware is assumed: targets have no video, so the pages show
their "No capture device" states — which is exactly what makes the status handling
testable on any machine. Snapshot/stream requests therefore 503 by design;
unexpected_errors() filters those and nothing else.
"""
import os
import sys
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "integration"))
from server_harness import Client, HoustonKVMServer, TestCA, free_port  # noqa: E402
from test_control import FakeChip  # noqa: E402

try:
    from playwright.sync_api import expect, sync_playwright
    # The 5s default is tight for a slower engine (Firefox runs this suite
    # about 1.7x slower than Chromium) or a loaded CI box; a timeout there
    # is noise, not a finding.
    expect.set_options(timeout=10_000)
except ImportError:  # pragma: no cover - environment dependent
    sync_playwright = None
    expect = None

OWNER = ("owner", "ownerpassword1")
PASSWORD = "password12345"
KEY_A, KEY_B, KEY_C = 0x04, 0x05, 0x06
KEY_7 = 0x24

# Requests that legitimately fail on a box with no capture card.
_EXPECTED_FAILURES = ("/snapshot", "/stream")


def wait_until(predicate, timeout=10, what="condition"):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {what}")


class BrowserPage:
    """One browser context: a page plus everything it complained about."""

    def __init__(self, browser, base, width=1280, height=850, bypass_csp=False, ignore_https_errors=False):
        self.base = base
        # ignore_https_errors: like clicking past the browser's warning about
        # a certificate it doesn't trust (HoustonKVM's own CA, untrusted here).
        self.context = browser.new_context(viewport={"width": width, "height": height},
                                           bypass_csp=bypass_csp, ignore_https_errors=ignore_https_errors)
        self.page = self.context.new_page()
        self.errors = []
        self.page.on("pageerror", lambda e: self.errors.append(f"PAGEERROR: {e}"))
        # Anything the server's Content-Security-Policy blocked: the UI
        # silently losing a script or style that way is a bug.
        self.page.on("console", self._on_console)
        self.page.on("response", self._on_response)
        # Every confirm() (delete / make default) is answered "OK".
        self.page.on("dialog", lambda d: d.accept())

    def _on_console(self, m):
        if m.type == "error" and "Content Security Policy" in m.text:
            self.errors.append(f"CSP: {m.text}")

    def _on_response(self, r):
        if r.status >= 400 and not any(p in r.url for p in _EXPECTED_FAILURES):
            self.errors.append(f"HTTP {r.status} {r.url}")

    def unexpected_errors(self):
        return list(self.errors)

    def sign_in(self, username, password=PASSWORD, here=False):
        """here: on the page as it is (its own address), not the server's base."""
        p = self.page
        if not here:
            p.goto(self.base + "/")
        p.wait_for_selector("#l-user", state="visible")
        p.fill("#l-user", username)
        p.fill("#l-pass", password)
        p.click("#form-login button[type=submit]")
        p.wait_for_selector("#app:not([hidden])")

    def goto(self, hash_path):
        self.page.goto(self.base + "/" + hash_path)


class UITestCase(unittest.TestCase):
    """One server + one browser per test class. Subclasses build their
    fixtures in seed()."""

    SETUP_OWNER = True   # False leaves the server on its first-run setup form
    SERVER_ARGS = ()     # extra command-line options for this class's server

    @classmethod
    def setUpClass(cls):
        if sync_playwright is None:
            raise unittest.SkipTest("Playwright not installed (pip install playwright)")
        cls._pw = sync_playwright().start()
        engine = os.environ.get("HOUSTONKVM_UI_BROWSER", "chromium")  # chromium | firefox | webkit
        try:
            cls.browser = getattr(cls._pw, engine).launch()
        except Exception as e:
            cls._pw.stop()
            raise unittest.SkipTest(f"{engine} unavailable ({str(e).splitlines()[0]})")

        cls.server = HoustonKVMServer(extra_args=cls.SERVER_ARGS)
        cls.server.start()
        cls.base = f"http://127.0.0.1:{cls.server.port}"
        cls.owner = Client(cls.server.port)
        if cls.SETUP_OWNER:
            status, raw, _ = cls.server.setup_owner(cls.owner, *OWNER)
            assert status == 200, raw
        cls.chips = []
        cls.seed()

    @classmethod
    def seed(cls):
        """Override: create users/targets."""

    @classmethod
    def tearDownClass(cls):
        for c in getattr(cls, "chips", []):
            c.close()
        if getattr(cls, "browser", None):
            cls.browser.close()
        if getattr(cls, "_pw", None):
            cls._pw.stop()
        if getattr(cls, "server", None):
            cls.server.stop()

    # -- fixtures --------------------------------------------------------
    @classmethod
    def add_user(cls, name, role):
        status, raw, _ = cls.owner.post("/api/users", {"username": name, "password": PASSWORD, "role": role})
        assert status == 200, raw

    @classmethod
    def add_target(cls, name, chip=True, **fields):
        """Creates a target; with chip=True its input is a fake CH9329 whose
        received frames are on the returned dict's 'chip'."""
        # Distinct, nonexistent capture paths: the server rejects two targets on one device.
        body = {"name": name, "enabled": True, "v4l2_device": f"/dev/video{100 + cls._next_device()}"}
        chip_obj = None
        if chip:
            chip_obj = FakeChip()
            cls.chips.append(chip_obj)
            body["serial_device"] = chip_obj.path
        body.update(fields)
        status, raw, _ = cls.owner.post("/api/targets", body)
        assert status == 201, raw
        target = cls.owner.as_json(raw)
        target["chip"] = chip_obj
        return target

    @classmethod
    def new_chip(cls):
        """A fake CH9329 not yet attached to any target (for tests that wire
        one up through the UI itself)."""
        chip = FakeChip()
        cls.chips.append(chip)
        return chip

    _device_counter = 0

    @classmethod
    def _next_device(cls):
        UITestCase._device_counter += 1
        return UITestCase._device_counter

    @classmethod
    def wait_input_ready(cls, target_id, timeout=40):
        def ready():
            t = cls.owner.as_json(cls.owner.get(f"/api/targets/{target_id}")[1])
            return t["status"]["input_ready"]
        wait_until(ready, timeout, f"target {target_id} to be input-ready")

    # -- browser ---------------------------------------------------------
    def open(self, user=None, password=PASSWORD, width=1280, height=850, bypass_csp=False,
             ignore_https_errors=False):
        bp = BrowserPage(self.browser, self.base, width, height, bypass_csp, ignore_https_errors)
        self.addCleanup(bp.context.close)
        if user:
            bp.sign_in(user, password)
        return bp

    def assert_no_unexpected_errors(self, bp):
        self.assertEqual(bp.unexpected_errors(), [], "the page reported errors")
