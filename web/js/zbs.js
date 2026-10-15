// zbs.js - the ZBS_Flasher serial protocol (atc1441's ZBS_Flasher firmware on a D1 mini) for the
// browser, over Web Serial. A port of tools/flash-tui/zbs_proto.py; the frame format and every
// command are described there. As there, ERASE_INFOBLOCK (27) is not implemented: this page
// never erases the info page.
//
// A transport is anything with:
//   write(Uint8Array) -> Promise
//   read(n, timeoutMs) -> Promise<Uint8Array>   up to n bytes, fewer on timeout
//   flushInput() -> Promise                     drops everything received so far

export const BAUD = 115200;

export const CMD_GET_VERSION = 1;
export const CMD_ZBS_BEGIN = 10;
export const CMD_RESET_ZBS = 11;
export const CMD_SELECT_PAGE = 12;
export const CMD_READ_FLASH = 22;
export const CMD_WRITE_FLASH = 23;
export const CMD_ERASE_FLASH = 26;

export const PAGE_MAIN = 0;
export const PAGE_INFO = 1;

export const FLASH_SIZE = 0x10000;
export const INFO_SIZE = 0x400;
export const READ_CHUNK = 0xff;
export const WRITE_CHUNK = 250;
const CRC_INIT = 0xab34;

export class FlasherError extends Error {}
export class NoTagError extends FlasherError {}

export function checksum(body) {
  let s = CRC_INIT;
  for (const b of body) s += b;
  return s & 0xffff;
}

export function encodeFrame(cmd, data = new Uint8Array(0)) {
  if (data.length > 255) throw new RangeError("payload too long: " + data.length);
  const f = new Uint8Array(6 + data.length);
  f[0] = 0x41; // 'A'
  f[1] = 0x54; // 'T'
  f[2] = cmd & 0xff;
  f[3] = data.length;
  f.set(data, 4);
  const crc = checksum(f.subarray(2, 4 + data.length));
  f[4 + data.length] = crc >> 8;
  f[5 + data.length] = crc & 0xff;
  return f;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

export class ZbsFlasher {
  constructor(transport, { retries = 3, slowSpi = false, timeoutMs = 2000 } = {}) {
    this.t = transport;
    this.retries = retries;
    this.slowSpi = slowSpi;
    this.timeoutMs = timeoutMs;
  }

  // ---- framing ----

  async flushInput() {
    await this.t.flushInput();
    await sleep(20);
    await this.t.flushInput();
  }

  async readExact(n, what) {
    const out = new Uint8Array(n);
    let got = 0;
    while (got < n) {
      const part = await this.t.read(n - got, this.timeoutMs);
      if (!part.length) throw new FlasherError(`timeout in answer ${what} (${got} of ${n} bytes)`);
      out.set(part, got);
      got += part.length;
    }
    return out;
  }

  async readFrame() {
    // skip anything before "AT" (boot noise, a stray answer)
    let prev = -1;
    for (;;) {
      const b = await this.t.read(1, this.timeoutMs);
      if (!b.length) throw new FlasherError("timeout waiting for an answer");
      if (prev === 0x41 && b[0] === 0x54) break;
      prev = b[0];
    }
    const head = await this.readExact(2, "header");
    const data = head[1] ? await this.readExact(head[1], "data") : new Uint8Array(0);
    const crc = await this.readExact(2, "CRC");
    const body = new Uint8Array(2 + data.length);
    body.set(head);
    body.set(data, 2);
    if (((crc[0] << 8) | crc[1]) !== checksum(body)) throw new FlasherError("CRC error in answer");
    return { cmd: head[0], data };
  }

  async query(cmd, data = new Uint8Array(0), retries = this.retries) {
    let last;
    for (let i = 0; i < Math.max(1, retries); i++) {
      try {
        await this.t.write(encodeFrame(cmd, data));
        const r = await this.readFrame();
        if (r.cmd !== cmd) throw new FlasherError(`answer for command ${r.cmd}, expected ${cmd}`);
        return r.data;
      } catch (e) {
        if (!(e instanceof FlasherError)) throw e;
        last = e;
        await this.flushInput();
      }
    }
    throw new FlasherError(`command ${cmd} failed after ${retries} tries: ${last.message}`);
  }

  async status(cmd, data) {
    const r = await this.query(cmd, data);
    if (r.length !== 1) throw new FlasherError(`command ${cmd}: expected 1 status byte, got ${r.length}`);
    return r[0];
  }

  // ---- commands ----

  async version(retries) {
    const r = await this.query(CMD_GET_VERSION, undefined, retries);
    if (r.length !== 4) throw new FlasherError(`version answer has ${r.length} bytes, expected 4`);
    return ((r[0] << 24) | (r[1] << 16) | (r[2] << 8) | r[3]) >>> 0;
  }

  async begin() {
    if ((await this.status(CMD_ZBS_BEGIN, Uint8Array.of(this.slowSpi ? 1 : 0))) !== 1)
      throw new NoTagError("no contact with the tag (check that it sits right in the jig)");
  }

  async reset() {
    await this.query(CMD_RESET_ZBS);
  }

  async selectPage(page) {
    if ((await this.status(CMD_SELECT_PAGE, Uint8Array.of(page ? 1 : 0))) !== 1)
      throw new FlasherError(`could not select flash page ${page} (tag contact?)`);
  }

  async readChunk(addr, length) {
    if (!(length > 0 && length <= READ_CHUNK)) throw new RangeError("read length " + length);
    if (addr < 0 || addr + length > FLASH_SIZE) throw new RangeError("read out of range");
    const r = await this.query(CMD_READ_FLASH, Uint8Array.of(length, addr >> 8, addr & 0xff));
    if (r.length !== length) throw new FlasherError(`read 0x${hex(addr, 4)}: got ${r.length} of ${length} bytes`);
    return r;
  }

  async read(addr, length, progress) {
    const out = new Uint8Array(length);
    let done = 0;
    while (done < length) {
      const n = Math.min(READ_CHUNK, length - done);
      out.set(await this.readChunk(addr + done, n), done);
      done += n;
      if (progress) progress(done, length);
    }
    return out;
  }

  async writeChunk(addr, data) {
    if (!(data.length > 0 && data.length <= WRITE_CHUNK)) throw new RangeError("write length " + data.length);
    if (addr < 0 || addr + data.length > FLASH_SIZE) throw new RangeError("write out of range");
    const msg = new Uint8Array(3 + data.length);
    msg.set([data.length, addr >> 8, addr & 0xff]);
    msg.set(data, 3);
    const r = await this.query(CMD_WRITE_FLASH, msg);
    if (!(r.length === 1 && r[0] === 1)) {
      await this.flushInput(); // on a failed byte the firmware answers [0] and then [1]
      throw new FlasherError(`write failed at 0x${hex(addr, 4)} (answer ${toHex(r)})`);
    }
  }

  async write(addr, data, progress) {
    let done = 0;
    while (done < data.length) {
      const chunk = data.subarray(done, done + WRITE_CHUNK);
      if (!chunk.every((b) => b === 0xff)) await this.writeChunk(addr + done, chunk);
      done += chunk.length;
      if (progress) progress(done, data.length);
    }
  }

  async eraseMain() {
    await this.selectPage(PAGE_MAIN); // so the info page is never hit
    if ((await this.status(CMD_ERASE_FLASH)) !== 1) throw new FlasherError("erase failed");
  }
}

// Checks that the port really is a ZBS_Flasher (GET_VERSION only, the tag is not touched).
// Repeats for waitMs, since a D1 mini reset by opening the port needs about a second to boot.
export async function probe(fl, waitMs = 3000) {
  const end = Date.now() + waitMs;
  let last;
  for (;;) {
    try {
      await fl.flushInput();
      return await fl.version(1);
    } catch (e) {
      if (!(e instanceof FlasherError)) throw e;
      last = e;
    }
    if (Date.now() >= end)
      throw new FlasherError(`the port does not answer the ZBS_Flasher protocol (${last.message}). ` +
        "Wrong port, e.g. the debug Wemos?");
    await sleep(200);
  }
}

export function hex(v, w) {
  return v.toString(16).toUpperCase().padStart(w, "0");
}

export function toHex(bytes) {
  return Array.from(bytes, (b) => hex(b, 2)).join("");
}

export { eq as bytesEqual };

// Web Serial port as a transport. DTR and RTS are set low after opening, so the D1 mini's
// auto-reset circuit is not pulsed on purpose (probe() waits if it resets anyway).
// lowSignals: false leaves them as the port opened, as esl_client.py does on the AP.
export class WebSerialTransport {
  constructor(port) {
    this.port = port;
    this.buf = [];
    this.waiters = [];
    this.closed = false;
  }

  static async open(port, { lowSignals = true } = {}) {
    await port.open({ baudRate: BAUD });
    if (lowSignals) {
      try {
        await port.setSignals({ dataTerminalReady: false, requestToSend: false });
      } catch {
        /* not every driver supports it */
      }
    }
    const t = new WebSerialTransport(port);
    t.pump();
    return t;
  }

  async pump() {
    while (!this.closed && this.port.readable) {
      this.reader = this.port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          for (const b of value) this.buf.push(b);
          this.wake();
        }
      } catch {
        /* a framing/parity error: keep reading */
      } finally {
        this.reader.releaseLock();
      }
    }
  }

  wake() {
    const w = this.waiters;
    this.waiters = [];
    w.forEach((f) => f());
  }

  async write(data) {
    const w = this.port.writable.getWriter();
    try {
      await w.write(data);
    } finally {
      w.releaseLock();
    }
  }

  async read(n, timeoutMs) {
    const end = Date.now() + timeoutMs;
    while (!this.buf.length) {
      const left = end - Date.now();
      if (left <= 0) return new Uint8Array(0);
      await new Promise((r) => {
        const t = setTimeout(r, left);
        this.waiters.push(() => {
          clearTimeout(t);
          r();
        });
      });
    }
    return Uint8Array.from(this.buf.splice(0, n));
  }

  async flushInput() {
    this.buf = [];
  }

  async close() {
    this.closed = true;
    try {
      await this.reader?.cancel();
    } catch {
      /* already closed */
    }
    await this.port.close();
  }
}
