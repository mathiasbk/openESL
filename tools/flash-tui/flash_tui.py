#!/usr/bin/env python3
"""
Terminal UI for flashing many SoluM ZBS243 tags in a row with a ZBS_Flasher D1 mini and a
pogo-pin jig.

  python tools/flash-tui/flash_tui.py                 normal use
  python tools/flash-tui/flash_tui.py --fake          simulated flasher and tag, no hardware
  python tools/flash-tui/flash_tui.py --no-mac-write  never program the MAC into the info page
  python tools/flash-tui/flash_tui.py --mac 020793500102
                                       a tag with no MAC left: the barcode from its label (or the
                                       whole MAC), used for the next such tag

Keys: space/enter flash the tag in the jig, m switch model (2.9" / 1.54"), f firmware file
for the model, p choose serial port, a auto-detect on/off, q quit.

Per tag: MAC -> backup of the stock flash (if none yet) -> MAC to info page (stock tags)
-> erase + write firmware -> verify. See workflow.py and README.md.
"""
import argparse
import os
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from textual import on  # noqa: E402
from textual.app import App, ComposeResult  # noqa: E402
from textual.binding import Binding  # noqa: E402
from textual.containers import Horizontal, Vertical  # noqa: E402
from textual.screen import ModalScreen  # noqa: E402
from textual.widgets import (DataTable, Footer, Header, Input, Label, OptionList,  # noqa: E402
                             ProgressBar, RichLog, Static)
from textual.widgets.option_list import Option  # noqa: E402

import settings  # noqa: E402
import workflow  # noqa: E402
import zbs_proto as zp  # noqa: E402


def port_label(p):
    loc = p["location"] or "?"
    ids = "%04x:%04x" % (p["vid"], p["pid"]) if p["vid"] is not None else "----:----"
    tag = "  CH340" if p["ch340"] else ""
    return "%-14s usb %-12s %s  %s%s" % (p["device"], loc, ids, p["description"], tag)


class PortScreen(ModalScreen):
    """Pick the flasher's serial port. Returns the port dict, or None."""

    BINDINGS = [Binding("escape", "cancel", "Cancel"), Binding("r", "refresh", "Refresh")]

    def __init__(self, lister, remembered=None, message=""):
        super().__init__()
        self.lister = lister
        self.remembered = remembered
        self.message = message
        self.ports = []

    def compose(self):
        with Vertical(id="dialog"):
            yield Label("Choose the ZBS_Flasher port and press Enter (r = refresh, Esc = cancel).\n"
                        "The debug Wemos is also a CH340: tell them apart by the USB path.\n"
                        "The port is checked with a version request before anything is done.")
            if self.message:
                yield Label(self.message, id="port-msg")
            yield OptionList(id="ports")

    def on_mount(self):
        self.action_refresh()

    def action_refresh(self):
        self.ports = self.lister()
        ol = self.query_one(OptionList)
        ol.clear_options()
        if not self.ports:
            ol.add_option(Option("no serial ports found", disabled=True))
            return
        for i, p in enumerate(self.ports):
            mark = "  (last used)" if self.remembered and p["location"] == self.remembered else ""
            ol.add_option(Option(port_label(p) + mark, id=str(i)))
        ol.highlighted = 0
        ol.focus()

    @on(OptionList.OptionSelected)
    def picked(self, event):
        self.dismiss(self.ports[int(event.option.id)])

    def action_cancel(self):
        self.dismiss(None)


class FirmwareScreen(ModalScreen):
    """Edit the firmware path for one model. Returns the path, or None."""

    BINDINGS = [Binding("escape", "cancel", "Cancel")]

    def __init__(self, model, path):
        super().__init__()
        self.model = model
        self.path = path

    def compose(self):
        with Vertical(id="dialog"):
            yield Label("Firmware file for %s (Enter to save, Esc to cancel). Default: %s"
                        % (workflow.MODELS[self.model]["name"],
                           os.path.relpath(workflow.MODELS[self.model]["firmware"])))
            yield Input(value=self.path, id="fw-path")

    @on(Input.Submitted)
    def submitted(self, event):
        self.dismiss(os.path.abspath(os.path.expanduser(event.value.strip())) if event.value.strip() else None)

    def action_cancel(self):
        self.dismiss(None)


class FlashApp(App):
    TITLE = "OpenESL tag flasher"
    CSS = """
    #status { height: auto; padding: 0 1; border: round $primary; }
    #tags { height: 1fr; min-height: 6; }
    #log { height: 12; border: round $secondary; }
    #progress-row { height: 1; padding: 0 1; }
    #progress-label { width: 10; }
    #dialog { width: 90%; height: auto; max-height: 80%; padding: 1 2; border: thick $primary;
              background: $surface; }
    #ports { height: auto; max-height: 15; }
    #port-msg { color: $error; }
    PortScreen, FirmwareScreen { align: center middle; }
    """
    BINDINGS = [
        Binding("space", "flash", "Flash tag"),
        Binding("enter", "flash", "Flash tag", show=False),
        Binding("m", "model", "Model"),
        Binding("f", "firmware", "Firmware"),
        Binding("p", "port", "Port"),
        Binding("a", "auto", "Auto-detect"),
        Binding("q", "quit", "Quit"),
    ]

    def __init__(self, cfg, cfg_path=None, lister=zp.list_ports, opener=zp.open_port,
                 write_mac=True, probe_wait=3.0, manual_mac=None):
        super().__init__()
        self.cfg = cfg
        self.cfg_path = cfg_path
        self.lister = lister
        self.opener = opener
        self.write_mac = write_mac
        self.manual_mac = manual_mac  # --mac: for the next tag without a MAC of its own, then dropped
        self.probe_wait = probe_wait
        self.port = None            # open serial object
        self.port_info = None       # dict from list_ports
        self.flasher = None
        self.version = None
        self.lock = threading.Lock()
        self.busy = False
        self.auto = False
        self.armed = True           # auto-detect: wait for the tag to leave before the next one
        self.empty_polls = 0
        self.results = []

    # ---- layout ----

    def compose(self) -> ComposeResult:
        yield Header()
        yield Static(id="status")
        yield DataTable(id="tags", cursor_type="row", zebra_stripes=True)
        with Horizontal(id="progress-row"):
            yield Label("", id="progress-label")
            yield ProgressBar(id="progress", total=100, show_eta=False)
        yield RichLog(id="log", wrap=True, markup=True)
        yield Footer()

    def on_mount(self):
        t = self.query_one(DataTable)
        t.add_columns("#", "Time", "MAC", "Barcode", "Model", "MAC from", "Backup", "Result", "s")
        self.update_status()
        self.set_interval(1.0, self.auto_tick)
        self.say("Backups and log go to %s" % self.cfg["backup_dir"])
        ports = self.lister()
        p = settings.find_remembered(self.cfg, ports)
        if p:
            self.say("Remembered flasher port %s (usb %s), checking it ..." % (p["device"], p["location"]))
            self.connect(p)
        else:
            if self.cfg.get("port_location"):
                self.say("[yellow]The remembered flasher (usb %s) is not plugged in.[/]"
                         % self.cfg["port_location"])
            self.action_port()

    # ---- helpers ----

    def say(self, text):
        self.query_one(RichLog).write(text)

    def say_from_thread(self, text):
        self.call_from_thread(self.say, text)

    def fw_path(self, model=None):
        return self.cfg["firmware"][model or self.cfg["model"]]

    def save_cfg(self):
        try:
            settings.save(self.cfg, self.cfg_path)
        except OSError as e:
            self.say("[red]Could not save settings: %s[/]" % e)

    def update_status(self):
        m = self.cfg["model"]
        model = workflow.MODELS[m]
        fw = self.fw_path()
        if os.path.isfile(fw):
            fw_state = "[green]%d bytes[/]" % os.path.getsize(fw)
        else:
            fw_state = "[red]MISSING[/] (build: MODEL=%s python tag/tools/build.py fw)" % m
        if self.flasher:
            port = "[green]%s[/] usb %s, ZBS_Flasher v0x%X" % (
                self.port_info["device"], self.port_info["location"] or "?", self.version)
        else:
            port = "[red]not connected[/] (p to choose)"
        ok = sum(1 for r in self.results if r.ok)
        lines = [
            "Port:      %s" % port,
            "Model:     [b]%s[/] %s (m to switch)" % (model["name"], model["size"]),
            "Firmware:  %s  %s" % (fw, fw_state),
            "Auto:      %s    Tags this session: %d, %d OK, %d failed%s" % (
                "[green]ON[/] (flashes when a tag answers)" if self.auto else "off",
                len(self.results), ok, len(self.results) - ok,
                "    [yellow]BUSY[/]" if self.busy else ""),
        ]
        if not self.write_mac:
            lines.append("[yellow]--no-mac-write: stock MACs are not copied to the info page[/]")
        if self.manual_mac:
            lines.append("[yellow]--mac %s: for the next tag that has no MAC of its own[/]"
                         % self.manual_mac)
        self.query_one("#status", Static).update("\n".join(lines))

    def set_progress(self, label, done, total):
        self.query_one("#progress-label", Label).update(label)
        bar = self.query_one(ProgressBar)
        bar.update(total=max(total, 1), progress=done)

    # ---- port ----

    def action_port(self):
        if self.busy:
            self.say("[yellow]Busy, wait for the current tag.[/]")
            return
        self.push_screen(PortScreen(self.lister, self.cfg.get("port_location")), self.port_chosen)

    def port_chosen(self, p):
        if p:
            self.connect(p)

    def disconnect(self):
        if self.port is not None:
            try:
                self.port.close()
            except Exception:
                pass
        self.port = self.flasher = self.version = None
        self.port_info = None

    def connect(self, p):
        self.disconnect()
        self.busy = True
        self.update_status()
        self.run_worker(lambda: self._connect_thread(p), thread=True, group="serial")

    def _connect_thread(self, p):
        port = None
        try:
            with self.lock:
                port = self.opener(p["device"])
                fl = zp.ZbsFlasher(port)
                version = zp.probe(fl, wait_s=self.probe_wait)
        except (zp.FlasherError, OSError, ValueError) as e:
            if port is not None:
                try:
                    port.close()
                except Exception:
                    pass
            self.call_from_thread(self._connect_failed, p, str(e))
            return
        self.call_from_thread(self._connected, p, port, fl, version)

    def _connected(self, p, port, fl, version):
        self.port, self.port_info, self.flasher, self.version = port, p, fl, version
        self.busy = False
        self.cfg["port_location"] = p["location"]
        self.cfg["port_device"] = p["device"]
        self.save_cfg()
        self.say("[green]Connected to ZBS_Flasher v0x%X on %s (usb %s).[/] Put a tag in the jig "
                 "and press space." % (version, p["device"], p["location"] or "?"))
        self.update_status()

    def _connect_failed(self, p, msg):
        self.busy = False
        self.say("[red]%s: %s[/]" % (p["device"], msg))
        self.update_status()
        self.push_screen(PortScreen(self.lister, self.cfg.get("port_location"),
                                    "%s did not answer as a ZBS_Flasher. Pick another port."
                                    % p["device"]), self.port_chosen)

    # ---- settings ----

    def action_model(self):
        if self.busy:
            return
        self.cfg["model"] = "154" if self.cfg["model"] == "29" else "29"
        self.save_cfg()
        self.say("Model: %s" % workflow.MODELS[self.cfg["model"]]["name"])
        self.update_status()

    def action_firmware(self):
        if self.busy:
            return
        m = self.cfg["model"]

        def done(path):
            if path:
                self.cfg["firmware"][m] = path
                self.save_cfg()
                if not os.path.isfile(path):
                    self.say("[yellow]Note: %s does not exist (yet).[/]" % path)
            self.update_status()
        self.push_screen(FirmwareScreen(m, self.fw_path(m)), done)

    def action_auto(self):
        self.auto = not self.auto
        self.armed = True
        self.empty_polls = 0
        self.say("Auto-detect %s" % ("on: a tag is flashed as soon as it answers" if self.auto else "off"))
        self.update_status()

    # ---- flashing ----

    def can_flash(self, quiet=False):
        if self.busy:
            return False
        if not self.flasher:
            if not quiet:
                self.say("[red]No flasher connected. Press p to choose the port.[/]")
            return False
        try:
            workflow.load_firmware(self.fw_path())
        except workflow.JobError as e:
            if not quiet:
                self.say("[red]%s[/]" % e)
            return False
        return True

    def action_flash(self):
        if self.can_flash():
            self.start_job()

    def start_job(self):
        self.busy = True
        self.armed = False
        self.update_status()
        model, fw = self.cfg["model"], self.fw_path()
        self.say("[b]--- Tag %d: %s, %s ---[/]" % (len(self.results) + 1,
                                                   workflow.MODELS[model]["name"], os.path.basename(fw)))
        self.run_worker(lambda: self._job_thread(model, fw), thread=True, group="serial")

    def _job_thread(self, model, fw):
        with self.lock:
            res = workflow.flash_tag(
                self.flasher, model, fw, backup_dir=self.cfg["backup_dir"], write_mac=self.write_mac,
                manual_mac=self.manual_mac, log=self.say_from_thread,
                progress=lambda label, d, t: self.call_from_thread(self.set_progress, label, d, t))
            try:
                workflow.append_log(self.cfg["backup_dir"], res.log_row())
            except OSError as e:
                self.say_from_thread("[red]Could not write the CSV log: %s[/]" % e)
        self.call_from_thread(self._job_done, res)

    def _job_done(self, res):
        self.busy = False
        if res.mac_source == "typed in":
            self.manual_mac = None
        self.results.append(res)
        t = self.query_one(DataTable)
        result = "[green]OK[/]" if res.ok else "[red]%s[/]" % res.result
        backup = {"saved": "[green]saved[/]", "exists": "exists",
                  "failed": "[red]failed[/]"}.get(res.backup, "-")
        t.add_row(str(len(self.results)), res.time.strftime("%H:%M:%S"),
                  workflow.mac_str(res.mac) if res.mac else "-", res.barcode or "-",
                  workflow.MODELS[res.model]["name"], res.mac_source or "-", backup, result,
                  "%.0f" % res.seconds)
        t.move_cursor(row=t.row_count - 1)
        if res.ok:
            self.say("[green b]DONE %s[/] - take the tag out and put in the next one."
                     % workflow.mac_str(res.mac))
        else:
            self.say("[red b]FAILED[/] [red]%s[/]" % res.message)
            if self.auto:
                self.say("Auto-detect waits until the tag is taken out (or press space to retry).")
        self.set_progress("", 0, 1)
        self.empty_polls = 0
        self.update_status()

    # ---- auto-detect ----

    def auto_tick(self):
        if not self.auto or self.busy or not self.flasher:
            return
        if self.armed and not self.can_flash(quiet=True):
            return
        self.busy = True
        self.run_worker(self._poll_thread, thread=True, group="serial")

    def _poll_thread(self):
        try:
            with self.lock:
                present = self.flasher.tag_present()
        except zp.FlasherError as e:
            self.call_from_thread(self._poll_done, None, str(e))
            return
        self.call_from_thread(self._poll_done, present, "")

    def _poll_done(self, present, err):
        self.busy = False
        if present is None:
            self.say("[red]Flasher stopped answering: %s[/]" % err)
            self.auto = False
        elif present and self.armed:
            self.say("Tag detected.")
            self.start_job()
            return
        elif not present:
            self.empty_polls += 1
            if not self.armed and self.empty_polls >= 2:
                self.armed = True
                self.say("Jig empty, ready for the next tag.")
        self.update_status()

    def on_unmount(self):
        self.disconnect()


def fake_setup(model="29"):
    """Lister and opener for --fake: one simulated flasher and one simulated debug Wemos."""
    import fake_flasher

    tags = iter(fake_flasher.stock_tag(model, seed=i) for i in range(1, 10000))
    ports = [
        {"device": "/fake/ttyUSB0", "location": "fake-1", "vid": 0x1A86, "pid": 0x7523,
         "serial": "", "description": "simulated debug Wemos", "ch340": True},
        {"device": "/fake/ttyUSB1", "location": "fake-2", "vid": 0x1A86, "pid": 0x7523,
         "serial": "", "description": "simulated ZBS_Flasher", "ch340": True},
    ]

    class Opener:
        last = None

        def __call__(self, device):
            if device == "/fake/ttyUSB0":
                return fake_flasher.FakeDebugWemosPort()
            port = fake_flasher.FakeFlasherPort(next(tags))
            Opener.last = port
            return port

    return (lambda: [dict(p) for p in ports]), Opener()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fake", action="store_true", help="simulated flasher and tag (no hardware)")
    ap.add_argument("--no-mac-write", action="store_true",
                    help="do not program the stock MAC into the blank info page")
    ap.add_argument("--mac", metavar="BARCODE_OR_MAC",
                    help="for a tag with no MAC in its info page or stock flash (an earlier build "
                         "wiped it): the 12-digit barcode from its label or its 16-digit MAC; "
                         "used for one tag")
    ap.add_argument("--config", help="settings file (default %s)" % settings.default_path())
    ap.add_argument("--backup-dir", help="backup folder (default %s)" % workflow.DEFAULT_BACKUP_DIR)
    args = ap.parse_args(argv)

    cfg_path = args.config
    if args.fake and not cfg_path:
        import tempfile
        cfg_path = os.path.join(tempfile.mkdtemp(prefix="flash-tui-fake-"), "flash-tui.json")
    cfg = settings.load(cfg_path)
    if args.backup_dir:
        cfg["backup_dir"] = os.path.abspath(args.backup_dir)
    elif args.fake:
        cfg["backup_dir"] = os.path.join(os.path.dirname(cfg_path), "backup")
    kw = {}
    if args.mac:
        try:
            workflow.mac_from_entry(args.mac, cfg["model"])
        except ValueError as e:
            ap.error("--mac: %s" % e)
        kw["manual_mac"] = args.mac
    if args.fake:
        kw["lister"], kw["opener"] = fake_setup(cfg["model"])
        kw["probe_wait"] = 0.3
    FlashApp(cfg, cfg_path, write_mac=not args.no_mac_write, **kw).run()


if __name__ == "__main__":
    main()
