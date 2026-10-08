"""Automated accessibility audit (axe-core) of every screen.

The UI is hand-rolled (no vendored design system) — native controls plus a
small custom stylesheet — so nothing here inherits accessibility behavior
from anywhere else: a combobox, tab lists, target cards, and a pale palette
with hand-derived text colours are all exactly where contrast and ARIA
mistakes creep in. Zero violations is the bar.

axe-core is large third-party code, so it isn't vendored: it's downloaded once
(from the npm CDN) and cached in the system temp directory. If it can't be
fetched — offline CI, say — these tests skip rather than fail.
"""
import sys
import tempfile
import unittest
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ui_harness import OWNER, UITestCase, expect

AXE_VERSION = "4.10.2"
AXE_URL = f"https://cdn.jsdelivr.net/npm/axe-core@{AXE_VERSION}/axe.min.js"
AXE_CACHE = Path(tempfile.gettempdir()) / f"houstonkvm-axe-core-{AXE_VERSION}.js"

_AXE_SCRIPT = """async () => {
  const r = await axe.run(document, {resultTypes: ['violations']});
  return r.violations.map(v => ({
    id: v.id, impact: v.impact, help: v.help, count: v.nodes.length,
    where: v.nodes.slice(0, 3).map(n => n.target.join(' ')),
    why: ((v.nodes[0].any[0] || v.nodes[0].all[0] || v.nodes[0].none[0]) || {}).message
  }));
}"""


def load_axe():
    if not AXE_CACHE.exists():
        try:
            with urllib.request.urlopen(AXE_URL, timeout=20) as resp:
                AXE_CACHE.write_bytes(resp.read())
        except Exception as e:
            raise unittest.SkipTest(f"axe-core unavailable ({e})")
    return AXE_CACHE


# Colour maths shared by the in-page checks below: parse a computed colour,
# composite it over what's behind, and take WCAG contrast.
_HELPERS = """
  const parse = c => { const m = c.match(/rgba?\\(([^)]+)\\)/); if (!m) return null;
    const v = m[1].split(',').map(parseFloat); return {r: v[0], g: v[1], b: v[2], a: v.length > 3 ? v[3] : 1}; };
  const over = (t, b) => { const a = t.a + b.a * (1 - t.a);
    return a === 0 ? {r: 0, g: 0, b: 0, a: 0}
      : {r: (t.r * t.a + b.r * b.a * (1 - t.a)) / a, g: (t.g * t.a + b.g * b.a * (1 - t.a)) / a,
         b: (t.b * t.a + b.b * b.a * (1 - t.a)) / a, a}; };
  const lin = v => { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
  const lum = c => 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
  const backdrop = el => {
    const layers = [];
    for (let n = el; n; n = n.parentElement) {
      const c = parse(getComputedStyle(n).backgroundColor);
      if (c && c.a > 0) { layers.push(c); if (c.a >= 1) break; }
    }
    return layers.reduceRight((below, c) => over(c, below), {r: 255, g: 255, b: 255, a: 1});
  };
  const ratioOf = (x, y) => { const [hi, lo] = [lum(x), lum(y)].sort((p, q) => q - p); return (hi + 0.05) / (lo + 0.05); };
  const shown = el => el.checkVisibility({checkVisibilityCSS: true}) && el.getClientRects().length > 0;
  const name = el => el.tagName.toLowerCase() + (el.id ? '#' + el.id : el.className ? '.' + String(el.className).split(' ')[0] : '');
"""

# WCAG contrast of every visible text run against the colours actually behind
# it: 4.5:1, or 3:1 for large text and for disabled controls (which WCAG
# exempts but which still shouldn't vanish).
_CONTRAST_SCRIPT = "() => {" + _HELPERS + """
  const out = [], seen = new Set();
  const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
  for (let node; (node = walker.nextNode());) {
    if (!node.nodeValue.trim()) continue;
    const el = node.parentElement;
    if (!el || seen.has(el) || !el.checkVisibility({checkVisibilityCSS: true})) continue;
    if (el.closest('.sr-only, .skip-link, .target-thumb, .video-status, #kvm-viewport, script, style')) continue;
    if (el.getClientRects().length === 0) continue;
    seen.add(el);
    const cs = getComputedStyle(el);
    const bg = backdrop(el);
    const fg = over(parse(cs.color), bg);
    const [hi, lo] = [lum(fg), lum(bg)].sort((x, y) => y - x);
    const ratio = (hi + 0.05) / (lo + 0.05);
    const px = parseFloat(cs.fontSize), bold = parseInt(cs.fontWeight, 10) >= 700;
    const disabled = !!el.closest('[disabled], [aria-disabled=true]') ||
      !!(el.closest('label') && el.closest('label').control && el.closest('label').control.disabled);
    const need = (px >= 24 || (px >= 18.66 && bold) || disabled) ? 3 : 4.5;
    if (ratio < need) out.push(`${el.tagName.toLowerCase()}${el.className ? '.' + String(el.className).split(' ')[0] : ''} ` +
      `"${node.nodeValue.trim().slice(0, 40)}" ${cs.color} on ${cs.backgroundColor === 'rgba(0, 0, 0, 0)' ? 'inherited' : cs.backgroundColor} = ${ratio.toFixed(2)}:1 (needs ${need})`);
  }
  return out;
}"""

# The house style, measured on the rendered page (tests/integration/
# test_style.py checks the same rules in the CSS source):
#  - control edges: an input's border is how you find it, so WCAG 1.4.11
#    wants 3:1 against whatever is actually behind it;
#  - target size: anything you click or tap is at least 24x24 CSS px
#    (WCAG 2.5.8). Checkboxes and radios are exempt — their labels are part
#    of the target;
#  - type: every visible piece of text is in one of our three faces, and
#    none is smaller than 12px.
_STYLE_SCRIPT = "() => {" + _HELPERS + """
  const out = [];
  const controls = 'input:not([type=checkbox]):not([type=radio]):not([type=hidden]), select, textarea';
  for (const el of document.querySelectorAll(controls)) {
    if (!shown(el)) continue;
    const behind = backdrop(el.parentElement);
    const edge = over(parse(getComputedStyle(el).borderTopColor), behind);
    const r = ratioOf(edge, behind);
    if (r < 3) out.push(`edge of ${name(el)} is ${r.toFixed(2)}:1 (needs 3)`);
  }
  const targets = 'button, select, input:not([type=checkbox]):not([type=radio]):not([type=hidden]), [role=tab], a.btn, .chip';
  for (const el of document.querySelectorAll(targets)) {
    if (!shown(el) || el.closest('.sr-only, .skip-link')) continue;
    const b = el.getBoundingClientRect();
    if (b.width < 24 || b.height < 24) out.push(`${name(el)} is ${b.width.toFixed(0)}x${b.height.toFixed(0)}px (needs 24x24)`);
  }
  const faces = /^("Playfair Display"|"Source Sans 3"|ui-monospace)(,|$)/;
  const seen = new Set();
  const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
  for (let node; (node = walker.nextNode());) {
    const el = node.parentElement;
    if (!node.nodeValue.trim() || !el || seen.has(el) || !shown(el) || el.closest('.sr-only, script, style')) continue;
    seen.add(el);
    const cs = getComputedStyle(el);
    if (!faces.test(cs.fontFamily)) out.push(`${name(el)} "${node.nodeValue.trim().slice(0, 30)}" is in ${cs.fontFamily}`);
    if (parseFloat(cs.fontSize) < 12) out.push(`${name(el)} "${node.nodeValue.trim().slice(0, 30)}" is ${cs.fontSize}`);
  }
  return out;
}"""


class TestAccessibility(UITestCase):
    @classmethod
    def setUpClass(cls):
        cls.axe = load_axe()
        super().setUpClass()

    @classmethod
    def seed(cls):
        cls.add_user("op", "operator")
        cls.web = cls.add_target("Web Server 1", group="Rack 3", description="Front door nginx box",
                                 tags=["prod", "linux"])
        cls.add_target("DB Server", group="Rack 3", description="Primary PostgreSQL", tags=["prod", "windows"])
        cls.add_target("Spare PC", chip=False, enabled=False, group="Lab", tags=["dev"],
                       serial_device="/dev/ttyUSB77")

    def audit(self, page, label):
        page.add_script_tag(path=str(self.axe))
        violations = page.evaluate(_AXE_SCRIPT)
        self.assertEqual(violations, [], f"accessibility violations on: {label}")
        # axe reports text it can't measure (e.g. labels drawn beside a
        # styled checkbox) as "incomplete" rather than failing it, and that
        # is exactly how near-black radio labels on a dark page slipped by.
        # So measure every piece of visible text ourselves as well.
        faint = page.evaluate(_CONTRAST_SCRIPT)
        self.assertEqual(faint, [], f"text that is hard to read on: {label}")
        off_style = page.evaluate(_STYLE_SCRIPT)
        self.assertEqual(off_style, [], f"off the house style on: {label}")

    def test_signed_out_screens(self):
        bp = self.open(bypass_csp=True)   # axe is injected as an inline script
        p = bp.page
        p.goto(self.base + "/")
        expect(p.locator("#form-login")).to_be_visible()
        self.audit(p, "sign-in")

    def test_every_signed_in_screen(self):
        bp = self.open(bypass_csp=True)   # axe is injected as an inline script
        bp.sign_in(*OWNER)
        p = bp.page

        p.goto(f"{self.base}/#/targets")
        p.locator(".target-card").first.wait_for()
        self.audit(p, "directory")

        p.fill("#targets-search", "serv")
        p.keyboard.press("Enter")
        expect(p.locator(".target-card")).to_have_count(2)
        self.audit(p, "directory, searched")

        p.goto(f"{self.base}/#/targets/{self.web['id']}")
        expect(p.locator("#kvm-title")).to_have_text("Web Server 1")
        self.audit(p, "target view")
        p.click("#display-btn")
        expect(p.locator("#display-panel")).to_be_visible()
        expect(p.locator("#vid-apply")).to_be_enabled()
        self.audit(p, "target view with the Display panel open")

        for tab in ("account", "preferences", "bots"):
            p.goto(f"{self.base}/#/settings")
            p.click(f"#stab-{tab}")
            expect(p.locator(f"#spanel-{tab}")).to_be_visible()
            self.audit(p, f"settings/{tab}")

        p.goto(f"{self.base}/#/admin")
        p.locator("#admin-targets-body tr").first.wait_for()
        self.audit(p, "admin/targets")
        p.click("#target-add-btn")
        expect(p.locator("#form-target")).to_be_visible()
        p.locator("#t-video option").first.wait_for(state="attached")
        self.audit(p, "admin/target editor")
        p.click("#t-cancel")
        p.click("#atab-users")
        expect(p.locator("#apanel-users")).to_be_visible()
        self.audit(p, "admin/users")
        p.click("#atab-network")
        expect(p.locator("#https-status")).not_to_be_empty()
        for source in ("org", "own"):
            p.click(f"#https-source-{source}")
            expect(p.locator(f"#https-route-{source}")).to_be_visible()
            p.evaluate("document.querySelectorAll('#apanel-network details').forEach(d => d.open = true)")
            self.audit(p, f"admin/network, {source} CA")


if __name__ == "__main__":
    unittest.main()
