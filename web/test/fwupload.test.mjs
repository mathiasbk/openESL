// Firmware from the dashboard (js/fwupload.js) against a simulated AP that checks the
// parts as esl_tagfw_part() and the AP's POST /api/ota do (docs/openapi.yaml).
import { test } from "node:test";
import assert from "node:assert/strict";
import { PART, firmwareKind, sendTagFirmware, sendApFirmware, tagFirmwareRows, waitApBack, fmtVer } from "../js/fwupload.js";

const enc = new TextEncoder();

// A tag firmware file: the 16-byte header (shared/proto.h ESL_FWF_*) and size bytes
function tagFile(model, ver, size) {
  const d = new Uint8Array(16 + size);
  d.set([0x4f, 0x53, 0x46, 0x57, 1, model, ver & 0xff, ver >> 8, size & 0xff, (size >> 8) & 0xff, size >> 16, 0]);
  for (let i = 0; i < size; i++) d[16 + i] = (i * 7) & 0xff;
  return d;
}

function apImage(size) {
  const d = new Uint8Array(size);
  d[0] = 0xe9;
  return d;
}

// The AP's side of an upload in parts: offset 0 starts one, every other part must go on
// from the last; the reply to the last part says what the AP now has
function simAp() {
  const ap = { requests: [], got: null, total: 0, done: null, kind: null };
  const reply = (status, obj) => ({ status, body: enc.encode(JSON.stringify(obj)) });
  ap.send = async (method, path, body) => {
    const u = new URL("http://ap" + path);
    ap.requests.push({ method, path: u.pathname, offset: Number(u.searchParams.get("offset")), len: body.length });
    if (body.length > 24576) return reply(413, { error: "body too large" });
    const off = Number(u.searchParams.get("offset"));
    const total = Number(u.searchParams.get("total"));
    if (off === 0) {
      ap.got = [];
      ap.total = total;
      ap.kind = u.pathname;
    } else if (!ap.got || off !== ap.got.length || total !== ap.total || u.pathname !== ap.kind) {
      return reply(409, { error: "not the next part of the upload in progress" });
    }
    ap.got.push(...body);
    if (ap.got.length < total) return reply(200, { received: ap.got.length, total });
    ap.done = Uint8Array.from(ap.got);
    if (u.pathname === "/api/ota") return reply(200, { slot: "ota_1", version: "0.3.0", received: total, total });
    const k = firmwareKind(ap.done);
    return reply(200, { model: k.model, version: k.version, size: k.size, crc: "00000000", received: total, total });
  };
  return ap;
}

test("what a file is", () => {
  assert.deepEqual(firmwareKind(tagFile(2, 290, 1024)), { kind: "tag", model: "1.54", version: "0.2.90", size: 1024 });
  assert.equal(firmwareKind(tagFile(1, 200, 1024)).model, "2.9");
  assert.equal(firmwareKind(tagFile(9, 200, 1024)).kind, null);
  assert.equal(firmwareKind(apImage(4096)).kind, "ap");
  const e = firmwareKind(new Uint8Array(4096)); // openesl.bin for the flasher starts with code, not a header
  assert.equal(e.kind, null);
  assert.match(e.error, /openesl\.bin is for the tag flasher/);
  assert.equal(fmtVer(10203), "1.2.3");
});

test("tag firmware goes in 16 KB parts, each after the one before", async () => {
  const ap = simAp();
  const file = tagFile(2, 290, 54272); // a whole 1.54" image
  const seen = [];
  const r = await sendTagFirmware(ap, file, (sent, total) => seen.push([sent, total]));
  assert.deepEqual(r, { model: "1.54", version: "0.2.90", size: 54272, crc: "00000000", received: file.length, total: file.length });
  assert.deepEqual(ap.done, file);
  assert.deepEqual(ap.requests.map((q) => [q.method, q.path, q.offset]), [
    ["PUT", "/api/tag-firmware", 0], ["PUT", "/api/tag-firmware", PART], ["PUT", "/api/tag-firmware", 2 * PART], ["PUT", "/api/tag-firmware", 3 * PART],
  ]);
  assert.ok(ap.requests.every((q) => q.len <= PART));
  assert.deepEqual(seen.at(-1), [file.length, file.length]);
  assert.equal(seen.length, 4);
});

test("AP firmware goes to POST /api/ota the same way", async () => {
  const ap = simAp();
  const img = apImage(1100000);
  const r = await sendApFirmware(ap, img);
  assert.equal(r.slot, "ota_1");
  assert.deepEqual(ap.done, img);
  assert.equal(ap.requests.length, Math.ceil(img.length / PART));
  assert.ok(ap.requests.every((q) => q.method === "POST" && q.path === "/api/ota"));
});

test("the AP's error stops the upload and is shown", async () => {
  const ap = simAp();
  const send = ap.send;
  ap.send = async (m, p, b) => (ap.requests.length === 1 ? { status: 422, body: enc.encode('{"error":"the image does not match the CRC in its header"}') } : send(m, p, b));
  // the second (last) part is refused, as when the CRC does not match
  await assert.rejects(sendTagFirmware(ap, tagFile(1, 300, 20480)), /422: the image does not match the CRC/);
  assert.equal(ap.requests.length, 1);
});

test("tag firmware table", () => {
  const rows = tagFirmwareRows({
    images: [
      { model: "2.9", version: null },
      { model: "1.54", version: "0.2.90", size: 54272, crc: "1234ABCD", tags: { updated: 3, pending: 2, failed: 1, no_ota: 4 } },
    ],
  });
  assert.deepEqual(rows, [
    { model: "2.9", version: null, text: "no firmware on the AP" },
    { model: "1.54", version: "0.2.90", text: "3 updated, 2 waiting, 1 failed, 4 without radio updates" },
  ]);
  assert.deepEqual(tagFirmwareRows(null), []);
});

test("after an AP update: back on the new version, back on the old one, or gone", async () => {
  const sleep = async () => {};
  const answers = (list) => {
    let i = 0;
    return { call: async () => { const a = list[i++]; if (a instanceof Error) throw a; return a; } };
  };
  let r = await waitApBack(answers([new Error("restarting"), { slot: "ota_1", version: "0.3.0" }]), "ota_1", "0.3.0", { sleep });
  assert.equal(r.ok, true);
  r = await waitApBack(answers([{ slot: "ota_0", version: "0.2.0" }]), "ota_1", "0.3.0", { sleep });
  assert.equal(r.ok, false);
  assert.match(r.text, /went back to the old one/);
  r = await waitApBack(answers([new Error("x")]), "ota_1", "0.3.0", { sleep, alive: () => false });
  assert.match(r.text, /Connect again/);
});
