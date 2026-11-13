"""Tests for sniff.py: python -m unittest test_sniff (tools/check.py runs them)."""
import io
import os
import struct
import tempfile
import unittest

import sniff

TAG = bytes.fromhex("153BA64305020000")      # 0000020543A63B15 on the air (little-endian)


def poll_frame(seq=7):
    payload = struct.pack("<BBHHIBB", 0x01, 1, 10100, 2950, 0x1234ABCD, 0x29, 1) + struct.pack("<HBBBB", 3, 0, 0, 2, 0)
    return struct.pack("<HBHH", 0xC841, seq, 0x4553, 0x0000) + TAG + payload


def idle_frame(seq=7):
    return struct.pack("<HBH", 0x8C41, seq, 0x4553) + TAG + struct.pack("<H", 0x0000) + struct.pack("<BBH", 0x02, 1, 60)


def line(us, frame, ch=25, rssi=-60, lqi=200, prefix=""):
    return "%s@SNF %d %d %d %d %s" % (prefix, us, ch, rssi, lqi, frame.hex().upper())


class ParseTest(unittest.TestCase):
    def test_line(self):
        f = sniff.parse_line(line(1500, poll_frame(), prefix="\x1b[0m"))
        self.assertEqual(f["us"], 1500)
        self.assertEqual(f["channel"], 25)
        self.assertEqual(f["rssi"], -60)
        self.assertEqual(f["lqi"], 200)
        self.assertEqual(f["frame"], poll_frame())

    def test_other_lines(self):
        self.assertIsNone(sniff.parse_line("I (123) radio: stats: rx 4"))
        self.assertIsNone(sniff.parse_line("@SNF-ST ch 25 hop 0 rx 0 dropped 0"))
        self.assertIsNone(sniff.parse_line("@SNF 12 25 -60 200 XYZ"))

    def test_empty_frame(self):
        self.assertEqual(sniff.parse_line("@SNF 12 25 -60 200")["frame"], b"")


class DecodeTest(unittest.TestCase):
    def test_poll(self):
        h = sniff.decode_mac(poll_frame())
        self.assertEqual(h["type"], 1)
        self.assertEqual(h["dst"], "0000")
        self.assertEqual(h["src"], "0000020543A63B15")
        self.assertEqual(h["dst_pan"], 0x4553)
        self.assertEqual(h["src_pan"], 0x4553)          # PAN ID compression
        self.assertEqual(h["payload"], 15)              # ESL_MAC_HDR_LEN
        text = sniff.describe(sniff.parse_line(line(0, poll_frame())))
        self.assertIn("POLL fw=10100 bat=2950mV id=1234ABCD model=1", text)
        self.assertIn("[booted,ota,image]", text)
        self.assertIn("0000020543A63B15/4553 -> 0000/4553", text)

    def test_idle_and_gap(self):
        f = sniff.parse_line(line(2600, idle_frame()))
        text = sniff.describe(f, prev_us=1000)
        self.assertIn("+1.6ms", text)
        self.assertIn("IDLE next=60s", text)
        self.assertIn("0000/4553 -> 0000020543A63B15/4553", text)
        self.assertNotIn("unexpected", text)

    def test_wrong_direction_and_ack_request(self):
        # an AP answer with the ACK request bit set: not openESL's frame control, shown as such
        frame = bytearray(idle_frame())
        frame[0] |= 0x20
        text = sniff.describe(sniff.parse_line(line(0, bytes(frame))))
        self.assertIn("ackreq", text)
        self.assertIn("fcf=8C61", text)
        self.assertNotIn("IDLE", text)

    def test_ack_and_short(self):
        self.assertIn("ack seq=9", sniff.describe(sniff.parse_line(line(0, bytes([0x02, 0x00, 9])))))
        self.assertIn("short or odd", sniff.describe(sniff.parse_line(line(0, poll_frame()[:9]))))

    def test_filters(self):
        f = sniff.parse_line(line(0, poll_frame()))
        self.assertTrue(sniff.matches(f, pan=0x4553))
        self.assertFalse(sniff.matches(f, pan=0x1234))
        self.assertTrue(sniff.matches(f, mac="0000020543A63B15"))
        self.assertTrue(sniff.matches(f, mac="0000"))
        self.assertFalse(sniff.matches(f, mac="0000000000000001"))


class RunTest(unittest.TestCase):
    def test_read_log_and_pcap(self):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "in.log")
            pcap = os.path.join(d, "out.pcap")
            with open(log, "w") as fh:
                fh.write("I (5) radio: 802.15.4 sniffer on channel 25\n@SNF-ST ch 25 hop 0 rx 0 dropped 0\n")
                fh.write(line(1000, poll_frame()) + "\n" + line(2600, idle_frame()) + "\n")
                fh.write(line(3000, poll_frame()).replace("5345", "3412", 1) + "\n")
            args = sniff.argparse.Namespace(pcap=pcap, log=None, pan=0x4553, mac=None, verbose=False)
            out = io.StringIO()
            with open(log) as fh:
                n = sniff.run((l.rstrip("\n") for l in fh), args, out)
            self.assertEqual(n, 2)                       # the third is on another PAN
            self.assertIn("# @SNF-ST ch 25", out.getvalue())
            with open(pcap, "rb") as fh:
                data = fh.read()
            magic, _, _, _, _, _, link = struct.unpack_from("<IHHiIII", data)
            self.assertEqual(magic, 0xA1B23C4D)
            self.assertEqual(link, 230)
            self.assertEqual(len(data), 24 + 2 * 16 + len(poll_frame()) + len(idle_frame()))


if __name__ == "__main__":
    unittest.main()
