"""web/build.py --firmware: which radio image a file is (ap_radio_for), by its CI or release name.

The release names share prefixes (openesl-ap-radio-esp32c6 and openesl-ap-radio-esp32c6-usb), so
a file must go to the radio with the longest matching name: the Linux AP's C6 on USB must never be
taken for the S3's C6 on a cable, whose image the page and the dashboard offer to an S3's radio.

  python3 -m unittest discover -s web/test -p 'test_*.py'
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import build  # noqa: E402


class RadioNames(unittest.TestCase):
    def test_every_radio_by_its_own_names(self):
        radios = build.ap_radios(linux=True)
        self.assertTrue(any(r.get("host") == "linux" for r in radios), "no Linux AP radio (c6-usb) to check")
        for r in radios:
            for name in (r["artifact"] + ".bin",
                         r["release"] + "-v0.11.0.bin",
                         r["release"] + "-v0.11.0-rc1.bin",
                         "fw/ap-firmware/" + r["artifact"] + ".bin"):
                with self.subTest(name=name):
                    got = build.ap_radio_for(name)
                    self.assertIsNotNone(got)
                    self.assertEqual((got["key"], got.get("host")), (r["key"], r.get("host")))

    def test_usb_radio_is_never_the_s3_cable_radio(self):
        for name in ("openesl-ap-radio-c6-usb.bin", "rel/openesl-ap-radio-esp32c6-usb-v0.11.0.bin"):
            with self.subTest(name=name):
                r = build.ap_radio_for(name)
                self.assertEqual((r["id"], r.get("host")), ("c6-usb", "linux"))
        self.assertEqual(build.ap_radio_for("openesl-ap-radio-esp32c6-v0.11.0.bin")["key"], "uart-c6")
        self.assertEqual(build.ap_radio_for("openesl-ap-radio-esp32c6-lcd-v0.11.0.bin")["key"], "uart-c6-lcd")

    def test_the_page_never_lists_the_linux_radio(self):
        self.assertFalse(any(r.get("host") == "linux" for r in build.ap_radios()))

    def test_add_firmware_leaves_the_usb_radio_out(self):
        # the order ap/build.py --release gives: the USB radio after the S3's, so a wrong match
        # would replace the S3's image; with nothing else given, nothing may be listed
        with tempfile.TemporaryDirectory() as d:
            f = os.path.join(d, "openesl-ap-radio-esp32c6-usb-v0.11.0.bin")
            open(f, "wb").close()
            dist, build.DIST = build.DIST, d
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    build.add_firmware(f)
            finally:
                build.DIST = dist
            with open(os.path.join(d, "firmware", "info.json")) as fh:
                info = json.load(fh)
            self.assertEqual(info["radios"], [])
            self.assertEqual(info["ap_boards"], [])
            self.assertEqual(info["ota"], {})


if __name__ == "__main__":
    unittest.main()
