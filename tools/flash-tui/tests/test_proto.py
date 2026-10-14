"""Protocol tests against the simulated ZBS_Flasher. Run: python -m unittest discover -s tools/flash-tui/tests"""
import unittest
from unittest import mock

import helpers  # noqa: F401  (path + real-port guard)
import fake_flasher as ff
import zbs_proto as zp


class FrameTest(unittest.TestCase):
    def test_encode_no_data(self):
        # 'A' 'T' cmd=1 len=0 crc=0xAB34+1 big endian
        self.assertEqual(zp.encode_frame(1), b"AT\x01\x00\xab\x35")

    def test_encode_with_data(self):
        f = zp.encode_frame(22, bytes([0xFF, 0x12, 0x34]))
        self.assertEqual(f[:7], b"AT\x16\x03\xff\x12\x34")
        self.assertEqual(int.from_bytes(f[7:], "big"), (0xAB34 + 22 + 3 + 0xFF + 0x12 + 0x34) & 0xFFFF)

    def test_crc_wraps(self):
        f = zp.encode_frame(23, bytes([0xFF] * 250))
        self.assertEqual(int.from_bytes(f[-2:], "big"), (0xAB34 + 23 + 250 + 0xFF * 250) & 0xFFFF)

    def test_too_long(self):
        with self.assertRaises(ValueError):
            zp.encode_frame(23, bytes(256))


class FlasherTest(unittest.TestCase):
    def setUp(self):
        self.tag = ff.FakeTag()
        self.port = ff.FakeFlasherPort(self.tag)
        self.fl = zp.ZbsFlasher(self.port)

    def test_version_and_probe(self):
        self.assertEqual(self.fl.version(), ff.FLASHER_VERSION)
        self.assertEqual(zp.probe(self.fl, wait_s=0), ff.FLASHER_VERSION)
        # probe must not touch the tag
        self.assertEqual(set(self.port.commands), {zp.CMD_GET_VERSION})

    def test_probe_rejects_debug_wemos(self):
        fl = zp.ZbsFlasher(ff.FakeDebugWemosPort())
        with self.assertRaises(zp.FlasherError) as cm:
            zp.probe(fl, wait_s=0)
        self.assertIn("ZBS_Flasher", str(cm.exception))

    def test_probe_rejects_silent_port(self):
        port = ff.FakeFlasherPort(self.tag)
        port.drop_next = 100
        with self.assertRaises(zp.FlasherError):
            zp.probe(zp.ZbsFlasher(port), wait_s=0)

    def test_begin_no_tag(self):
        fl = zp.ZbsFlasher(ff.FakeFlasherPort(None))
        with self.assertRaises(zp.NoTagError):
            fl.begin()
        self.assertFalse(fl.tag_present())

    def test_read_write_roundtrip(self):
        self.fl.begin()
        self.fl.erase_main()
        data = bytes((i * 7) & 0xFF for i in range(1000))
        self.fl.write(0x100, data)
        self.assertEqual(self.fl.read(0x100, 1000), data)
        self.assertEqual(bytes(self.tag.flash[0x100:0x100 + 1000]), data)

    def test_read_chunks_at_most_255(self):
        self.fl.begin()
        sent = []
        orig = self.port.write
        self.port.write = lambda b: (sent.append(b), orig(b))[1]
        self.fl.read(0, 600)
        reads = [b for b in sent if b[2] == zp.CMD_READ_FLASH]
        self.assertEqual([b[4] for b in reads], [255, 255, 90])

    def test_write_skips_blank_chunks(self):
        self.fl.begin()
        self.fl.erase_main()
        data = b"\x01" * 10 + b"\xff" * 600 + b"\x02"
        self.fl.write(0, data)
        self.assertEqual(self.port.commands.count(zp.CMD_WRITE_FLASH), 2)
        self.assertEqual(self.fl.read(0, len(data)), data)

    def test_info_page_select(self):
        self.tag.info[0x10:0x18] = bytes(range(8))
        self.fl.begin()
        self.fl.select_page(zp.PAGE_INFO)
        self.assertEqual(self.fl.read(0x10, 8), bytes(range(8)))
        self.fl.select_page(zp.PAGE_MAIN)
        self.assertEqual(self.fl.read(0x10, 8), b"\xff" * 8)

    def test_write_failure_and_double_answer(self):
        """The firmware answers [0] and then [1]; the extra answer must not confuse the next command."""
        self.fl.begin()
        self.fl.erase_main()
        self.tag.stuck_bits[(0, 5)] = 0x01     # bit 0 of 0x0005 never clears
        with self.assertRaises(zp.FlasherError):
            self.fl.write_chunk(0, b"\x00" * 10)
        self.assertEqual(self.fl.version(), ff.FLASHER_VERSION)
        self.assertEqual(self.fl.read(0, 4), b"\x00" * 4)

    def test_retry_on_crc_error_and_lost_answer(self):
        self.fl.begin()
        self.port.corrupt_next = 1
        self.assertEqual(self.fl.version(), ff.FLASHER_VERSION)
        self.port.drop_next = 1
        self.assertEqual(self.fl.version(), ff.FLASHER_VERSION)
        self.port.drop_next = 5
        with self.assertRaises(zp.FlasherError):
            self.fl.version()

    def test_noise_before_answer(self):
        self.port.noise = b"\x00garbage\r\nets Jan  8 2013,rst cause:2\r\n"
        self.assertEqual(self.fl.version(), ff.FLASHER_VERSION)

    def test_bounds(self):
        with self.assertRaises(ValueError):
            self.fl.read_chunk(0xFFF0, 0x20)
        with self.assertRaises(ValueError):
            self.fl.write_chunk(0, b"\x00" * 251)

    def test_no_info_erase_command(self):
        self.assertFalse(hasattr(zp, "CMD_ERASE_INFOBLOCK"))
        self.assertFalse(any("info" in n and "erase" in n for n in dir(zp.ZbsFlasher)))


class PortListTest(unittest.TestCase):
    def test_list_ports(self):
        class P:
            def __init__(self, dev, loc, vid):
                self.device, self.location, self.vid, self.pid = dev, loc, vid, 0x7523
                self.serial_number, self.description = None, "USB Serial"
        fake = [P("/dev/ttyUSB1", "1-2:1.0", 0x1A86), P("/dev/ttyUSB0", None, 0x10C4)]
        with mock.patch("serial.tools.list_ports.comports", return_value=fake):
            ports = zp.list_ports()
        self.assertEqual([p["device"] for p in ports], ["/dev/ttyUSB0", "/dev/ttyUSB1"])
        self.assertEqual(ports[1]["location"], "1-2:1.0")
        self.assertTrue(ports[1]["ch340"])
        self.assertFalse(ports[0]["ch340"])

    def test_real_ports_are_blocked_in_tests(self):
        with self.assertRaises(AssertionError):
            zp.open_port("/dev/ttyUSB-does-not-matter")


if __name__ == "__main__":
    unittest.main()
