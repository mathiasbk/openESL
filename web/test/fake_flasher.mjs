// A simulated ZBS_Flasher behind a fake transport, for the tests. A port of
// tools/flash-tui/fake_flasher.py with the same quirks: WRITE_FLASH skips 0xFF bytes and
// reads every byte back (4 tries), a failed byte gets [0] and then also [1], frames with a
// bad CRC get no answer. Flash behaves like NOR: erase sets 0xFF, writes only clear bits.
import * as zp from "../js/zbs.js";

export function stockTag(seed = 1) {
  let x = seed;
  const rnd = () => ((x = (x * 1103515245 + 12345) >>> 0) >>> 16) & 0xff;
  const flash = new Uint8Array(zp.FLASH_SIZE).fill(0xff);
  for (let i = 0; i < 0xc000; i++) flash[i] = rnd();
  flash.set([0, 0, 0, 0, 0, 0, 0x02, 0x07, 0x93, 0x50 + (seed % 16), 0x01, 0x02], 0xfc00);
  const info = new Uint8Array(zp.INFO_SIZE).fill(0xff);
  info.set([0x0c, 0x17, 0x21, 0x08, 0x01, 0x09, 0, 0, 0x01, 0x18, 0x3a, 0x02, 0x9a, 0x03, 0x2b, 0x18]);
  return new FakeTag(flash, info);
}

export class FakeTag {
  constructor(flash, info) {
    this.pages = [flash || new Uint8Array(zp.FLASH_SIZE).fill(0xff), info || new Uint8Array(zp.INFO_SIZE).fill(0xff)];
    this.page = 0;
    this.eraseCount = [0, 0];
    this.readCorrupt = null;
    this.stuck = new Map(); // "page:addr" -> bits that never clear
  }
  get flash() { return this.pages[0]; }
  get info() { return this.pages[1]; }
  read(a) {
    const p = this.pages[this.page];
    let v = p[a % p.length];
    if (this.readCorrupt) v = this.readCorrupt(this.page, a, v);
    return v;
  }
  write(a, v) {
    const p = this.pages[this.page];
    const i = a % p.length;
    p[i] &= v | (this.stuck.get(`${this.page}:${i}`) || 0);
  }
  erase(page) {
    this.pages[page].fill(0xff);
    this.eraseCount[page]++;
  }
}

export class FakeFlasherTransport {
  constructor(tag = null) {
    this.tag = tag;
    this.rx = [];
    this.buf = [];
    this.commands = [];
    this.powered = false;
    this.dropNext = 0;
    this.corruptNext = 0;
    this.noise = [];
  }
  async write(data) {
    this.buf.push(...data);
    this.parse();
  }
  async read(n) {
    return Uint8Array.from(this.rx.splice(0, n));
  }
  async flushInput() {
    this.rx = [];
  }
  parse() {
    for (;;) {
      let i = -1;
      for (let k = 0; k + 1 < this.buf.length; k++) if (this.buf[k] === 0x41 && this.buf[k + 1] === 0x54) { i = k; break; }
      if (i < 0) { this.buf = this.buf.at(-1) === 0x41 ? [0x41] : []; return; }
      this.buf.splice(0, i);
      if (this.buf.length < 4) return;
      const n = this.buf[3];
      if (this.buf.length < 6 + n) return;
      const body = this.buf.slice(2, 4 + n);
      const crc = (this.buf[4 + n] << 8) | this.buf[5 + n];
      this.buf.splice(0, 6 + n);
      if (crc === zp.checksum(body)) this.handle(body[0], body.slice(2));
    }
  }
  answer(cmd, data = []) {
    if (this.dropNext) { this.dropNext--; return; }
    const f = Array.from(zp.encodeFrame(cmd, Uint8Array.from(data)));
    if (this.corruptNext) { this.corruptNext--; f[f.length - 1] ^= 0x55; }
    this.rx.push(...this.noise, ...f);
    this.noise = [];
  }
  handle(cmd, data) {
    this.commands.push(cmd);
    const tag = this.powered ? this.tag : null;
    switch (cmd) {
      case zp.CMD_GET_VERSION: return this.answer(cmd, [0, 0, 0, 0x20]);
      case zp.CMD_ZBS_BEGIN: this.powered = true; return this.answer(cmd, [this.tag ? 1 : 0]);
      case zp.CMD_RESET_ZBS: this.powered = !!this.tag; if (this.tag) this.tag.page = 0; return this.answer(cmd, [1]);
      case zp.CMD_SELECT_PAGE: if (tag) tag.page = data[0] ? 1 : 0; return this.answer(cmd, [tag ? 1 : 0]);
      case zp.CMD_READ_FLASH: {
        const [n, a] = [data[0], (data[1] << 8) | data[2]];
        return this.answer(cmd, Array.from({ length: n }, (_, i) => (tag ? tag.read(a + i) : 0xff)));
      }
      case zp.CMD_WRITE_FLASH: {
        const [n, a] = [data[0], (data[1] << 8) | data[2]];
        if (n >= 0xff - 3) return this.answer(cmd, [0xee]);
        for (let i = 0; i < n; i++) {
          const v = data[3 + i];
          if (v === 0xff) continue;
          let ok = false;
          for (let t = 0; t < 4 && tag; t++) {
            tag.write(a + i, v);
            if (tag.read(a + i) === v) { ok = true; break; }
          }
          if (!ok) { this.answer(cmd, [0]); break; }
        }
        return this.answer(cmd, [1]);
      }
      case zp.CMD_ERASE_FLASH: if (tag) tag.erase(0); return this.answer(cmd, [1]);
      case 27: if (tag) tag.erase(1); return this.answer(cmd, [1]);
      default: return undefined;
    }
  }
}

export class MemoryBackups {
  constructor() { this.m = new Map(); this.failSave = false; }
  async has(mac) { return this.m.has(mac); }
  async save(mac, flash, info, model = "", barcode = "") {
    if (this.failSave) throw new Error("disk full");
    if (this.m.has(mac)) throw new Error("exists");
    this.m.set(mac, { flash, info, model, barcode });
  }
  async get(mac) { return this.m.get(mac); }
  async setBarcode(mac, barcode) { this.m.get(mac).barcode = barcode; }
}
