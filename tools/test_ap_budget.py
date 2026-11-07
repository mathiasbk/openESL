#!/usr/bin/env python3
"""Tests for the AP flash budget in ap/build.py (app_slot, --sizes) and the check that keeps
what is only for the web page out of the AP's web pack (ap/main/webui.py, PAGES_ONLY).

    cd tools && python3 -m unittest -v test_ap_budget
"""
import importlib.util
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


build = load("ap_build", "ap/build.py")
webui = load("ap_webui", "ap/main/webui.py")
KB = 1024


def entry(kind, sub, offset, size, label):
    return struct.pack("<HBBII16sI", 0x50AA, kind, sub, offset, size, label.encode(), 0)


class AppSlot(unittest.TestCase):
    def test_smallest_app_partition(self):
        # the C6 table (ap/boards/c6-supermini/partitions.csv), then the MD5 entry and padding
        table = (entry(1, 2, 0x9000, 0x75000, "nvs") + entry(1, 0, 0x7E000, 0x2000, "otadata")
                 + entry(0, 0x10, 0x90000, 0x180000, "ota_0") + entry(0, 0x11, 0x210000, 0x180000, "ota_1")
                 + entry(1, 0x40, 0x390000, 0x70000, "tagfw") + b"\xeb\xeb" + b"\xff" * 30 + b"\xff" * 64)
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(table)
        try:
            self.assertEqual(build.app_slot(f.name), 0x180000)
        finally:
            os.remove(f.name)

    def test_no_app_partition(self):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(entry(1, 2, 0x9000, 0x6000, "nvs") + b"\xff" * 32)
        try:
            with self.assertRaises(SystemExit):
                build.app_slot(f.name)
        finally:
            os.remove(f.name)


class Sizes(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = os.path.join(self.tmp.name, "out")
        self.base = os.path.join(self.tmp.name, "base")
        os.makedirs(self.out)
        os.makedirs(self.base)

    def tearDown(self):
        self.tmp.cleanup()

    def image(self, artifact, app, slot=0x180000):
        with open(os.path.join(self.out, artifact + ".size.json"), "w") as f:
            json.dump({"id": artifact, "artifact": artifact, "app": app, "slot": slot}, f)

    def sizes(self, *args):
        out = io.StringIO()
        with mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": os.path.join(self.tmp.name, "summary.md")}), \
                redirect_stdout(out):
            try:
                build.sizes(list(args))
                code = 0
            except SystemExit as e:
                code = e.code
        return code, out.getvalue()

    def test_ok_and_change_against_base(self):
        self.image("openesl-ap", 0x180000 - 200 * KB)
        # an older build: only the app itself (-ota.bin), as before the budget
        with open(os.path.join(self.base, "openesl-ap-ota.bin"), "wb") as f:
            f.write(b"\0" * (0x180000 - 210 * KB))
        code, out = self.sizes(self.out, "--base", self.base)
        self.assertEqual(code, 0)
        self.assertIn("| openesl-ap | 1336.0 KB | 1536.0 KB | 200.0 KB (13%) | +10.0 KB | ok |", out)
        with open(os.path.join(self.tmp.name, "summary.md")) as f:
            self.assertIn("### AP flash budget", f.read())

    def test_warns_under_128_kb(self):
        self.image("openesl-ap", 0x180000 - 100 * KB)
        code, out = self.sizes(self.out)
        self.assertEqual(code, 0)
        self.assertIn("::warning::", out)
        self.assertIn("| n/a |", out)

    def test_fails_under_64_kb(self):
        self.image("openesl-ap", 0x180000 - 100 * KB)
        self.image("openesl-ap-lcd", 0x180000 - 60 * KB)
        code, out = self.sizes(self.out, "--base", os.path.join(self.tmp.name, "missing"))
        self.assertNotEqual(code, 0)
        self.assertIn("::error::openesl-ap-lcd", out)

    def test_nothing_built(self):
        code, _ = self.sizes(self.out)
        self.assertNotEqual(code, 0)


class PagesOnly(unittest.TestCase):
    def test_every_module_packs_without_pages_only_files(self):
        files = webui.page_files(os.path.join(ROOT, "web"), list(webui.MODULES), renderer=False)
        self.assertTrue(files)
        for path, mime, _ in files:
            self.assertFalse(webui.PAGES_ONLY.search(path), path)
            self.assertFalse(mime.startswith("image/"), path)

    def test_demo_in_a_module_is_refused(self):
        modules = json.loads(json.dumps(webui.MODULES))
        modules["dashboard"]["js"].append("demoap.js")
        with tempfile.TemporaryDirectory() as web:
            for sub in ("js", "css"):
                os.makedirs(os.path.join(web, sub))
            with open(os.path.join(ROOT, "web", "index.html"), "rb") as src, \
                    open(os.path.join(web, "index.html"), "wb") as dst:
                dst.write(src.read())
            open(os.path.join(web, "css", "style.css"), "w").close()
            for name in {"aptabs.js", "theme.js", *modules["dashboard"]["js"]}:
                open(os.path.join(web, "js", name), "w").close()
            with mock.patch.object(webui, "MODULES", modules), self.assertRaises(SystemExit) as e:
                webui.page_files(web, ["dashboard"])
        self.assertIn("demoap.js", str(e.exception))


if __name__ == "__main__":
    unittest.main()
