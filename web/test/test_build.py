"""web/build.py --firmware: which radio image a file is (ap_radio_for), by its CI or release name,
and that its image is built for that radio's link (the project name, check_radio_image).

The release names share prefixes (openesl-ap-radio-esp32c6 and openesl-ap-radio-esp32c6-usb), so
a file must go to the radio with the longest matching name: the Linux AP's C6 on USB must never be
taken for the S3's C6 on a cable, whose image the page and the dashboard offer to an S3's radio.

  python3 -m unittest discover -s web/test -p 'test_*.py'
"""
import contextlib
import io
import json
import os
import struct
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

    def test_add_firmware_lists_the_usb_radio_apart(self):
        # the order ap/build.py --release gives: the USB radio after the S3's, so a wrong match
        # would replace the S3's image; it must land in linux_radios only, never in radios or ota
        with tempfile.TemporaryDirectory() as d:
            f = os.path.join(d, "openesl-ap-radio-esp32c6-usb-v0.11.0.bin")
            open(f, "wb").close()
            saved = build.DIST, build.split_ap_image
            build.DIST = d
            build.split_ap_image = lambda src, out, name: [(name, 0)]   # no real image here
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    build.add_firmware(f)
            finally:
                build.DIST, build.split_ap_image = saved
            with open(os.path.join(d, "firmware", "info.json")) as fh:
                info = json.load(fh)
            self.assertEqual(info["radios"], [])
            self.assertEqual(info["ap_boards"], [])
            self.assertEqual(info["ota"], {})
            self.assertEqual([(r["key"], r["kind"], r["manifest"]) for r in info["linux_radios"]],
                             [("usb-c6-usb", "usb", "radio-linux-c6-usb-manifest.json")])
            self.assertTrue(os.path.exists(os.path.join(d, "firmware", "radio-linux-c6-usb-manifest.json")))


def app_image(project):
    """The start of an ESP32 app image with esp_app_desc_t: header, first segment, then the
    description (magic, secure_version, reserved, version, project_name)"""
    img = bytearray(b"\xff" * 256)
    img[0] = 0xE9
    img[24:32] = b"\0" * 8
    img[32:36] = struct.pack("<I", build.APP_DESC_MAGIC)
    img[36:48] = b"\0" * 12
    img[48:80] = b"0.11.0".ljust(32, b"\0")
    img[80:112] = project.encode().ljust(32, b"\0")
    return bytes(img)


def merged_image(project):
    """A flasher image: bootloader space, a partition table with ota_0 at 0x10000, and the app"""
    img = bytearray(b"\xff" * 0x10000)
    entry = b"\xaa\x50" + struct.pack("<BBII", 0, 0x10, 0x10000, 0x100000) + b"ota_0".ljust(16, b"\0") + b"\0" * 4
    img[build.PT_OFFSET:build.PT_OFFSET + 32] = entry
    return bytes(img) + app_image(project)


class RadioProjects(unittest.TestCase):
    """add_firmware checks the project name in each radio image against the link its file name
    says (#455): a USB image under the S3's radio name, or the other way round, stops the build"""

    def add(self, name, project, ota_project=None):
        with tempfile.TemporaryDirectory() as d:
            f = os.path.join(d, name)
            with open(f, "wb") as fh:
                fh.write(merged_image(project))
            with open(f[:-4] + "-ota.bin", "wb") as fh:
                fh.write(app_image(ota_project or project))
            saved = build.DIST
            build.DIST = d
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    build.add_firmware(f)
            finally:
                build.DIST = saved
            with open(os.path.join(d, "firmware", "info.json")) as fh:
                return json.load(fh)

    def test_each_radio_with_its_own_project(self):
        info = self.add("openesl-ap-radio-esp32c6-v0.11.0.bin", "esl_radio")
        self.assertEqual([r["key"] for r in info["radios"]], ["uart-c6"])
        info = self.add("openesl-ap-radio-esp32c6-lcd-v0.11.0.bin", "esl_radio")
        self.assertEqual([r["key"] for r in info["radios"]], ["uart-c6-lcd"])
        info = self.add("openesl-ap-netradio-esp32c6-v0.11.0.bin", "esl_netradio")
        self.assertEqual([r["key"] for r in info["radios"]], ["net-c6"])
        info = self.add("openesl-ap-radio-esp32c6-usb-v0.11.0.bin", "esl_radio_usb")
        self.assertEqual([r["key"] for r in info["linux_radios"]], ["usb-c6-usb"])

    def test_usb_image_under_the_s3_radio_name_stops_the_build(self):
        for name in ("openesl-ap-radio-esp32c6-v0.11.0.bin", "openesl-ap-radio-c6.bin",
                     "openesl-ap-radio-c6-lcd.bin"):
            with self.subTest(name=name), self.assertRaises(SystemExit) as e:
                self.add(name, "esl_radio_usb")
            self.assertIn("esl_radio_usb, not esl_radio", str(e.exception))

    def test_uart_image_under_the_usb_radio_name_stops_the_build(self):
        with self.assertRaises(SystemExit) as e:
            self.add("openesl-ap-radio-esp32c6-usb-v0.11.0.bin", "esl_radio")
        self.assertIn("not esl_radio_usb", str(e.exception))

    def test_wrong_ota_image_next_to_a_right_flasher_image(self):
        with self.assertRaises(SystemExit) as e:
            self.add("openesl-ap-radio-esp32c6-v0.11.0.bin", "esl_radio", ota_project="esl_radio_usb")
        self.assertIn("-ota.bin", str(e.exception))

    def test_netradio_image_under_the_uart_radio_name_stops_the_build(self):
        with self.assertRaises(SystemExit):
            self.add("openesl-ap-radio-esp32c6-v0.11.0.bin", "esl_netradio")


if __name__ == "__main__":
    unittest.main()
