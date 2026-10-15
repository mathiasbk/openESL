"""Per-tag job tests against the simulated flasher and tag."""
import csv
import os
import tempfile
import unittest

import helpers  # noqa: F401
import fake_flasher as ff
import settings
import workflow as wf
import zbs_proto as zp


def make_fw(size=3000, seed=3):
    data = bytearray((i * 31 + seed) & 0xFF for i in range(size))
    data[100:400] = b"\xff" * 300        # a blank run, like padding in a real image
    return bytes(data)


class WorkflowTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.backup = os.path.join(self.dir, "backup")
        self.fw = make_fw()
        self.fw_path = os.path.join(self.dir, "fw.bin")
        with open(self.fw_path, "wb") as f:
            f.write(self.fw)
        self.tag = ff.stock_tag("29", seed=1)
        self.stock_flash = bytes(self.tag.flash)
        self.stock_info = bytes(self.tag.info)
        self.port = ff.FakeFlasherPort(self.tag)
        self.fl = zp.ZbsFlasher(self.port)
        self.lines = []

    def tearDown(self):
        self.tmp.cleanup()

    def run_job(self, model="29", fw_path=None, **kw):
        return wf.flash_tag(self.fl, model, fw_path or self.fw_path, backup_dir=self.backup,
                            log=self.lines.append, **kw)

    # ---- MAC helpers ----

    def test_mac_from_stock_matches_existing_backups(self):
        # Values from the user's earlier ZBS_Flasher backups (flash 0xFC06 -> MAC folder name)
        self.assertEqual(wf.mac_str(wf.mac_from_stock(bytes.fromhex("020793500102"), "154")),
                         "000002079350341C")
        self.assertEqual(wf.mac_str(wf.mac_from_stock(bytes.fromhex("020663460102"), "154")),
                         "0000020663463415")
        self.assertEqual(wf.mac_str(wf.mac_from_stock(bytes.fromhex("01c280920102"), "29")),
                         "000001C280923B15")
        self.assertIsNone(wf.mac_from_stock(b"\xff" * 6, "29"))

    def test_mac_from_info_lsb_first(self):
        info = bytearray(b"\xff" * 0x400)
        info[0x10:0x18] = bytes.fromhex("1c34509307020000")
        self.assertEqual(wf.mac_str(wf.mac_from_info(info)), "000002079350341C")
        self.assertEqual(wf.model_of_mac(wf.mac_from_info(info)), "154")
        self.assertIsNone(wf.mac_from_info(b"\xff" * 0x400))

    # ---- full job ----

    def test_stock_tag(self):
        r = self.run_job()
        self.assertTrue(r.ok, r.message)
        mac = wf.mac_from_stock(self.stock_flash[0xFC06:0xFC0C], "29")
        self.assertEqual(r.mac, mac)
        self.assertEqual(r.mac_source, "stock flash")
        self.assertEqual(r.backup, "saved")
        # backup is the untouched stock flash and info page
        with open(os.path.join(self.backup, wf.mac_str(mac) + ".bin"), "rb") as f:
            self.assertEqual(f.read(), self.stock_flash)
        with open(os.path.join(self.backup, wf.mac_str(mac) + ".info.bin"), "rb") as f:
            self.assertEqual(f.read(), self.stock_info)
        # firmware written, rest of main flash erased
        self.assertEqual(bytes(self.tag.flash[:len(self.fw)]), self.fw)
        self.assertEqual(bytes(self.tag.flash[len(self.fw):]), b"\xff" * (0x10000 - len(self.fw)))
        # info page: never erased, only the MAC bytes changed
        self.assertEqual(self.tag.erase_count, [1, 0])
        self.assertEqual(bytes(self.tag.info[0x10:0x18]), mac[::-1])
        self.assertEqual(bytes(self.tag.info[:0x10]), self.stock_info[:0x10])
        self.assertEqual(bytes(self.tag.info[0x18:]), self.stock_info[0x18:])
        self.assertNotIn(27, self.port.commands)
        self.assertEqual(self.port.commands[-1], zp.CMD_RESET_ZBS)

    def test_second_run_uses_info_mac_and_keeps_backup(self):
        r1 = self.run_job()
        path = os.path.join(self.backup, wf.mac_str(r1.mac) + ".bin")
        mtime = os.stat(path).st_mtime_ns
        fw2 = make_fw(5000, seed=9)
        p2 = os.path.join(self.dir, "fw2.bin")
        with open(p2, "wb") as f:
            f.write(fw2)
        r2 = self.run_job(fw_path=p2)
        self.assertTrue(r2.ok, r2.message)
        self.assertEqual(r2.mac, r1.mac)
        self.assertEqual(r2.mac_source, "info page")
        self.assertEqual(r2.backup, "exists")
        self.assertEqual(os.stat(path).st_mtime_ns, mtime)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), self.stock_flash)
        self.assertEqual(bytes(self.tag.flash[:len(fw2)]), fw2)
        self.assertEqual(self.tag.erase_count[1], 0)

    def test_legacy_backup_folder_is_recognised(self):
        mac = wf.mac_from_stock(self.stock_flash[0xFC06:0xFC0C], "29")
        os.makedirs(os.path.join(self.backup, wf.mac_str(mac)))
        with open(os.path.join(self.backup, wf.mac_str(mac), "flash.bin"), "wb") as f:
            f.write(self.stock_flash)
        r = self.run_job()
        self.assertTrue(r.ok, r.message)
        self.assertEqual(r.backup, "exists")
        self.assertFalse(os.path.exists(os.path.join(self.backup, wf.mac_str(mac) + ".bin")))

    def test_no_tag(self):
        self.port.tag = None
        r = self.run_job()
        self.assertFalse(r.ok)
        self.assertEqual(r.step, "contact")
        self.assertNotIn(zp.CMD_ERASE_FLASH, self.port.commands)

    def test_missing_firmware_touches_nothing(self):
        r = self.run_job(fw_path=os.path.join(self.dir, "nope.bin"))
        self.assertFalse(r.ok)
        self.assertEqual(r.step, "firmware")
        self.assertIn("not found", r.message)
        self.assertEqual(self.port.commands, [])

    def test_firmware_too_big(self):
        p = os.path.join(self.dir, "big.bin")
        with open(p, "wb") as f:
            f.write(b"\x00" * (0xF400 + 1))   # one byte into the layout store
        r = self.run_job(fw_path=p)
        self.assertEqual(r.step, "firmware")
        self.assertIn("0xF400", r.message)
        self.assertEqual(self.port.commands, [])

    def test_flaky_backup_read_writes_nothing(self):
        state = {"n": 0}

        def corrupt(page, addr, v):
            if page == 0 and addr == 0x8000:
                state["n"] += 1
                if state["n"] == 2:        # second pass of the backup read differs
                    return v ^ 0x10
            return v
        self.tag.read_corrupt = corrupt
        r = self.run_job()
        self.assertFalse(r.ok)
        self.assertEqual(r.step, "backup")
        self.assertEqual(r.backup, "failed")
        self.assertEqual(bytes(self.tag.flash), self.stock_flash)
        self.assertEqual(bytes(self.tag.info), self.stock_info)
        self.assertEqual(self.tag.writes, [0, 0])
        self.assertNotIn(zp.CMD_ERASE_FLASH, self.port.commands)
        self.assertEqual(os.listdir(self.backup) if os.path.isdir(self.backup) else [], [])

    def test_unwritable_backup_dir_writes_nothing(self):
        blocker = os.path.join(self.dir, "file")
        open(blocker, "w").close()
        r = wf.flash_tag(self.fl, "29", self.fw_path, backup_dir=os.path.join(blocker, "sub"),
                         log=self.lines.append)
        self.assertEqual(r.step, "backup")
        self.assertEqual(self.tag.writes, [0, 0])
        self.assertEqual(self.tag.erase_count, [0, 0])

    def test_verify_failure(self):
        # A bit that reads wrong only during the final read-back (the flasher's own
        # per-byte check passed), so only our verify can catch it.
        self._verifying = False
        self.tag.read_corrupt = (lambda page, addr, v:
                                 v ^ 1 if self._verifying and page == 0 and addr == 0x0200 else v)
        orig_read = self.fl.read

        def read(addr, length, progress=None):
            self._verifying = (addr == 0 and length == len(self.fw))
            try:
                return orig_read(addr, length, progress)
            finally:
                self._verifying = False
        self.fl.read = read
        r = self.run_job()
        self.assertFalse(r.ok)
        self.assertEqual(r.step, "verify")
        self.assertIn("0x0200", r.message)

    def test_write_failure_reported(self):
        self.tag.stuck_bits[(0, 0x0010)] = 0xFF
        r = self.run_job()
        self.assertFalse(r.ok)
        self.assertEqual(r.step, "serial")
        self.assertIn("write failed", r.message)
        self.assertEqual(r.backup, "saved")       # backup happened before

    def test_wrong_model(self):
        self.run_job("29")
        r = self.run_job("154")
        self.assertFalse(r.ok)
        self.assertEqual(r.step, "model")
        self.assertEqual(self.tag.erase_count, [1, 0])     # only the first job erased

    def test_no_mac_anywhere(self):
        self.tag.flash[0xFC06:0xFC0C] = b"\xff" * 6
        r = self.run_job()
        self.assertEqual(r.step, "mac")
        self.assertEqual(self.tag.erase_count, [0, 0])
        self.assertEqual(self.tag.writes, [0, 0])

    def test_no_mac_write_option(self):
        r = self.run_job(write_mac=False)
        self.assertTrue(r.ok, r.message)
        self.assertEqual(bytes(self.tag.info), self.stock_info)
        self.assertTrue(any("WARNING" in s for s in self.lines))

    def test_csv_log(self):
        r = self.run_job()
        wf.append_log(self.backup, r.log_row())
        self.port.tag = None
        r2 = self.run_job()
        wf.append_log(self.backup, r2.log_row())
        with open(os.path.join(self.backup, wf.LOG_NAME)) as f:
            rows = list(csv.DictReader(f))
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["mac"], wf.mac_str(r.mac))
        self.assertEqual(rows[0]["result"], "OK")
        self.assertEqual(rows[0]["backup"], "saved")
        self.assertEqual(rows[1]["result"], "FAILED")
        self.assertTrue(rows[1]["message"].startswith("contact"))

    def test_progress_reported(self):
        seen = []
        self.run_job(progress=lambda label, d, t: seen.append(label))
        self.assertEqual(set(seen), {"Backup", "Write", "Verify"})


class SettingsTest(unittest.TestCase):
    def test_roundtrip_and_find(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "x", "flash-tui.json")
            cfg = settings.load(path)
            self.assertEqual(cfg["model"], "29")
            self.assertTrue(cfg["firmware"]["154"].endswith(os.path.join("fw154", "openesl.bin")))
            cfg["port_location"] = "1-2:1.0"
            cfg["model"] = "154"
            settings.save(cfg, path)
            cfg2 = settings.load(path)
            self.assertEqual(cfg2["model"], "154")
            ports = [{"device": "/dev/ttyUSB0", "location": "1-1:1.0"},
                     {"device": "/dev/ttyUSB1", "location": "1-2:1.0"}]
            self.assertEqual(settings.find_remembered(cfg2, ports)["device"], "/dev/ttyUSB1")
            self.assertIsNone(settings.find_remembered(cfg2, ports[:1]))

    def test_bad_file_gives_defaults(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "c.json")
            with open(path, "w") as f:
                f.write("{not json")
            self.assertEqual(settings.load(path)["model"], "29")


if __name__ == "__main__":
    unittest.main()
