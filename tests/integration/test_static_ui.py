"""The embedded ui/ bundle (build/generated/ui_bundle.h) is served correctly
by the plain, unauthenticated static-file routes wired up in server.cpp."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from server_harness import REPO_ROOT, ServerTestCase

UI_DIR = REPO_ROOT / "ui"

# Content types the embedder (cmake/embed_files.py) is expected to assign.
EXPECTED_TYPES = {
    ".html": "text/html", ".css": "text/css", ".js": "javascript",
    ".svg": "image/svg+xml", ".woff2": "font/woff2", ".ttf": "font/ttf",
}


class TestStaticUI(ServerTestCase):
    def test_root_serves_index_html(self):
        status, raw, resp = self.client.get("/")
        self.assertEqual(status, 200)
        self.assertIn("text/html", resp.getheader("Content-Type", ""))
        self.assertIn(b"HoustonKVM", raw)

    def test_index_html_also_served_at_its_own_path(self):
        status, raw, resp = self.client.get("/index.html")
        self.assertEqual(status, 200)
        self.assertIn("text/html", resp.getheader("Content-Type", ""))

    def test_stylesheets(self):
        for path in ["/css/tokens.css", "/css/base.css", "/css/layout.css",
                     "/css/components.css", "/css/pages.css"]:
            with self.subTest(path=path):
                status, raw, resp = self.client.get(path)
                self.assertEqual(status, 200)
                self.assertIn("text/css", resp.getheader("Content-Type", ""))
                self.assertGreater(len(raw), 0)

    def test_fonts_are_served_by_us_with_their_licences(self):
        # Never fetched from the internet: the CSP allows only our own
        # origin, and a KVM may have no way out. Each licence must travel
        # with its font, in the source and in the RPM.
        spec = (REPO_ROOT / "houstonkvm.spec").read_text()
        for name, file, kind, magic in (
                ("playfair-display", "playfair-display.ttf", "font/ttf", b"\x00\x01\x00\x00"),
                ("source-sans-3", "source-sans-3.woff2", "font/woff2", b"wOF2")):
            with self.subTest(font=name):
                status, raw, resp = self.client.get(f"/fonts/{file}")
                self.assertEqual(status, 200)
                self.assertIn(kind, resp.getheader("Content-Type", ""))
                self.assertTrue(raw.startswith(magic))
                self.assertIn("SIL Open Font License",
                              (UI_DIR / "fonts" / f"LICENSE-{name}.txt").read_text())
                self.assertRegex(spec, rf"(?m)^%license .*ui/fonts/LICENSE-{name}\.txt")

    def test_js_files_served_with_js_mimetype(self):
        for path in [
            "/js/util.js",
            "/js/auth.js",
            "/js/input.js",
            "/js/webrtc.js",
            "/js/settings.js",
            "/js/admin.js",
        ]:
            with self.subTest(path=path):
                status, raw, resp = self.client.get(path)
                self.assertEqual(status, 200)
                self.assertIn("javascript", resp.getheader("Content-Type", ""))
                self.assertGreater(len(raw), 0)

    def test_every_ui_file_is_served_byte_for_byte_with_the_right_type(self):
        # Generic on purpose: adding a file under ui/ (a font, an icon, a new
        # script) needs no test change, and the binary ones — which the
        # embedder stores as byte arrays rather than raw strings — can't
        # silently get corrupted or mistyped.
        files = sorted(p for p in UI_DIR.rglob("*") if p.is_file())
        self.assertGreaterEqual(len(files), 10)  # index.html, the css/ and js/ modules, the fonts
        for path in files:
            url = "/" + path.relative_to(UI_DIR).as_posix()
            with self.subTest(path=url):
                status, raw, resp = self.client.get(url)
                self.assertEqual(status, 200)
                want_type = EXPECTED_TYPES.get(path.suffix)
                if want_type:
                    self.assertIn(want_type, resp.getheader("Content-Type", ""))
                data = path.read_bytes()
                # Text files are embedded with a cosmetic leading newline
                # (except SVG, where XML forbids one before its declaration).
                if path.suffix in (".html", ".css", ".js", ".txt") and raw.startswith(b"\n"):
                    raw = raw[1:]
                self.assertEqual(raw, data)

    def test_nothing_vendored(self):
        # HoustonKVM's UI has no third-party frontend framework or icon set
        # vendored into it — the HTML, CSS and JavaScript under ui/ are ours,
        # and controls are native. (The two fonts in ui/fonts/ are the one
        # deliberate exception, each with its licence beside it.) Guard both
        # directions: no /vendor/ directory, and no source file points at
        # one. Nothing may load from another origin either: the CSP would
        # block it anyway.
        import re
        self.assertFalse((UI_DIR / "vendor").exists())
        sources = [UI_DIR / "index.html", *sorted((UI_DIR / "css").glob("*.css")),
                   *sorted((UI_DIR / "js").glob("*.js"))]
        referenced = set()
        for src in sources:
            referenced |= set(re.findall(r"[\"'(](/vendor/[^\"')\s?#]+)", src.read_text()))
            referenced |= set(re.findall(r"(?:(?:href|src)=\"|url\(\s*[\"']?)(https?://[^\"')\s]+)",
                                         src.read_text()))
        self.assertEqual(referenced, set())

    def test_no_file_points_at_a_source_map_we_do_not_ship(self):
        # A "sourceMappingURL" comment makes a browser's developer tools
        # request the map, which would 404 and log an error on every load.
        for path in sorted(UI_DIR.rglob("*")):
            if path.suffix in (".css", ".js", ".html"):
                with self.subTest(file=str(path.relative_to(UI_DIR))):
                    self.assertNotIn("sourceMappingURL", path.read_text(),
                                     "references a source map the server doesn't serve")

    def test_svgs_have_no_leading_whitespace(self):
        # An <?xml ?> declaration (if present) must be the very first bytes,
        # or browsers refuse to render the icon. Nothing under ui/ is an SVG
        # file today — the app's one icon is an inline data: URI in
        # index.html — but this stays as a guard for whenever one is added.
        for path in sorted(UI_DIR.rglob("*.svg")):
            with self.subTest(file=str(path.relative_to(UI_DIR))):
                status, raw, _ = self.client.get("/" + path.relative_to(UI_DIR).as_posix())
                self.assertEqual(status, 200)
                self.assertTrue(raw.startswith(b"<"), raw[:20])

    def test_the_ui_cannot_be_framed_and_is_served_with_basic_hardening_headers(self):
        for path in ("/", "/index.html", "/js/kvm.js", "/css/base.css"):
            with self.subTest(path=path):
                status, _, resp = self.client.get(path)
                self.assertEqual(status, 200)
                self.assertEqual(resp.getheader("X-Frame-Options"), "DENY")
                self.assertIn("frame-ancestors 'none'", resp.getheader("Content-Security-Policy", ""))
                self.assertEqual(resp.getheader("X-Content-Type-Options"), "nosniff")
                self.assertEqual(resp.getheader("Referrer-Policy"), "no-referrer")

    def test_a_file_the_browser_already_has_costs_a_304_with_no_body(self):
        # no-cache still makes the browser ask; the ETag lets the answer be
        # "unchanged" instead of the whole file again.
        for path in ("/", "/index.html", "/js/kvm.js"):
            with self.subTest(path=path):
                status, _, resp = self.client.get(path)
                self.assertEqual(status, 200)
                etag = resp.getheader("ETag", "")
                self.assertRegex(etag, r'^"[0-9a-f]{32}"$')
                self.assertEqual(resp.getheader("Cache-Control"), "no-cache")

                status, raw, resp = self.client.get(path, headers={"If-None-Match": etag})
                self.assertEqual(status, 304)
                self.assertEqual(raw, b"")
                self.assertEqual(resp.getheader("ETag"), etag)
                self.assertEqual(resp.getheader("Cache-Control"), "no-cache")
                self.assertIn("frame-ancestors 'none'", resp.getheader("Content-Security-Policy", ""))

    def test_if_none_match_is_compared_the_way_http_says(self):
        _, _, resp = self.client.get("/js/util.js")
        etag = resp.getheader("ETag")
        for header in (f"W/{etag}", f'"0000", {etag}', f'"0000",{etag} ', "*"):
            with self.subTest(header=header):
                status, _, _ = self.client.get("/js/util.js", headers={"If-None-Match": header})
                self.assertEqual(status, 304)

    def test_a_changed_file_is_sent_in_full(self):
        # A stale tag (the file changed in an upgrade) or another file's tag
        # gets the current file, never a 304.
        _, _, other = self.client.get("/js/util.js")
        for header in ('"00000000000000000000000000000000"', other.getheader("ETag"), ""):
            with self.subTest(header=header):
                status, raw, resp = self.client.get("/js/kvm.js", headers={"If-None-Match": header})
                self.assertEqual(status, 200)
                self.assertGreater(len(raw), 0)
                self.assertIn("javascript", resp.getheader("Content-Type", ""))

    def test_every_file_has_its_own_etag(self):
        tags = {}
        for path in sorted(p for p in UI_DIR.rglob("*") if p.is_file()):
            url = "/" + path.relative_to(UI_DIR).as_posix()
            _, _, resp = self.client.get(url)
            tags[url] = resp.getheader("ETag")
        self.assertEqual(len(set(tags.values())), len(tags), tags)

    def test_unknown_path_is_not_found(self):
        status, _, _ = self.client.get("/js/does-not-exist.js")
        self.assertEqual(status, 404)
