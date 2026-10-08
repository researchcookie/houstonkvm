"""The license and the documents a newcomer reads.

HoustonKVM's own code is Apache-2.0 (LICENSE, NOTICE). houstonkvm.spec's
License tag is a compound SPDX expression instead of a bare match, because
the RPM also bundles MIT/BSD-3-Clause/MPL-2.0 code from its dependencies
(see THIRD_PARTY_NOTICES.md) — Fedora packaging convention wants every
bundled license listed there, not just the project's own. The documents
must exist, and their links and the scripts they name must be real.
"""
import hashlib
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# The canonical Apache License 2.0 text (https://www.apache.org/licenses/LICENSE-2.0.txt).
APACHE_2_0_SHA256 = "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"


class TestLicense(unittest.TestCase):
    def test_license_is_the_unmodified_apache_2_0_text(self):
        digest = hashlib.sha256((REPO / "LICENSE").read_bytes()).hexdigest()
        self.assertEqual(digest, APACHE_2_0_SHA256, "LICENSE is not the canonical Apache-2.0 text")

    def test_notice_and_third_party_notices_exist_and_point_at_the_license(self):
        notice = (REPO / "NOTICE").read_text()
        self.assertIn("Apache License, Version 2.0", notice)
        self.assertIn("THIRD_PARTY_NOTICES.md", notice)
        self.assertTrue((REPO / "THIRD_PARTY_NOTICES.md").is_file())

    def test_the_rpm_declares_apache_2_0_plus_whatever_it_bundles(self):
        # A bare "Apache-2.0" would undersell it: the RPM also bundles
        # MIT/BSD-3-Clause/MPL-2.0 code from its dependencies (see
        # THIRD_PARTY_NOTICES.md), and Fedora convention wants every bundled
        # license listed in the tag, not just the project's own. Apache-2.0
        # must lead the expression, since that's HoustonKVM's own license.
        spec = (REPO / "houstonkvm.spec").read_text()
        m = re.search(r"(?m)^License:\s+(\S.*\S)\s*$", spec)
        self.assertTrue(m, "spec has no License: tag")
        parts = m.group(1).split()
        self.assertEqual(parts[0], "Apache-2.0", "spec License: must lead with the project's own license")
        for extra in ("MIT", "BSD-3-Clause", "MPL-2.0"):
            self.assertIn(extra, parts, f"spec License: is missing {extra} (bundled — see THIRD_PARTY_NOTICES.md)")
        self.assertTrue(re.search(r"(?m)^%license\b.*\bLICENSE\b.*\bNOTICE\b", spec),
                        "spec does not install LICENSE and NOTICE with %license")


DOCS = ["README.md", "CONTRIBUTING.md", "SECURITY.md", "docs/API.md", "docs/install.md", "docs/security.md"]


def github_slug(heading):
    """The anchor GitHub gives a heading."""
    text = re.sub(r"[`*_]", "", heading).strip().lower()
    return re.sub(r"\s", "-", re.sub(r"[^\w\s-]", "", text))


class TestDocumentation(unittest.TestCase):
    def test_the_documents_a_newcomer_expects_exist(self):
        for name in DOCS:
            with self.subTest(doc=name):
                self.assertTrue((REPO / name).is_file(), f"{name} is missing")

    def test_the_readme_names_the_license_and_the_way_to_report_a_vulnerability(self):
        readme = (REPO / "README.md").read_text()
        self.assertIn("Apache License 2.0", readme)
        self.assertIn("SECURITY.md", readme)
        self.assertIn("## License", readme)

    def test_every_local_link_and_anchor_in_the_docs_resolves(self):
        for name in DOCS:
            doc = REPO / name
            text = doc.read_text()
            anchors = {github_slug(h) for h in re.findall(r"(?m)^#{1,6}\s+(.+?)\s*$", text)}
            in_code = re.sub(r"```.*?```", "", text, flags=re.S)        # links inside code samples aren't links
            for target in re.findall(r"\]\(([^)\s]+)\)", in_code):
                if re.match(r"[a-z]+:", target):                        # http:, mailto:, ...
                    continue
                with self.subTest(doc=name, link=target):
                    path, _, anchor = target.partition("#")
                    dest = (doc.parent / path).resolve() if path else doc
                    self.assertTrue(dest.exists(), f"{name} links to {path}, which doesn't exist")
                    if anchor:
                        dest_anchors = anchors if dest == doc else {
                            github_slug(h) for h in re.findall(r"(?m)^#{1,6}\s+(.+?)\s*$", dest.read_text())}
                        self.assertIn(anchor, dest_anchors, f"{name} links to #{anchor}, which isn't a heading in {dest.name}")

    def test_every_script_the_docs_tell_people_to_run_is_in_the_tree(self):
        for name in DOCS:
            for script in sorted(set(re.findall(r"\bscripts/[A-Za-z0-9_.-]+", (REPO / name).read_text()))):
                with self.subTest(doc=name, script=script):
                    self.assertTrue((REPO / script).is_file(), f"{name} mentions {script}, which doesn't exist")


if __name__ == "__main__":
    unittest.main()
