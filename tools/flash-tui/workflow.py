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
import time

import zbs_proto as zp

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

MODELS = {
    "29": {"name": '2.9"', "size": "296x128", "mac_code": 0x3B,
           "firmware": os.path.join(ROOT, "tag", "build", "fw", "openesl.bin")},
    "154": {"name": '1.54"', "size": "200x200", "mac_code": 0x34,
            "firmware": os.path.join(ROOT, "tag", "build", "fw154", "openesl.bin")},
}
MAC_CODE_TO_MODEL = {m["mac_code"]: k for k, m in MODELS.items()}

CODE_LIMIT = 0xFC00                 # tag/tools/build.py CODE_SIZE: code stays below the stock MAC
STOCK_MAC_ADDR = 0xFC06             # 6 bytes in stock firmware (kildelogg 2026-10-10)
INFO_MAC_ADDR = 0x10                # 8 bytes, LSB first (kildelogg 2026-10-14)
DEFAULT_BACKUP_DIR = os.path.join(ROOT, "backup")
LOG_NAME = "flash_log.csv"
LOG_FIELDS = ["time", "mac", "model", "mac_source", "backup", "firmware", "fw_sha256",
              "result", "message", "seconds"]


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
        raise JobError("firmware", "firmware is %d bytes, over the 0x%04X limit (stock MAC area)"
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

def append_log(backup_dir, row):
    os.makedirs(backup_dir, exist_ok=True)
    path = os.path.join(backup_dir, LOG_NAME)
    new = not os.path.exists(path) or os.path.getsize(path) == 0
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
              log=print, progress=None, reset_after=True):
    """Run the whole job on the tag in the jig. Never raises for tag problems: returns TagResult.

    log(text) gets one line per step; progress(label, done, total) drives a progress bar.
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
            raw = _read_twice(fl, zp.PAGE_MAIN, STOCK_MAC_ADDR, 6, "stock MAC")
            mac = mac_from_stock(raw, model)
            if mac is None:
                raise JobError("mac", "no MAC in the info page and none at 0x%04X (%s). "
                               "Not stock firmware and no MAC written?" % (STOCK_MAC_ADDR, raw.hex(" ")))
            res.mac_source = "stock flash"
            need_mac_write = True
        res.mac = mac
        log("MAC %s (from %s)" % (mac_str(mac), res.mac_source))

        existing = find_backup(backup_dir, mac)
        if existing:
            res.backup, res.backup_path = "exists", existing
            log("Backup already exists: %s" % existing)
        else:
            log("Reading the whole flash for the backup (two passes) ...")
            try:
                flash = _read_twice(fl, zp.PAGE_MAIN, 0, zp.FLASH_SIZE, "main flash",
                                    prog("Backup"))
                if need_mac_write and flash[STOCK_MAC_ADDR:STOCK_MAC_ADDR + 6] != raw:
                    raise JobError("backup", "backup does not contain the MAC bytes read before")
                if flash.count(0xFF) == len(flash):
                    raise JobError("backup", "the flash reads as all 0xFF; refusing to call that a backup")
                res.backup_path = save_backup(backup_dir, mac, flash, info)
            except (JobError, zp.FlasherError, OSError) as e:
                res.backup = "failed"
                raise JobError("backup", "backup failed, nothing written: %s" % e)
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
