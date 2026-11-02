#!/usr/bin/env python3
"""
The per-tag flashing job, without any UI: MAC -> backup -> MAC to info page -> firmware -> verify.

Order and rules:

  1. ZBS_BEGIN. No answer from the tag -> stop.
  2. Read the info page twice; the two reads must match (bad pogo contact gives noise).
  3. MAC: if info page 0x10-0x17 holds a MAC (LSB first), use it. A known model code in
     it (0x3B = 2.9", 0x34 = 1.54") must match the chosen model. If the bytes are blank
     (all 0xFF), the tag still has its stock firmware: the MAC is made from flash
     0xFC06-0xFC0B the same way as ZBS_Flasher's "mac" command (00 00 + 6 bytes, with
     byte 6/7 set to the model code and the low nibble of byte 7 a XOR checksum).
     The same 6 bytes as hex are the barcode printed on the label (see barcode_from_stock).
     If both are blank (an earlier build was written with a plain erase, which wiped
     0xFC06), the MAC can be typed in (manual_mac): the barcode from the label or the
     whole MAC, see mac_from_entry. It is then written to the info page as in step 5.
  4. Backup: if backup/<MAC>.bin does not exist yet, read the whole 64 KB main flash
     (twice, must match) and save it as <MAC>.bin plus the info page as <MAC>.info.bin.
     Any failure here stops the job before anything is written.
  5. If the MAC came from flash, program it into the blank info page bytes 0x10-0x17
     (no erase: only bytes that are 0xFF are written) and read it back. This is done
     before erasing the main flash, so the MAC is never only in the backup file.
  6. Erase the main flash, write the firmware, read it back and compare.
  7. Power-cycle the tag so it starts the new firmware.

The info page is never erased.
"""
import csv
import datetime
import hashlib
import os
import re
import sys
import time

import zbs_proto as zp

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

sys.path.insert(0, os.path.join(ROOT, "shared"))
import models  # noqa: E402  (the model registry, shared/models.json)

# By build ("29", "154"): name, size, the MAC code in byte 6 and the firmware tag/tools/build.py makes
MODELS = {
    m["build"]: {"name": models.label(m), "size": "%dx%d" % (m["width"], m["height"]), "mac_code": m["mac_code"],
                 "firmware": os.path.join(ROOT, "tag", "build", models.fw_dir(m), "openesl.bin")}
    for m in models.load()
}
MAC_CODE_TO_MODEL = {m["mac_code"]: k for k, m in MODELS.items()}

CODE_LIMIT = 0xF400                 # tag/tools/build.py CODE_SIZE: free page 0xF400, settings record 0xF800, stock MAC 0xFC00
STOCK_MAC_ADDR = 0xFC06             # 6 bytes in stock firmware (kildelogg 2026-10-10)
INFO_MAC_ADDR = 0x10                # 8 bytes, LSB first (kildelogg 2026-10-14)
DEFAULT_BACKUP_DIR = os.path.join(ROOT, "backup")
LOG_NAME = "flash_log.csv"
LOG_FIELDS = ["time", "mac", "model", "mac_source", "backup", "firmware", "fw_sha256",
              "result", "message", "seconds", "barcode"]


class JobError(Exception):
    """A step failed. `step` says which, for the log."""

    def __init__(self, step, msg):
        super().__init__(msg)
        self.step = step


# ---- MAC ----

def mac_str(mac8):
    return bytes(mac8).hex().upper()


def mac_from_info(info):
    """MAC (MSB first, 8 bytes) from the info page, or None if the bytes are blank."""
    raw = bytes(info[INFO_MAC_ADDR:INFO_MAC_ADDR + 8])
    if raw in (b"\xff" * 8, b"\x00" * 8):
        return None
    return raw[::-1]


def mac_from_stock(raw6, model):
    """OpenEPaperLink-style MAC from the 6 stock bytes at 0xFC06, as ZBS_Flasher's 'mac' command."""
    raw6 = bytes(raw6)
    if len(raw6) != 6 or raw6 in (b"\xff" * 6, b"\x00" * 6):
        return None
    mac = bytearray(b"\x00\x00" + raw6)
    mac[6] = MODELS[model]["mac_code"]
    mac[7] = 0x10
    chk = 0
    for b in mac[2:8]:
        chk ^= (b & 0x0F) ^ (b >> 4)
    mac[7] |= chk
    return bytes(mac)


def barcode_from_stock(raw6):
    """The barcode printed on the label: the 6 stock bytes at 0xFC06 as 12 hex digits.

    Seen on 0000020663463415: its label is Code 128 "020663461013", whose first 8 digits are MAC
    bytes 2-5 (= stock bytes 0-3). That the last 4 digits are stock bytes 4-5 is inferred, not yet
    checked against a backup. Those two bytes are not in the MAC, so they come from the stock
    flash (or its backup) alone. "" if the bytes are blank.
    """
    raw6 = bytes(raw6)
    if len(raw6) != 6 or raw6 in (b"\xff" * 6, b"\x00" * 6):
        return ""
    return raw6.hex().upper()


def barcode_from_backup(path, mac):
    """Barcode from a stored stock backup, if its 0xFC06 bytes give this MAC; else ""."""
    try:
        with open(path, "rb") as f:
            f.seek(STOCK_MAC_ADDR)
            raw = f.read(6)
    except OSError:
        return ""
    model = model_of_mac(mac)
    if not model or mac_from_stock(raw, model) != bytes(mac):
        return ""
    return barcode_from_stock(raw)


def mac_from_entry(text, model):
    """MAC typed in for a tag that has none: (mac8, barcode). Raises ValueError.

    Takes the 12 hex digits of the barcode on the label (the stock bytes, see
    barcode_from_stock) or the whole 16-digit MAC; spaces, ':' and '-' are ignored. A whole
    MAC must be one mac_from_stock could have made for this model (00 00 prefix, model code,
    checksum), which catches typos.
    """
    clean = re.sub(r"[\s:-]", "", text or "").upper()
    if not re.fullmatch(r"[0-9A-F]{12}|[0-9A-F]{16}", clean):
        raise ValueError("%r is neither the 12-digit barcode from the label nor a 16-digit MAC"
                         % text)
    raw = bytes.fromhex(clean)
    if len(raw) == 6:
        mac = mac_from_stock(raw, model)
        if mac is None:
            raise ValueError("barcode %s is blank" % clean)
        return mac, clean
    expect = mac_from_stock(raw[2:8], model)
    if expect != raw:
        raise ValueError("MAC %s does not fit a %s tag (from its bytes it would be %s)"
                         % (clean, MODELS[model]["name"], mac_str(expect) if expect else "none"))
    return raw, ""


def model_of_mac(mac8):
    return MAC_CODE_TO_MODEL.get(mac8[6])


# ---- firmware ----

def load_firmware(path):
    """Read and check a firmware image. Raises JobError with a clear message."""
    if not path:
        raise JobError("firmware", "no firmware file chosen")
    if not os.path.isfile(path):
        raise JobError("firmware", "firmware file not found: %s (build it with "
                       "'python tag/tools/build.py fw', MODEL=154 for the 1.54\")" % path)
    with open(path, "rb") as f:
        data = f.read()
    if not data:
        raise JobError("firmware", "firmware file is empty: %s" % path)
    if len(data) > CODE_LIMIT:
        raise JobError("firmware", "firmware is %d bytes, over the 0x%04X limit (the tag's settings store)"
                       % (len(data), CODE_LIMIT))
    if data.count(0xFF) == len(data):
        raise JobError("firmware", "firmware file is only 0xFF: %s" % path)
    return data


# ---- backups ----

def backup_paths(backup_dir, mac):
    m = mac_str(mac)
    return os.path.join(backup_dir, m + ".bin"), os.path.join(backup_dir, m + ".info.bin")


def find_backup(backup_dir, mac):
    """Existing backup of the main flash for this MAC, or None.

    Also accepts the older layout from ZBS_Flasher/flash_loop.py: <dir>/<MAC>/flash.bin.
    """
    flash, _ = backup_paths(backup_dir, mac)
    old = os.path.join(backup_dir, mac_str(mac), "flash.bin")
    for p in (flash, old):
        if os.path.isfile(p) and os.path.getsize(p) == zp.FLASH_SIZE:
            return p
    return None


def _write_atomic(path, data):
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    with open(path, "rb") as f:
        if f.read() != data:
            raise OSError("read-back of %s does not match" % path)


def save_backup(backup_dir, mac, flash, info):
    os.makedirs(backup_dir, exist_ok=True)
    fpath, ipath = backup_paths(backup_dir, mac)
    if os.path.exists(fpath):
        raise OSError("refusing to overwrite existing backup %s" % fpath)
    if not os.path.exists(ipath):
        _write_atomic(ipath, info)
    _write_atomic(fpath, flash)
    return fpath


# ---- CSV log ----

def _upgrade_log(path):
    """A log written before a column was added gets the new header (and empty cells)."""
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
        f.seek(0)
        header = next(csv.reader(f), [])
    if header == LOG_FIELDS:
        return
    tmp = path + ".part"
    with open(tmp, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=LOG_FIELDS, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k) or "" for k in LOG_FIELDS})
    os.replace(tmp, path)


def append_log(backup_dir, row):
    os.makedirs(backup_dir, exist_ok=True)
    path = os.path.join(backup_dir, LOG_NAME)
    new = not os.path.exists(path) or os.path.getsize(path) == 0
    if not new:
        _upgrade_log(path)
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=LOG_FIELDS)
        if new:
            w.writeheader()
        w.writerow({k: row.get(k, "") for k in LOG_FIELDS})
    return path


# ---- the job ----

class TagResult:
    def __init__(self, model, firmware):
        self.time = datetime.datetime.now()
        self.mac = None
        self.model = model
        self.mac_source = ""        # "info page" or "stock flash"
        self.barcode = ""           # printed on the label, from the stock bytes (or their backup)
        self.backup = ""            # "saved", "exists", "failed", ""
        self.backup_path = ""
        self.firmware = firmware
        self.fw_sha256 = ""
        self.ok = False
        self.step = ""
        self.message = ""
        self.seconds = 0.0

    @property
    def result(self):
        return "OK" if self.ok else "FAILED (%s)" % self.step

    def log_row(self):
        return {
            "time": self.time.isoformat(timespec="seconds"),
            "mac": mac_str(self.mac) if self.mac else "",
            "model": self.model,
            "mac_source": self.mac_source,
            "backup": self.backup,
            "firmware": os.path.basename(self.firmware or ""),
            "fw_sha256": self.fw_sha256[:16],
            "result": "OK" if self.ok else "FAILED",
            "message": self.message if self.ok else "%s: %s" % (self.step, self.message),
            "seconds": "%.1f" % self.seconds,
            "barcode": self.barcode,
        }


def _read_twice(fl, page, addr, length, what, progress=None):
    fl.select_page(page)
    a = fl.read(addr, length, progress and (lambda d, t: progress(d, 2 * t)))
    b = fl.read(addr, length, progress and (lambda d, t: progress(t + d, 2 * t)))
    if a != b:
        diff = next(i for i in range(length) if a[i] != b[i])
        raise JobError("read", "two reads of the %s differ (first at 0x%04X): bad contact?"
                       % (what, addr + diff))
    return a


def flash_tag(fl, model, firmware_path, backup_dir=DEFAULT_BACKUP_DIR, write_mac=True,
              log=print, progress=None, reset_after=True, manual_mac=None):
    """Run the whole job on the tag in the jig. Never raises for tag problems: returns TagResult.

    log(text) gets one line per step; progress(label, done, total) drives a progress bar.
    manual_mac (barcode or MAC, see mac_from_entry) is used only if the tag has no MAC of its
    own; res.mac_source is then "typed in".
    """
    res = TagResult(model, firmware_path)
    t0 = time.monotonic()

    def prog(label):
        if not progress:
            return None
        return lambda d, t: progress(label, d, t)

    touched = False
    try:
        if model not in MODELS:
            raise JobError("setup", "unknown model %r" % model)
        fw = load_firmware(firmware_path)
        typed = None
        if manual_mac:
            try:
                typed = mac_from_entry(manual_mac, model)
            except ValueError as e:
                raise JobError("setup", str(e))
        res.fw_sha256 = hashlib.sha256(fw).hexdigest()
        log("Firmware %s: %d bytes" % (os.path.basename(firmware_path), len(fw)))

        try:
            fl.begin()
        except zp.NoTagError as e:
            raise JobError("contact", str(e))
        touched = True

        info = _read_twice(fl, zp.PAGE_INFO, 0, zp.INFO_SIZE, "info page")
        mac = mac_from_info(info)
        need_mac_write = False
        raw = None                  # stock bytes, when the MAC came from them
        if mac is not None:
            res.mac_source = "info page"
            tag_model = model_of_mac(mac)
            if tag_model and tag_model != model:
                res.mac = mac
                raise JobError("model", "MAC %s says %s, but the batch is %s. Wrong tag or "
                               "wrong model chosen" % (mac_str(mac), MODELS[tag_model]["name"],
                                                       MODELS[model]["name"]))
        else:
            if info[INFO_MAC_ADDR:INFO_MAC_ADDR + 8] != b"\xff" * 8:
                raise JobError("mac", "info page MAC bytes are all 0x00; not touching this tag")
            stock = _read_twice(fl, zp.PAGE_MAIN, STOCK_MAC_ADDR, 6, "stock MAC")
            mac = mac_from_stock(stock, model)
            if mac is not None:
                raw = stock
                res.mac_source = "stock flash"
                res.barcode = barcode_from_stock(raw)
            elif typed:
                mac, res.barcode = typed
                res.mac_source = "typed in"
            else:
                raise JobError("mac", "no MAC in the info page and none at 0x%04X (%s). Not stock "
                               "firmware and no MAC written? Type in the barcode from the label "
                               "or the MAC" % (STOCK_MAC_ADDR, stock.hex(" ")))
            need_mac_write = True
        if typed and res.mac_source != "typed in":
            log("The tag has its own MAC; the typed one (%s) is not used" % manual_mac)
        res.mac = mac
        log("MAC %s (from %s)" % (mac_str(mac), res.mac_source))
        if res.barcode:
            log("Barcode %s" % res.barcode)

        existing = find_backup(backup_dir, mac)
        if existing:
            res.backup, res.backup_path = "exists", existing
            log("Backup already exists: %s" % existing)
            if not res.barcode:
                res.barcode = barcode_from_backup(existing, mac)
                if res.barcode:
                    log("Barcode %s (from the backup)" % res.barcode)
        else:
            log("Reading the whole flash for the backup (two passes) ...")
            try:
                flash = _read_twice(fl, zp.PAGE_MAIN, 0, zp.FLASH_SIZE, "main flash",
                                    prog("Backup"))
                if raw is not None and flash[STOCK_MAC_ADDR:STOCK_MAC_ADDR + 6] != raw:
                    raise JobError("backup", "backup does not contain the MAC bytes read before")
                blank = flash.count(0xFF) == len(flash)
                # A tag with its stock MAC never reads blank, so all 0xFF there means a bad
                # read. A tag whose MAC had to be typed in has lost its stock firmware already;
                # all 0xFF there is an empty flash (an earlier erase), with nothing to keep.
                if blank and res.mac_source != "typed in":
                    raise JobError("backup", "the flash reads as all 0xFF; refusing to call that a backup")
                if not blank:
                    res.backup_path = save_backup(backup_dir, mac, flash, info)
            except (JobError, zp.FlasherError, OSError) as e:
                res.backup = "failed"
                raise JobError("backup", "backup failed, nothing written: %s" % e)
            if blank:
                res.backup = "blank"
                log("The flash is empty (all 0xFF): nothing to back up")
            else:
                res.backup = "saved"
                log("Backup saved: %s" % res.backup_path)

        if need_mac_write:
            if not write_mac:
                log("WARNING: MAC not written to the info page (--no-mac-write); "
                    "the new firmware will not find it")
            else:
                fl.select_page(zp.PAGE_INFO)
                if fl.read(INFO_MAC_ADDR, 8) != b"\xff" * 8:
                    raise JobError("mac", "info page MAC bytes are no longer blank; not writing")
                fl.write_chunk(INFO_MAC_ADDR, mac[::-1])
                back = fl.read(INFO_MAC_ADDR, 8)
                if back != mac[::-1]:
                    raise JobError("mac", "MAC read back from the info page is %s" % back.hex(" "))
                log("MAC written to the info page 0x10-0x17")

        log("Erasing main flash ...")
        fl.erase_main()
        log("Writing firmware ...")
        fl.write(0, fw, prog("Write"))
        log("Verifying ...")
        fl.select_page(zp.PAGE_MAIN)
        back = fl.read(0, len(fw), prog("Verify"))
        if back != fw:
            diff = next(i for i in range(len(fw)) if back[i] != fw[i])
            raise JobError("verify", "verify failed at 0x%04X (wrote %02X, read %02X)"
                           % (diff, fw[diff], back[diff]))
        res.ok = True
        res.message = "flashed %d bytes, verified" % len(fw)
        log("OK: %s" % res.message)
    except JobError as e:
        res.step, res.message = e.step, str(e)
    except zp.FlasherError as e:
        res.step, res.message = "serial", str(e)
    except Exception as e:      # keep the batch going; the log shows what happened
        res.step, res.message = "internal", "%s: %s" % (type(e).__name__, e)
    finally:
        if touched and reset_after:
            try:
                fl.reset()
            except zp.FlasherError:
                pass
        res.seconds = time.monotonic() - t0
    if not res.ok:
        log("FAILED (%s): %s" % (res.step, res.message))
    return res
