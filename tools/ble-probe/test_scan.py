"""Tests for scan.py's output, without a Bluetooth adapter or bleak (python3 -m unittest test_scan)."""
import io
import unittest
from types import SimpleNamespace

import scan


def adv(name, rssi, maker=None, uuids=None):
    return SimpleNamespace(local_name=name, rssi=rssi, manufacturer_data=maker or {}, service_uuids=uuids or [],
                           service_data={}, tx_power=None)


class TestScan(unittest.TestCase):
    def test_rows_strongest_first(self):
        rows = [scan.adv_row(SimpleNamespace(address="AA:00:00:00:00:01", name=None), adv(None, -80)),
                scan.adv_row(SimpleNamespace(address="AA:00:00:00:00:02", name="x"),
                             adv("Tag 29", -40, {0x1234: b"\x01\x02"}, ["0000fef1-0000-1000-8000-00805f9b34fb"]))]
        self.assertEqual(rows[1]["manufacturer"], {"0x1234": "0102"})
        out = io.StringIO()
        scan.print_rows(rows, out)
        lines = out.getvalue().splitlines()
        self.assertTrue(lines[0].startswith("AA:00:00:00:00:02   -40 dBm  Tag 29"))
        self.assertIn("    maker 0x1234: 0102", lines)
        self.assertIn("(no name)", out.getvalue())

    def test_gatt_print(self):
        g = {"address": "AA:00:00:00:00:02", "mtu": 185, "services": [
            {"uuid": "s1", "handle": 1, "characteristics": [
                {"uuid": "c1", "handle": 0x21, "properties": ["write-without-response", "notify"],
                 "descriptors": [{"uuid": "00002902", "handle": 0x22}], "value": "0a"}]}]}
        out = io.StringIO()
        scan.print_gatt(g, out)
        s = out.getvalue()
        self.assertIn("AA:00:00:00:00:02, MTU 185", s)
        self.assertIn("0x0021 c1 [write-without-response,notify] = 0a", s)
        self.assertIn("0x0022 00002902", s)


if __name__ == "__main__":
    unittest.main()
