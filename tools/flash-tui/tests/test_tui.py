"""Headless TUI tests with Textual's pilot, against the simulated flasher."""
import csv
import os
import tempfile
import unittest

import helpers  # noqa: F401
import fake_flasher as ff
import flash_tui
import settings
import workflow as wf
from textual.widgets import DataTable, OptionList

FLASHER = {"device": "/fake/ttyUSB1", "location": "1-2:1.0", "vid": 0x1A86, "pid": 0x7523,
           "serial": "", "description": "USB Serial", "ch340": True}
WEMOS = {"device": "/fake/ttyUSB0", "location": "1-1:1.0", "vid": 0x1A86, "pid": 0x7523,
         "serial": "", "description": "USB Serial", "ch340": True}


class Rig:
    """Two CH340 ports: a debug Wemos on ttyUSB0 and the flasher on ttyUSB1."""

    def __init__(self, tag):
        self.tag = tag
        self.opened = []
        self.flasher_port = None

    def lister(self):
        return [dict(WEMOS), dict(FLASHER)]

    def opener(self, device):
        self.opened.append(device)
        if device == WEMOS["device"]:
            return ff.FakeDebugWemosPort()
        if device == FLASHER["device"]:
            self.flasher_port = ff.FakeFlasherPort(self.tag)
            return self.flasher_port
        raise OSError("no such port %s" % device)


class TuiTest(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        self.cfg_path = os.path.join(d, "cfg", "flash-tui.json")
        self.cfg = settings.load(self.cfg_path)
        self.cfg["backup_dir"] = os.path.join(d, "backup")
        self.fw_path = os.path.join(d, "fw29.bin")
        with open(self.fw_path, "wb") as f:
            f.write(bytes((i * 13) & 0xFF for i in range(4000)))
        self.cfg["firmware"]["29"] = self.fw_path
        self.cfg["firmware"]["154"] = os.path.join(d, "missing154.bin")
        self.tag = ff.stock_tag("29", seed=4)
        self.rig = Rig(self.tag)

    def tearDown(self):
        self.tmp.cleanup()

    def app(self):
        return flash_tui.FlashApp(self.cfg, self.cfg_path, lister=self.rig.lister,
                                  opener=self.rig.opener, probe_wait=0.1)

    async def wait_idle(self, app, pilot):
        for _ in range(200):
            await pilot.pause(0.02)
            if not app.busy:
                await app.workers.wait_for_complete()
                await pilot.pause()
                if not app.busy:
                    return
        self.fail("app stayed busy")

    async def test_pick_wrong_port_then_right_one_then_flash(self):
        app = self.app()
        async with app.run_test(size=(140, 45)) as pilot:
            # no remembered port: the picker opens
            self.assertIsInstance(app.screen, flash_tui.PortScreen)
            await pilot.press("enter")                 # first entry = debug Wemos (wrong)
            await self.wait_idle(app, pilot)
            self.assertIsNone(app.flasher)
            self.assertIsInstance(app.screen, flash_tui.PortScreen)
            self.assertEqual(settings.load(self.cfg_path)["port_location"], "")
            await pilot.press("down", "enter")         # the flasher
            await self.wait_idle(app, pilot)
            self.assertIsNotNone(app.flasher)
            self.assertEqual(settings.load(self.cfg_path)["port_location"], "1-2:1.0")

            await pilot.press("space")
            await self.wait_idle(app, pilot)
            self.assertEqual(len(app.results), 1)
            r = app.results[0]
            self.assertTrue(r.ok, r.message)
            self.assertEqual(r.backup, "saved")
            self.assertEqual(app.query_one(DataTable).row_count, 1)
            with open(self.fw_path, "rb") as f:
                self.assertEqual(bytes(self.tag.flash[:4000]), f.read())
            with open(os.path.join(self.cfg["backup_dir"], wf.LOG_NAME)) as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(rows[0]["mac"], wf.mac_str(r.mac))

            # switch to 1.54": its firmware is missing -> nothing happens on the port
            await pilot.press("m")
            self.assertEqual(app.cfg["model"], "154")
            n = len(self.rig.flasher_port.commands)
            await pilot.press("space")
            await self.wait_idle(app, pilot)
            self.assertEqual(len(app.results), 1)
            self.assertEqual(len(self.rig.flasher_port.commands), n)

    async def test_remembered_port_by_usb_path(self):
        self.cfg["port_location"] = "1-2:1.0"
        self.cfg["port_device"] = "/dev/ttyUSB7"     # device name changed since last time
        app = self.app()
        async with app.run_test(size=(140, 45)) as pilot:
            await self.wait_idle(app, pilot)
            self.assertIsNotNone(app.flasher)
            self.assertEqual(self.rig.opened, [FLASHER["device"]])
            self.assertNotIsInstance(app.screen, flash_tui.PortScreen)

    async def test_auto_detect(self):
        self.cfg["port_location"] = "1-2:1.0"
        app = self.app()
        async with app.run_test(size=(140, 45)) as pilot:
            await self.wait_idle(app, pilot)
            await pilot.press("a")
            app.auto_tick()
            await self.wait_idle(app, pilot)
            self.assertEqual(len(app.results), 1)
            self.assertTrue(app.results[0].ok)
            # same tag still in the jig: not flashed again
            app.auto_tick()
            await self.wait_idle(app, pilot)
            self.assertEqual(len(app.results), 1)
            # take it out (two empty polls), put a new tag in
            self.rig.flasher_port.tag = None
            for _ in range(2):
                app.auto_tick()
                await self.wait_idle(app, pilot)
            self.assertTrue(app.armed)
            self.rig.flasher_port.tag = ff.stock_tag("29", seed=5)
            app.auto_tick()
            await self.wait_idle(app, pilot)
            self.assertEqual(len(app.results), 2)
            self.assertNotEqual(app.results[0].mac, app.results[1].mac)

    async def test_port_picker_lists_usb_paths(self):
        app = self.app()
        async with app.run_test(size=(140, 45)) as pilot:
            await pilot.pause()
            ol = app.screen.query_one(OptionList)
            labels = [str(ol.get_option_at_index(i).prompt) for i in range(ol.option_count)]
            self.assertIn("1-1:1.0", labels[0])
            self.assertIn("1-2:1.0", labels[1])
            await pilot.press("escape")
            self.assertEqual(self.rig.opened, [])


if __name__ == "__main__":
    unittest.main()
