// tagflash.js - the per-tag flashing job for the browser. A port of tools/flash-tui/workflow.py
// with the same order and rules (see there):
//
//   1. ZBS_BEGIN; no answer from the tag -> stop.
//   2. Read the info page twice; the reads must match.
//   3. MAC from info page 0x10-0x17 (LSB first), whose model code must match the chosen model,
//      or, on a tag with stock firmware (blank bytes), from flash 0xFC06-0xFC0B. The same 6 bytes
//      as hex are the barcode printed on the label (barcodeFromStock). If both are blank (an
//      earlier build was written with a plain erase, which wiped 0xFC06), the MAC can be typed
//      in (manualMac): the barcode from the label or the whole MAC, see macFromEntry.
//   4. Backup: unless one is already stored for this MAC, read the whole 64 KB main flash
//      twice (must match) and store it with the info page. Any failure stops the job before
//      anything is written.
//   5. If the MAC came from flash, write it to the blank info page bytes and read it back.
//   6. Erase the main flash, write the firmware, read it back and compare.
//   7. Power-cycle the tag.
//
// The info page is never erased. Backups are kept in the browser (IndexedDB, see BackupStore)
// and can be downloaded from the page; the flash-tui's backup folder works the same way.

import * as zp from "./zbs.js";
import { MODELS as REGISTRY } from "./models.js";

export const MODELS = Object.fromEntries(
  Object.entries(REGISTRY).map(([n, m]) => [n, { name: n + '"', macCode: m.macCode, firmware: `firmware/tag-${m.build}.bin` }]),
);

// tag/tools/build.py CODE_SIZE: 0xF400 is a free page, the tag keeps its settings at 0xF800,
// and the stock MAC sits at 0xFC00
export const CODE_LIMIT = 0xf400;
export const STOCK_MAC_ADDR = 0xfc06;
export const INFO_MAC_ADDR = 0x10;

export class JobError extends Error {
  constructor(step, msg) {
    super(msg);
    this.step = step;
  }
}

export const macStr = (mac) => zp.toHex(mac);

export function macFromInfo(info) {
  const raw = info.slice(INFO_MAC_ADDR, INFO_MAC_ADDR + 8);
  if (raw.every((b) => b === 0xff) || raw.every((b) => b === 0)) return null;
  return raw.reverse();
}

export function macFromStock(raw6, model) {
  if (raw6.length !== 6 || raw6.every((b) => b === 0xff) || raw6.every((b) => b === 0)) return null;
  const mac = new Uint8Array(8);
  mac.set(raw6, 2);
  mac[6] = MODELS[model].macCode;
  mac[7] = 0x10;
  let chk = 0;
  for (const b of mac.subarray(2, 8)) chk ^= (b & 0x0f) ^ (b >> 4);
  mac[7] |= chk;
  return mac;
}

// The barcode printed on the label: the 6 stock bytes at 0xFC06 as 12 hex digits, "" if blank.
// Seen on 0000020663463415: its label is Code 128 "020663461013", whose first 8 digits are MAC bytes
// 2-5 (= stock bytes 0-3). That the last 4 digits are stock bytes 4-5 is inferred, not yet checked
// against a backup; those two bytes are not in the MAC, so only the stock flash or its backup has them.
export function barcodeFromStock(raw6) {
  if (!raw6 || raw6.length !== 6 || raw6.every((b) => b === 0xff) || raw6.every((b) => b === 0)) return "";
  return zp.toHex(raw6);
}

// Barcode from a stored stock backup of the main flash, if its 0xFC06 bytes give this MAC; else "".
export function barcodeFromBackup(flash, mac) {
  if (!flash || flash.length < STOCK_MAC_ADDR + 6) return "";
  const raw = flash.slice(STOCK_MAC_ADDR, STOCK_MAC_ADDR + 6);
  const model = modelOfMac(mac);
  const m = model && macFromStock(raw, model);
  return m && zp.bytesEqual(m, mac) ? barcodeFromStock(raw) : "";
}

// MAC typed in for a tag that has none: { mac, barcode }. Throws Error. Takes the 12 hex digits
// of the barcode on the label (the stock bytes) or the whole 16-digit MAC; spaces, ':' and '-'
// are ignored. A whole MAC must be one macFromStock could have made for this model (00 00
// prefix, model code, checksum), which catches typos.
export function macFromEntry(text, model) {
  const clean = String(text || "").replace(/[\s:-]/g, "").toUpperCase();
  if (!/^([0-9A-F]{12}|[0-9A-F]{16})$/.test(clean))
    throw new Error(`"${text}" is neither the 12-digit barcode from the label nor a 16-digit MAC`);
  const raw = Uint8Array.from(clean.match(/../g), (x) => parseInt(x, 16));
  if (raw.length === 6) {
    const mac = macFromStock(raw, model);
    if (!mac) throw new Error(`barcode ${clean} is blank`);
    return { mac, barcode: clean };
  }
  const expect = macFromStock(raw.slice(2, 8), model);
  if (!expect || !zp.bytesEqual(expect, raw))
    throw new Error(`MAC ${clean} does not fit a ${MODELS[model].name} tag (from its bytes it would be ${expect ? macStr(expect) : "none"})`);
  return { mac: raw, barcode: "" };
}

export function modelOfMac(mac) {
  return Object.keys(MODELS).find((k) => MODELS[k].macCode === mac[6]) || null;
}

export function checkFirmware(data) {
  if (!data || !data.length) throw new JobError("firmware", "the firmware file is empty");
  if (data.length > CODE_LIMIT)
    throw new JobError("firmware", `firmware is ${data.length} bytes, over the 0x${zp.hex(CODE_LIMIT, 4)} limit (the tag's layout and content store starts there)`);
  if (data.every((b) => b === 0xff)) throw new JobError("firmware", "the firmware file is only 0xFF");
  return data;
}

async function readTwice(fl, page, addr, length, what, progress) {
  await fl.selectPage(page);
  const a = await fl.read(addr, length, progress && ((d, t) => progress(d, 2 * t)));
  const b = await fl.read(addr, length, progress && ((d, t) => progress(t + d, 2 * t)));
  if (!zp.bytesEqual(a, b)) {
    const i = a.findIndex((v, k) => v !== b[k]);
    throw new JobError("read", `two reads of the ${what} differ (first at 0x${zp.hex(addr + i, 4)}): bad contact?`);
  }
  return a;
}

// Runs the whole job on the tag in the jig. Never throws for tag problems: returns a result.
// manualMac (barcode or MAC, see macFromEntry) is used only if the tag has no MAC of its own;
// res.macSource is then "typed in".
// backups: { has(macHex) -> Promise<bool>, save(macHex, flash, info, model, barcode) -> Promise,
//            and optionally get(macHex) -> Promise<{ flash, barcode }>, setBarcode(macHex, barcode) }
export async function flashTag(fl, model, firmware, backups, { log = () => {}, progress, writeMac = true, resetAfter = true, manualMac = "" } = {}) {
  const res = { model, mac: null, macSource: "", barcode: "", backup: "", ok: false, step: "", message: "", seconds: 0 };
  const t0 = Date.now();
  const prog = (label) => progress && ((d, t) => progress(label, d, t));
  let touched = false;
  try {
    if (!MODELS[model]) throw new JobError("setup", "unknown model " + model);
    const fw = checkFirmware(firmware);
    log(`Firmware: ${fw.length} bytes`);
    let typed = null;
    if (manualMac) {
      try {
        typed = macFromEntry(manualMac, model);
      } catch (e) {
        throw new JobError("setup", e.message);
      }
    }

    try {
      await fl.begin();
    } catch (e) {
      if (e instanceof zp.NoTagError) throw new JobError("contact", e.message);
      throw e;
    }
    touched = true;

    const info = await readTwice(fl, zp.PAGE_INFO, 0, zp.INFO_SIZE, "info page");
    let mac = macFromInfo(info);
    let needMacWrite = false;
    let raw = null; // stock bytes, when the MAC came from them
    if (mac) {
      res.macSource = "info page";
      const tagModel = modelOfMac(mac);
      if (tagModel && tagModel !== model) {
        res.mac = macStr(mac);
        throw new JobError("model", `MAC ${macStr(mac)} says ${MODELS[tagModel].name}, but ${MODELS[model].name} is chosen. Wrong tag or wrong model`);
      }
    } else {
      if (!info.slice(INFO_MAC_ADDR, INFO_MAC_ADDR + 8).every((b) => b === 0xff))
        throw new JobError("mac", "info page MAC bytes are all 0x00; not touching this tag");
      const stock = await readTwice(fl, zp.PAGE_MAIN, STOCK_MAC_ADDR, 6, "stock MAC");
      mac = macFromStock(stock, model);
      if (mac) {
        raw = stock;
        res.macSource = "stock flash";
        res.barcode = barcodeFromStock(raw);
      } else if (typed) {
        ({ mac, barcode: res.barcode } = typed);
        res.macSource = "typed in";
      } else {
        throw new JobError("mac", `no MAC in the info page and none at 0x${zp.hex(STOCK_MAC_ADDR, 4)}. Not stock firmware and no MAC written? Type in the barcode from the label or the MAC`);
      }
      needMacWrite = true;
    }
    if (typed && res.macSource !== "typed in") log(`The tag has its own MAC; the typed one (${manualMac}) is not used`);
    res.mac = macStr(mac);
    log(`MAC ${res.mac} (from ${res.macSource})`);
    if (res.barcode) log("Barcode " + res.barcode);

    if (await backups.has(res.mac)) {
      res.backup = "exists";
      log("A backup of this tag is already stored");
      const stored = backups.get && (await backups.get(res.mac));
      if (stored) {
        if (!res.barcode) {
          res.barcode = barcodeFromBackup(stored.flash, mac);
          if (res.barcode) log("Barcode " + res.barcode + " (from the backup)");
        }
        if (res.barcode && !stored.barcode && backups.setBarcode) await backups.setBarcode(res.mac, res.barcode);
      }
    } else {
      log("Reading the whole flash for the backup (two passes) ...");
      let blank;
      try {
        const flash = await readTwice(fl, zp.PAGE_MAIN, 0, zp.FLASH_SIZE, "main flash", prog("Backup"));
        if (raw && !zp.bytesEqual(flash.slice(STOCK_MAC_ADDR, STOCK_MAC_ADDR + 6), raw))
          throw new JobError("backup", "backup does not contain the MAC bytes read before");
        blank = flash.every((b) => b === 0xff);
        // A tag with its stock MAC never reads blank, so all 0xFF there means a bad read. A tag
        // whose MAC had to be typed in has lost its stock firmware already; all 0xFF there is
        // an empty flash (an earlier erase), with nothing to keep.
        if (blank && res.macSource !== "typed in") throw new JobError("backup", "the flash reads as all 0xFF; refusing to call that a backup");
        if (!blank) {
          await backups.save(res.mac, flash, info, model, res.barcode);
          if (!(await backups.has(res.mac))) throw new JobError("backup", "the stored backup cannot be found again");
        }
      } catch (e) {
        res.backup = "failed";
        throw new JobError("backup", "backup failed, nothing written: " + e.message);
      }
      if (blank) {
        res.backup = "blank";
        log("The flash is empty (all 0xFF): nothing to back up");
      } else {
        res.backup = "saved";
        log("Backup stored in this browser (download it under Backups)");
      }
    }

    if (needMacWrite) {
      if (!writeMac) {
        log("WARNING: MAC not written to the info page; the new firmware will not find it");
      } else {
        await fl.selectPage(zp.PAGE_INFO);
        if (!(await fl.read(INFO_MAC_ADDR, 8)).every((b) => b === 0xff))
          throw new JobError("mac", "info page MAC bytes are no longer blank; not writing");
        const lsb = mac.slice().reverse();
        await fl.writeChunk(INFO_MAC_ADDR, lsb);
        const back = await fl.read(INFO_MAC_ADDR, 8);
        if (!zp.bytesEqual(back, lsb)) throw new JobError("mac", "MAC read back from the info page is " + zp.toHex(back));
        log("MAC written to the info page 0x10-0x17");
      }
    }

    log("Erasing main flash ...");
    await fl.eraseMain();
    log("Writing firmware ...");
    await fl.write(0, fw, prog("Write"));
    log("Verifying ...");
    await fl.selectPage(zp.PAGE_MAIN);
    const back = await fl.read(0, fw.length, prog("Verify"));
    if (!zp.bytesEqual(back, fw)) {
      const i = back.findIndex((v, k) => v !== fw[k]);
      throw new JobError("verify", `verify failed at 0x${zp.hex(i, 4)} (wrote ${zp.hex(fw[i], 2)}, read ${zp.hex(back[i], 2)})`);
    }
    res.ok = true;
    res.message = `flashed ${fw.length} bytes, verified`;
    log("OK: " + res.message);
  } catch (e) {
    if (e instanceof JobError) [res.step, res.message] = [e.step, e.message];
    else if (e instanceof zp.FlasherError) [res.step, res.message] = ["serial", e.message];
    else [res.step, res.message] = ["internal", `${e.name}: ${e.message}`];
  } finally {
    if (touched && resetAfter) {
      try {
        await fl.reset();
      } catch {
        /* the result already says what went wrong */
      }
    }
    res.seconds = (Date.now() - t0) / 1000;
  }
  if (!res.ok) log(`FAILED (${res.step}): ${res.message}`);
  return res;
}

// Backups in IndexedDB: one record per MAC with the 64 KB main flash and the info page.
export class BackupStore {
  static async open(name = "openesl-backups") {
    const db = await new Promise((ok, fail) => {
      const req = indexedDB.open(name, 1);
      req.onupgradeneeded = () => req.result.createObjectStore("tags", { keyPath: "mac" });
      req.onsuccess = () => ok(req.result);
      req.onerror = () => fail(req.error);
    });
    return new BackupStore(db);
  }

  constructor(db) {
    this.db = db;
  }

  tx(mode, f) {
    return new Promise((ok, fail) => {
      const t = this.db.transaction("tags", mode);
      const req = f(t.objectStore("tags"));
      t.oncomplete = () => ok(req.result);
      t.onerror = () => fail(t.error);
      t.onabort = () => fail(t.error || new Error("aborted"));
    });
  }

  async has(mac) {
    const r = await this.get(mac);
    return !!(r && r.flash && r.flash.length === zp.FLASH_SIZE);
  }

  get(mac) {
    return this.tx("readonly", (s) => s.get(mac));
  }

  async save(mac, flash, info, model = "", barcode = "") {
    if (await this.has(mac)) throw new Error("refusing to overwrite the existing backup of " + mac);
    await this.tx("readwrite", (s) => s.add({ mac, model, barcode, time: new Date().toISOString(), flash, info }));
  }

  // Changes the barcode (or other notes) of a stored backup; the flash itself is never changed.
  async setBarcode(mac, barcode) {
    const r = await this.get(mac);
    if (!r) throw new Error("no backup of " + mac);
    r.barcode = barcode;
    await this.tx("readwrite", (s) => s.put(r));
  }

  list() {
    return this.tx("readonly", (s) => s.getAll());
  }
}
