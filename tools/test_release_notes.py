"""Tests for the upgrade instructions in release_notes.py (python3 -m unittest test_release_notes)."""

import os
import unittest

import release_notes

DOC = """# Making a release

## 4. Upgrade instructions

### Upgrading over more than one release

Go straight to the newest.

### Upgrading from 0.5.x to 0.6.0

Either order works ([protokoll.md](protokoll.md), [VERSION](../VERSION#x)).

### Upgrading from 0.6.x to 0.7.0

Order: only the AP, see [a page](https://example.com/a) and [the rollout](#3-rollout-to-the-tags).

Every detail, not in the notes.

## 5. A bad release

Not part of it.

## 6. Summaries

### Summary of 0.7.0

New icons ([protokoll.md](protokoll.md)).

### Summary of 0.6.0
"""


class UpgradeNotes(unittest.TestCase):
    def test_section_for_the_version(self):
        heading, body = release_notes.upgrade_section(DOC, "0.7.0")
        self.assertEqual(heading, "Upgrading from 0.6.x to 0.7.0")
        self.assertTrue(body.startswith("Order: only the AP,"))
        self.assertNotIn("bad release", body)

    def test_no_section(self):
        self.assertIsNone(release_notes.upgrade_section(DOC, "0.7.1"))
        self.assertIsNone(release_notes.upgrade_section(DOC, "0.7"))
        with self.assertRaises(SystemExit) as e:
            release_notes.upgrade_notes(DOC, "0.7.1", "v0.7.1")
        self.assertIn("Upgrading from <previous> to 0.7.1", str(e.exception))

    def test_links_point_at_the_release(self):
        notes = release_notes.upgrade_notes(DOC, "0.6.0", "v0.6.0")
        base = "https://github.com/%s/blob/v0.6.0/" % release_notes.REPO
        self.assertTrue(notes.startswith("## Upgrading from 0.5.x to 0.6.0\n"))
        self.assertIn("](%sdocs/protokoll.md)" % base, notes)
        self.assertIn("](%sVERSION#x)" % base, notes)
        self.assertIn("](%sdocs/release.md#upgrading-from-05x-to-060)" % base, notes)
        self.assertIn("#upgrading-over-more-than-one-release", notes)

    def test_first_paragraph_only(self):
        notes = release_notes.upgrade_notes(DOC, "0.7.0", "v0.7.0")
        self.assertIn("Order: only the AP", notes)
        self.assertNotIn("Every detail, not in the notes", notes)

    def test_summary(self):
        intro = release_notes.summary(DOC, "0.7.0", "v0.7.0")
        self.assertEqual(intro, "New icons ([protokoll.md](https://github.com/%s/blob/v0.7.0/docs/protokoll.md)).\n"
                         % release_notes.REPO)
        for version in ("0.6.0", "0.7.1"):  # empty, missing
            with self.assertRaises(SystemExit) as e:
                release_notes.summary(DOC, version, "v" + version)
            self.assertIn("Summary of %s" % version, str(e.exception))

    def test_other_links_kept(self):
        notes = release_notes.upgrade_notes(DOC, "0.7.0", "v0.7.0")
        self.assertIn("](https://example.com/a)", notes)
        self.assertIn("](#3-rollout-to-the-tags)", notes)

    def test_the_current_version_has_a_section(self):
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        version = open(os.path.join(root, "VERSION")).read().strip()
        doc = open(os.path.join(root, release_notes.RELEASE_MD), encoding="utf-8").read()
        self.assertIsNotNone(release_notes.upgrade_section(doc, version),
                             "docs/release.md needs \"### Upgrading from <previous> to %s\"" % version)
        self.assertIsNotNone(release_notes.upgrade_section(doc, version, release_notes.SUMMARY_HEAD),
                             "docs/release.md needs \"### Summary of %s\"" % version)


if __name__ == "__main__":
    unittest.main()
