// The browser flasher (js/zbs.js, js/tagflash.js) against the simulated ZBS_Flasher.
// Mirrors tools/flash-tui/tests/test_proto.py and test_workflow.py.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as zp from "../js/zbs.js";
import * as wf from "../js/tagflash.js";
import { FakeFlasherTransport, FakeTag, MemoryBackups, stockTag } from "./fake_flasher.mjs";

const fw = () => Uint8Array.from({ length: 30000 }, (_, i) => (i * 7 + 3) & 0xff);
const setup = (tag = stockTag()) => {
  const port = new FakeFlasherTransport(tag);
  return { tag, port, fl: new zp.ZbsFlasher(port, { timeoutMs: 5 }), backups: new MemoryBackups() };
};

test("frame encoding", () => {
  assert.deepEqual(Array.from(zp.encodeFrame(1)), [0x41, 0x54, 1, 0, 0xab, 0x35]);
  assert.throws(() => zp.encodeFrame(1, new Uint8Array(256)));
});

test("MAC from stock flash matches the earlier ZBS_Flasher backups", () => {
  const h = (s) => Uint8Array.from(s.match(/../g), (x) => parseInt(x, 16));
  assert.equal(wf.macStr(wf.macFromStock(h("020793500102"), "1.54")), "000002079350341C");
  assert.equal(wf.macStr(wf.macFromStock(h("020663460102"), "1.54")), "0000020663463415");
  assert.equal(wf.macStr(wf.macFromStock(h("01c280920102"), "2.9")), "000001C280923B15");
  assert.equal(wf.macFromStock(new Uint8Array(6).fill(0xff), "2.9"), null);
  const info = new Uint8Array(0x400).fill(0xff);
  info.set(h("1c34509307020000"), 0x10);
  assert.equal(wf.macStr(wf.macFromInfo(info)), "000002079350341C");
  assert.equal(wf.modelOfMac(wf.macFromInfo(info)), "1.54");
});

test("probe and retries on lost and corrupt answers and noise", async () => {
  const { port, fl } = setup();
  assert.equal(await zp.probe(fl), 0x20);
  port.dropNext = 1;
  port.corruptNext = 1;
  port.noise = [1, 2, 0x41, 3];
  assert.equal(await fl.version(), 0x20);
});

test("probe rejects a silent port", async () => {
  const silent = { write: async () => {}, read: async () => new Uint8Array(0), flushInput: async () => {} };
  await assert.rejects(zp.probe(new zp.ZbsFlasher(silent, { timeoutMs: 1 }), 50), /does not answer/);
});

test("stock tag: backup, MAC to info page, firmware, verify, reset", async () => {
  const { tag, port, fl, backups } = setup();
  const stock = tag.flash.slice();
  const image = fw();
  const r = await wf.flashTag(fl, "2.9", image, backups);
  assert.ok(r.ok, r.message);
  assert.equal(r.macSource, "stock flash");
  assert.equal(r.backup, "saved");
  assert.deepEqual(backups.m.get(r.mac).flash, stock);
  assert.deepEqual(tag.flash.slice(0, image.length), image);
  assert.equal(wf.macStr(wf.macFromInfo(tag.info)), r.mac);
  assert.equal(tag.eraseCount[1], 0, "the info page must never be erased");
  assert.ok(!port.commands.includes(27));
  assert.equal(port.commands.at(-1), zp.CMD_RESET_ZBS);

  // second run: MAC from the info page, backup kept
  const r2 = await wf.flashTag(fl, "2.9", image, backups);
  assert.ok(r2.ok);
  assert.equal(r2.macSource, "info page");
  assert.equal(r2.backup, "exists");
  assert.deepEqual(backups.m.get(r.mac).flash, stock);
});

test("no tag", async () => {
  const { fl, backups } = setup(null);
  const r = await wf.flashTag(fl, "2.9", fw(), backups);
  assert.equal(r.ok, false);
  assert.equal(r.step, "contact");
});

test("bad firmware touches nothing", async () => {
  const { port, fl, backups } = setup();
  for (const bad of [new Uint8Array(0), new Uint8Array(0xfc01).fill(1), new Uint8Array(10).fill(0xff)]) {
    const r = await wf.flashTag(fl, "2.9", bad, backups);
    assert.equal(r.step, "firmware");
  }
  assert.equal(port.commands.length, 0);
});

test("flaky backup read or failed save writes nothing", async () => {
  {
    const { tag, fl, backups } = setup();
    const before = tag.flash.slice();
    let n = 0;
    tag.readCorrupt = (page, a, v) => (page === 0 && a === 0x1234 && n++ % 2 ? v ^ 1 : v);
    const r = await wf.flashTag(fl, "2.9", fw(), backups);
    assert.equal(r.step, "backup");
    assert.deepEqual(tag.flash, before);
    assert.equal(tag.eraseCount[0], 0);
  }
  {
    const { tag, fl, backups } = setup();
    backups.failSave = true;
    const r = await wf.flashTag(fl, "2.9", fw(), backups);
    assert.equal(r.step, "backup");
    assert.equal(tag.eraseCount[0], 0);
    assert.ok(tag.info.slice(0x10, 0x18).every((b) => b === 0xff));
  }
});

test("verify and write failures are reported", async () => {
  const { tag, fl, backups } = setup();
  tag.stuck.set("0:100", 0x01);
  const image = fw();
  image[100] = 0x02;
  const r = await wf.flashTag(fl, "2.9", image, backups);
  assert.equal(r.ok, false);
  assert.equal(r.step, "serial");
  assert.match(r.message, /write failed/);
});

test("wrong model", async () => {
  const { fl, backups } = setup();
  assert.ok((await wf.flashTag(fl, "2.9", fw(), backups)).ok);
  const r = await wf.flashTag(fl, "1.54", fw(), backups);
  assert.equal(r.step, "model");
});

test("no MAC anywhere", async () => {
  const tag = new FakeTag(new Uint8Array(zp.FLASH_SIZE).fill(0x12));
  tag.flash.fill(0xff, 0xfc00, 0xfc10);
  const { fl, backups } = setup(tag);
  const r = await wf.flashTag(fl, "2.9", fw(), backups);
  assert.equal(r.step, "mac");
  assert.equal(tag.eraseCount[0], 0);
});

test("firmware must end below the tag's layout and content store at 0xF400", () => {
  assert.equal(wf.CODE_LIMIT, 0xf400);
  const ok = new Uint8Array(0xf400).fill(0x12);
  assert.equal(wf.checkFirmware(ok), ok);
  assert.throws(() => wf.checkFirmware(new Uint8Array(0xf401).fill(0x12)), /0xF400/i);
});
