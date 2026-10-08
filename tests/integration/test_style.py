"""The style rules the stylesheets promise, checked without a browser.

ui/css/tokens.css is the one place a colour, a font or a radius is named
(see the comment at its top). These tests hold every other file to that, and
check the palette itself: every pairing of text and background the UI uses
meets WCAG AA (4.5:1), and every line that has to be seen — focus ring,
selected tab, control edge, status dot — meets the 3:1 that WCAG 1.4.11 asks
of non-text contrast. They read the CSS directly, so a token change that
breaks contrast fails here in milliseconds, before any screen is drawn.
(tests/ui/test_a11y.py measures the rendered pages as well.)
"""
import re
import unittest
from pathlib import Path

UI_DIR = Path(__file__).resolve().parents[2] / "ui"
CSS_DIR = UI_DIR / "css"
TOKENS = CSS_DIR / "tokens.css"

COLOUR_LITERAL = re.compile(
    r"#[0-9a-fA-F]{3,8}\b|\b(?:rgba?|hsla?|hwb|lab|lch|oklab|oklch|color)\(|"
    r"\b(?:white|black|red|green|blue|gray|grey|silver|orange|yellow|purple|pink|brown)\b")


def strip_comments(css):
    return re.sub(r"/\*.*?\*/", "", css, flags=re.S)


def declarations(css):
    """(property, value) for every declaration in a stylesheet — read from
    the innermost {…} bodies only, so selectors like a:hover never count."""
    for body in re.findall(r"\{([^{}]*)\}", strip_comments(css)):
        for decl in body.split(";"):
            prop, sep, value = decl.partition(":")
            if sep and prop.strip():
                yield prop.strip(), value.strip()


def token_blocks():
    """{selector: {token: value}} for every block in tokens.css."""
    blocks = {}
    for selector, body in re.findall(r"([^{}]+)\{([^{}]*)\}", strip_comments(TOKENS.read_text())):
        blocks[selector.strip()] = dict(re.findall(r"(--[\w-]+)\s*:\s*([^;]+);", body))
    return blocks


def resolve(tokens, name, depth=0):
    value = tokens[name].strip()
    m = re.fullmatch(r"var\((--[\w-]+)\)", value)
    if m:
        if depth > 10:
            raise ValueError(f"{name}: var() loop")
        return resolve(tokens, m.group(1), depth + 1)
    return value


def luminance(hex_colour):
    h = hex_colour.lstrip("#")
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    def lin(c):
        c /= 255
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (lin(int(h[i:i + 2], 16)) for i in (0, 2, 4))
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    hi, lo = sorted((luminance(a), luminance(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)


# Every foreground/background pairing of tokens the stylesheets actually put
# together. Adding a new pairing to the CSS means adding it here too.
TEXT_ON = {  # 4.5:1 — normal-size text
    ("--text", "--muted", "--link", "--danger", "--success", "--nosig"): ("--bg", "--surface", "--surface-alt"),
    ("--accent-ink",): ("--accent",),
    ("--primary-ink",): ("--primary",),
    ("--header-fg", "--header-muted"): ("--header-bg",),
    ("--header-bg",): ("--header-fg",),        # the Owner's filled role badge
    ("--bg",): ("--muted", "--success"),       # a disabled button, a finished step's number
    ("--seashells", "--sunkissed-sand"): ("--viewport-bg",),  # notes over the remote screen
}
LINE_ON = {  # 3:1 — non-text: rings, underlines, edges, status dots
    ("--accent-line", "--control-border", "--live", "--nosig", "--off", "--danger"): ("--bg", "--surface", "--surface-alt"),
    ("--header-accent",): ("--header-bg",),
}


class TestStyleRules(unittest.TestCase):
    def test_colours_are_named_only_in_tokens_css(self):
        for css in sorted(CSS_DIR.glob("*.css")):
            if css == TOKENS:
                continue
            for prop, value in declarations(css.read_text()):
                with self.subTest(file=css.name, prop=prop):
                    self.assertIsNone(COLOUR_LITERAL.search(value),
                                      f"{css.name}: {prop}: {value} — name colours in tokens.css and use var()")

    def test_fonts_are_named_only_in_tokens_css(self):
        for css in sorted(CSS_DIR.glob("*.css")):
            if css == TOKENS:
                continue
            # @font-face blocks name the family they define; that's the one exception.
            text = re.sub(r"@font-face\s*\{[^}]*\}", "", css.read_text())
            for prop, value in declarations(text):
                if prop in ("font-family", "font"):
                    with self.subTest(file=css.name, value=value):
                        self.assertRegex(value, r"^(inherit|var\(--(font|font-display|mono)\))$")

    def test_text_colours_come_from_the_contrast_table(self):
        # Ties the stylesheets to TEXT_ON: a rule can only colour text with a
        # token whose pairings are checked above, so a hover or a new
        # component can't quietly use a colour that's only fit for lines.
        allowed = {fg for fgs in TEXT_ON for fg in fgs}
        for css in sorted(CSS_DIR.glob("*.css")):
            for prop, value in declarations(css.read_text()):
                if prop != "color" or value in ("inherit", "currentColor"):
                    continue
                with self.subTest(file=css.name, value=value):
                    m = re.fullmatch(r"var\((--[\w-]+)\)", value)
                    self.assertTrue(m and m.group(1) in allowed,
                                    f"{css.name}: color: {value} is not a text token in TEXT_ON")

    def test_every_custom_property_used_is_defined(self):
        # An undefined var() fails silently: the property just falls back to
        # its initial value. A typo'd token name is otherwise invisible.
        defined = set()
        for css in CSS_DIR.glob("*.css"):
            defined |= {p for p, _ in declarations(css.read_text()) if p.startswith("--")}
        sources = [*CSS_DIR.glob("*.css"), UI_DIR / "index.html", *(UI_DIR / "js").glob("*.js")]
        for src in sorted(sources):
            for name in sorted(set(re.findall(r"var\((--[\w-]+)", src.read_text()))):
                with self.subTest(file=src.name, token=name):
                    self.assertIn(name, defined)

    def test_every_token_is_used(self):
        # A token nothing reads is a colour the palette claims and the page
        # never shows; drop it or use it.
        tokens = set(token_blocks()[":root"])
        used = set()
        for src in [*CSS_DIR.glob("*.css"), UI_DIR / "index.html", *(UI_DIR / "js").glob("*.js")]:
            used |= set(re.findall(r"var\((--[\w-]+)", src.read_text()))
        self.assertEqual(sorted(tokens - used), [])

    def test_palette_contrast_meets_wcag_aa(self):
        blocks = token_blocks()
        root = blocks[":root"]
        themes = {"normal": root}
        for selector, overrides in blocks.items():
            if selector != ":root":
                themes[selector] = {**root, **overrides}
        for theme, tokens in themes.items():
            for table, need in ((TEXT_ON, 4.5), (LINE_ON, 3.0)):
                for fgs, bgs in table.items():
                    for fg in fgs:
                        for bg in bgs:
                            with self.subTest(theme=theme, fg=fg, bg=bg):
                                a, b = resolve(tokens, fg), resolve(tokens, bg)
                                ratio = contrast(a, b)
                                self.assertGreaterEqual(ratio, need, f"{fg} {a} on {bg} {b} is {ratio:.2f}:1")


if __name__ == "__main__":
    unittest.main()
