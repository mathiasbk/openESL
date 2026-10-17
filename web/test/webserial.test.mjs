// WebSerialTransport (js/zbs.js), the Web Serial side that both the tag flasher and the AP's
// USB port (js/apport.js) go through, against a fake SerialPort built on Node's web streams.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as zp from "../js/zbs.js";
import { FakeFlasherTransport, stockTag } from "./fake_flasher.mjs";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The parts of a Web Serial SerialPort the transport uses. push() is the device sending bytes;
// written holds what the page sent. After a read error (a framing or parity error) the browser
// gives the port a new readable, and so does breakStream().
class FakePort {
  constructor({ signalsFail = false, onWrite = null } = {}) {
    this.signalsFail = signalsFail;
    this.onWrite = onWrite;
    this.written = [];
    this.writes = 0;
    this.openedWith = null;
    this.signals = null;
    this.closed = false;
    this.cancelled = 0;
    this.newReadable();
    this.writable = new WritableStream({
      write: (chunk) => {
        this.writes++;
        this.written.push(...chunk);
        if (this.onWrite) this.onWrite(chunk);
      },
    });
  }
  newReadable() {
    this.readable = new ReadableStream({
      start: (c) => {
        this.ctrl = c;
      },
      cancel: () => {
        this.cancelled++;
      },
    });
  }
  async open(opts) {
    this.openedWith = opts;
  }
  async setSignals(s) {
    if (this.signalsFail) throw new Error("not supported");
    this.signals = s;
  }
  async close() {
    this.closed = true;
  }
  push(bytes) {
    this.ctrl.enqueue(Uint8Array.from(bytes));
  }
  breakStream() {
    const c = this.ctrl;
    this.newReadable();
    c.error(new DOMException("framing error", "FramingError"));
  }
}

test("open: 115200 baud, DTR and RTS low unless told not to", async () => {
  const p = new FakePort();
  const t = await zp.WebSerialTransport.open(p);
  assert.deepEqual(p.openedWith, { baudRate: 115200 });
  assert.deepEqual(p.signals, { dataTerminalReady: false, requestToSend: false });
  await t.close();

  const q = new FakePort();
  await (await zp.WebSerialTransport.open(q, { lowSignals: false })).close();
  assert.equal(q.signals, null);
});

test("open: a driver without setSignals is fine", async () => {
  const p = new FakePort({ signalsFail: true });
  const t = await zp.WebSerialTransport.open(p);
  p.push([1, 2]);
  assert.deepEqual(Array.from(await t.read(2, 100)), [1, 2]);
  await t.close();
});

test("write sends the bytes and lets go of the writer", async () => {
  const p = new FakePort();
  const t = await zp.WebSerialTransport.open(p);
  await t.write(Uint8Array.from([0x41, 0x54]));
  await t.write(Uint8Array.from([1, 0]));      // a held writer lock would make this throw
  assert.deepEqual(p.written, [0x41, 0x54, 1, 0]);
  assert.equal(p.writes, 2);
  await t.close();
});

test("read: bytes from several chunks, at most n, the rest kept", async () => {
  const p = new FakePort();
  const t = await zp.WebSerialTransport.open(p);
  p.push([1, 2, 3]);
  p.push([4]);
  p.push([5, 6]);
  await sleep(5);
  assert.deepEqual(Array.from(await t.read(4, 100)), [1, 2, 3, 4]);
  assert.deepEqual(Array.from(await t.read(10, 100)), [5, 6]);
  await t.close();
});

test("read: wakes as soon as bytes arrive, empty after the timeout", async () => {
  const p = new FakePort();
  const t = await zp.WebSerialTransport.open(p);
  setTimeout(() => p.push([7, 8]), 20);
  let start = Date.now();
  assert.deepEqual(Array.from(await t.read(5, 2000)), [7, 8]);
  assert.ok(Date.now() - start < 1000, "read waited for the timeout although bytes came");

  start = Date.now();
  const none = await t.read(5, 60);
  assert.ok(none instanceof Uint8Array);
  assert.equal(none.length, 0);
  assert.ok(Date.now() - start >= 50, "read returned before its timeout");
  await t.close();
});

test("flushInput drops what was received", async () => {
  const p = new FakePort();
  const t = await zp.WebSerialTransport.open(p);
  p.push([1, 2, 3]);
  await sleep(5);
  await t.flushInput();
  assert.equal((await t.read(3, 20)).length, 0);
  p.push([9]);
  assert.deepEqual(Array.from(await t.read(3, 100)), [9]);
  await t.close();
});

test("keeps reading after a framing error", async () => {
  const p = new FakePort();
  const t = await zp.WebSerialTransport.open(p);
  p.push([1]);
  await sleep(5);
  p.breakStream();
  await sleep(5);
  p.push([2, 3]);
  assert.deepEqual(Array.from(await t.read(1, 100)), [1]);
  assert.deepEqual(Array.from(await t.read(5, 100)), [2, 3]);
  await t.close();
});

test("close cancels the reader and closes the port", async () => {
  const p = new FakePort();
  const t = await zp.WebSerialTransport.open(p);
  await sleep(5);
  await t.close();
  assert.equal(p.cancelled, 1);
  assert.equal(p.closed, true);
  await sleep(5);
  assert.equal(p.readable.locked, false, "the pump still holds the reader after close");
});

// The simulated ZBS_Flasher behind the fake port, answering in pieces of a few bytes as a
// USB serial chip does: the flasher protocol must put the frames back together.
function flasherPort(tag, piece = 3) {
  const fake = new FakeFlasherTransport(tag);
  const port = new FakePort({
    onWrite: async (chunk) => {
      await fake.write(chunk);
      const out = await fake.read(1 << 20);
      for (let i = 0; i < out.length; i += piece) {
        const part = out.subarray(i, i + piece);
        setTimeout(() => port.push(part), 1);
      }
    },
  });
  return { fake, port };
}

test("ZBS_Flasher over Web Serial: probe, then read the tag's flash and info page", async () => {
  const tag = stockTag(3);
  const { port } = flasherPort(tag);
  const t = await zp.WebSerialTransport.open(port);
  const fl = new zp.ZbsFlasher(t, { timeoutMs: 500 });
  await zp.probe(fl, 1000);
  await fl.begin();
  assert.deepEqual(await fl.read(0xfc00, 600), tag.flash.subarray(0xfc00, 0xfc00 + 600));
  await fl.selectPage(zp.PAGE_INFO);
  assert.deepEqual(await fl.read(0, 16), tag.info.subarray(0, 16));
  await t.close();
});

test("ZBS_Flasher over Web Serial: write and read back", async () => {
  const tag = stockTag(4);
  const { port } = flasherPort(tag, 7);
  const t = await zp.WebSerialTransport.open(port);
  const fl = new zp.ZbsFlasher(t, { timeoutMs: 500 });
  await zp.probe(fl, 1000);
  await fl.begin();
  await fl.eraseMain();
  const data = Uint8Array.from({ length: 700 }, (_, i) => (i * 13 + 5) & 0xff);
  await fl.write(0x100, data);
  assert.deepEqual(await fl.read(0x100, data.length), data);
  await t.close();
});
