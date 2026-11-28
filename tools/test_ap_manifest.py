"""The AP's capability manifest (ap/manifest.py): every board and variant fits ap/protocols.json,
and a build's sdkconfig comes out of it as the boards had it before the manifest"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ap"))
import manifest  # noqa: E402


def board(**kw):
    b = {"id": "test", "radios": {"802.15.4": {"via": "native"}}, "protocols": ["openesl"]}
    b.update(kw)
    return manifest.normalize(b)


class Boards(unittest.TestCase):
    def test_every_board_and_variant_fits(self):
        self.assertEqual(manifest.check_all(), [])

    def test_older_fields_are_filled_in(self):
        by_id = {b["id"]: b for b in manifest.load_boards()}
        self.assertEqual(by_id["c6-supermini"]["radio"], "native")
        self.assertEqual(by_id["s3-n16r8-c6"]["radio"], "uart")
        self.assertEqual(by_id["s3-n16r8-c6"]["radio_fw"], ["c6", "c6-lcd"])
        self.assertEqual(by_id["s3-n16r8-netradio"]["radio"], "net")

    def test_protocol_models(self):
        ps = manifest.load_protocols()
        self.assertIn("29", manifest.protocol_models(ps["openesl"]))
        self.assertEqual(manifest.protocol_models(ps["solum-stock"]), ["29", "154"])
        self.assertTrue(manifest.protocol_models(ps["gicisky"]))


class Checks(unittest.TestCase):
    def test_a_protocol_needs_its_radio(self):
        b = board(protocols=["openesl", "gicisky"])
        self.assertIn("protocol gicisky needs a ble radio", " ".join(manifest.check_build(b)))

    def test_needs_are_checked_on_the_radio(self):
        b = board(radios={"802.15.4": {"via": "uart", "radio_fw": ["c6"]}}, protocols=["solum-stock"])
        self.assertIn("needs its 802.15.4 radio to be native", " ".join(manifest.check_build(b)))
        self.assertEqual(manifest.check_build(board(protocols=["openesl", "solum-stock"])), [])

    def test_conflicting_radio(self):
        # ESL_BLE depends on the stub radio mode (main/Kconfig.projbuild): not next to 802.15.4
        b = board(radios={"802.15.4": {"via": "native"}, "ble": {"via": "native"}}, protocols=["openesl", "gicisky"])
        self.assertIn("gicisky cannot be built with a 802.15.4 radio", " ".join(manifest.check_build(b)))

    def test_the_sniffer_has_no_protocols(self):
        b = board(variants={"sniffer": {"sdkconfig": "sniffer.sdkconfig.defaults"}})
        self.assertIn("the sniffer answers no tags", " ".join(manifest.check_build(b, "sniffer")))
        b = board(variants={"sniffer": {"protocols": [], "sdkconfig": "sniffer.sdkconfig.defaults"}})
        self.assertEqual(manifest.check_build(b, "sniffer"), [])

    def test_unknown_protocol_and_radio(self):
        self.assertTrue(manifest.check_build(board(protocols=["zigbee"])))
        self.assertTrue(manifest.check_build(board(radios={"802.15.4": {"via": "spi"}})))
        self.assertTrue(manifest.check_build(board(radios={"802.15.4": {"via": "uart"}})))

    def test_unknown_variant(self):
        with self.assertRaises(manifest.ManifestError):
            manifest.resolve(board(), "ble")


class Sdkconfig(unittest.TestCase):
    def test_radio_driver_per_way(self):
        for via, driver in (("native", "IEEE802154"), ("uart", "UART"), ("net", "NET")):
            b = board(radios={"802.15.4": {"via": via, "radio_fw": ["c6"]}})
            self.assertIn("CONFIG_ESL_RADIO_DRIVER_%s=y" % driver, manifest.fragment(b))

    def test_protocols_not_in_the_build_are_off(self):
        f = manifest.fragment(board())
        self.assertIn("# CONFIG_ESL_BLE is not set", f)
        self.assertIn("# CONFIG_ESL_SOLUM_STOCK_TAGS is not set", f)

    def test_without_802154_the_stub_radio(self):
        b = board(radios={"ble": {"via": "native"}}, protocols=["gicisky"])
        f = manifest.fragment(b)
        self.assertIn("CONFIG_ESL_RADIO_STUB=y", f)
        self.assertIn("CONFIG_IEEE802154_ENABLED=n", f)
        self.assertNotIn("CONFIG_ESL_BLE is not set", f)

    def test_defaults_order(self):
        by_id = {b["id"]: b for b in manifest.load_boards()}
        self.assertEqual(manifest.defaults_files(by_id["c6-supermini-lcd"], None, "bd"),
                         ["sdkconfig.defaults", "boards/c6-supermini/sdkconfig.defaults",
                          "boards/c6-supermini-lcd/sdkconfig.defaults", os.path.join("bd", "manifest.sdkconfig")])
        self.assertEqual(manifest.defaults_files(by_id["c6-supermini"], "ble", "bd"),
                         ["sdkconfig.defaults", "boards/c6-supermini/sdkconfig.defaults",
                          os.path.join("bd", "manifest.sdkconfig"), "protocols/gicisky.sdkconfig",
                          "ble.sdkconfig.defaults"])

    def test_cmake_output(self):
        import io
        import tempfile
        from contextlib import redirect_stdout
        with tempfile.TemporaryDirectory() as d:
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(manifest.main(["cmake", "c6-supermini", "sniffer", d]), 0)
            files, protocols, app, kconfig = out.getvalue().splitlines()
            self.assertTrue(files.endswith("|sniffer.sdkconfig.defaults"))
            self.assertEqual(protocols, "-")
            self.assertEqual(app, "esl_ap")
            self.assertEqual(kconfig, "-")
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(manifest.main(["cmake", "c6-supermini", "ble", d]), 0)
            self.assertEqual(out.getvalue().splitlines()[1:], ["gicisky", "esl_ap_ble", "ESL_BLE"])
            self.assertTrue(os.path.isfile(os.path.join(d, "manifest.sdkconfig")))


if __name__ == "__main__":
    unittest.main()
