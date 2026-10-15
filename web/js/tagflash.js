// tagflash.js - the per-tag flashing job for the browser. A port of tools/flash-tui/workflow.py
// with the same order and rules (see there):
//
//   1. ZBS_BEGIN; no answer from the tag -> stop.
//   2. Read the info page twice; the reads must match.
//   3. MAC from info page 0x10-0x17 (LSB first), whose model code must match the chosen model,
//      or, on a tag with stock firmware (blank bytes), from flash 0xFC06-0xFC0B.
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

export const MODELS = {
  "2.9": { name: '2.9"', macCode: 0x3b, firmware: "firmware/tag-29.bin" },
  "1.54": { name: '1.54"', macCode: 0x34, firmware: "firmware/tag-154.bin" },
};

// tag/tools/build.py CODE_SIZE: from 0xF400 the tag keeps its layout file (0xF400) and its
// last content (0xF800), and the stock MAC sits at 0xFC00
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
// backups: { has(macHex) -> Promise<bool>, save(macHex, flash, info, model) -> Promise }
export async function flashTag(fl, model, firmware, backups, { log = () => {}, progress, writeMac = true, resetAfter = true } = {}) {
  const res = { model, mac: null, macSource: "", backup: "", ok: false, step: "", message: "", seconds: 0 };
  const t0 = Date.now();
  const prog = (label) => progress && ((d, t) => progress(label, d, t));
  let touched = false;
  try {
    if (!MODELS[model]) throw new JobError("setup", "unknown model " + model);
    const fw = checkFirmware(firmware);
    log(`Firmware: ${fw.length} bytes`);

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
    let raw;
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
      raw = await readTwice(fl, zp.PAGE_MAIN, STOCK_MAC_ADDR, 6, "stock MAC");
      mac = macFromStock(raw, model);
      if (!mac)
        throw new JobError("mac", `no MAC in the info page and none at 0x${zp.hex(STOCK_MAC_ADDR, 4)}. Not stock firmware and no MAC written?`);
      res.macSource = "stock flash";
      needMacWrite = true;
    }
    res.mac = macStr(mac);
    log(`MAC ${res.mac} (from ${res.macSource})`);

    if (await backups.has(res.mac)) {
      res.backup = "exists";
      log("A backup of this tag is already stored");
    } else {
      log("Reading the whole flash for the backup (two passes) ...");
      try {
        const flash = await readTwice(fl, zp.PAGE_MAIN, 0, zp.FLASH_SIZE, "main flash", prog("Backup"));
        if (needMacWrite && !zp.bytesEqual(flash.slice(STOCK_MAC_ADDR, STOCK_MAC_ADDR + 6), raw))
          throw new JobError("backup", "backup does not contain the MAC bytes read before");
        if (flash.every((b) => b === 0xff)) throw new JobError("backup", "the flash reads as all 0xFF; refusing to call that a backup");
        await backups.save(res.mac, flash, info, model);
        if (!(await backups.has(res.mac))) throw new JobError("backup", "the stored backup cannot be found again");
      } catch (e) {
        res.backup = "failed";
        throw new JobError("backup", "backup failed, nothing written: " + e.message);
      }
      res.backup = "saved";
      log("Backup stored in this browser (download it under Backups)");
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

  async save(mac, flash, info, model = "") {
    if (await this.has(mac)) throw new Error("refusing to overwrite the existing backup of " + mac);
    await this.tx("readwrite", (s) => s.add({ mac, model, barcode: "", time: new Date().toISOString(), flash, info }));
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
