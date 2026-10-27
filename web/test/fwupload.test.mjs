// Firmware from the dashboard (js/fwupload.js) against a simulated AP that checks the
// parts as esl_tagfw_part() and the AP's POST /api/ota do (docs/openapi.yaml).
import { test } from "node:test";
import assert from "node:assert/strict";
import { PART, TAGFW_PART, tagUpdate, firmwareKind, sendTagFirmware, chooseTagFirmware, fwProgress, sendApFirmware, tagFirmwareRows, waitApBack, fmtVer, cmpVersion, apImageFor, apSendQuestion, tagFirmwarePlan, radioImageFor, radioSendQuestion, waitRadioBack } from "../js/fwupload.js";

const enc = new TextEncoder();

// A tag firmware file: the 16-byte header (shared/proto.h ESL_FWF_*) and size bytes
function tagFile(model, ver, size) {
  const d = new Uint8Array(16 + size);
  d.set([0x4f, 0x53, 0x46, 0x57, 1, model, ver & 0xff, ver >> 8, size & 0xff, (size >> 8) & 0xff, size >> 16, 0]);
  for (let i = 0; i < size; i++) d[16 + i] = (i * 7) & 0xff;
  return d;
}

// An ESP32 app image: the header byte, and with version the app description after the
// image and segment headers (esp_app_desc_t: magic word, then the version at 16)
function apImage(size, version, chip = 13, project = "esl_ap") {
  const d = new Uint8Array(size);
  d[0] = 0xe9;
  d[12] = chip; // esp_image_header_t chip_id: 13 the ESP32-C6, 23 the C5
  if (version) {
    d.set([0x32, 0x54, 0xcd, 0xab], 32);
    d.set(enc.encode(version), 48);
    d.set(enc.encode(project), 80);
  }
  return d;
}

// The AP's side of an upload in parts: offset 0 starts one, every other part must go on
// from the last; the reply to the last part says what the AP now has
function simAp() {
  const ap = { requests: [], got: null, total: 0, done: null, kind: null };
  const reply = (status, obj) => ({ status, body: enc.encode(JSON.stringify(obj)) });
  ap.send = async (method, path, body) => {
    const u = new URL("http://ap" + path);
    ap.requests.push({ method, path: u.pathname, offset: Number(u.searchParams.get("offset")), to: u.searchParams.get("to"), len: body.length });
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
  assert.deepEqual(firmwareKind(tagFile(2, 290, 1024)), { kind: "tag", model: "1.54", version: "0.2.90", ver: 290, size: 1024 });
  assert.equal(firmwareKind(tagFile(1, 200, 1024)).model, "2.9");
  assert.equal(firmwareKind(tagFile(9, 200, 1024)).kind, null);
  assert.equal(firmwareKind(apImage(4096)).kind, "ap");
  const e = firmwareKind(new Uint8Array(4096)); // openesl.bin for the flasher starts with code, not a header
  assert.equal(e.kind, null);
  assert.match(e.error, /openesl\.bin is for the tag flasher/);
  assert.equal(fmtVer(10203), "1.2.3");
});

test("tag firmware goes in 8 KB parts, each after the one before", async () => {
  const ap = simAp();
  const file = tagFile(2, 290, 54272); // a whole 1.54" image
  const seen = [];
  const r = await sendTagFirmware(ap, file, (sent, total) => seen.push([sent, total]));
  assert.deepEqual(r, { model: "1.54", version: "0.2.90", size: 54272, crc: "00000000", received: file.length, total: file.length });
  assert.deepEqual(ap.done, file);
  const n = Math.ceil(file.length / TAGFW_PART);
  assert.deepEqual(ap.requests.map((q) => [q.method, q.path, q.offset]),
    Array.from({ length: n }, (_, i) => ["PUT", "/api/tag-firmware", i * TAGFW_PART]));
  assert.ok(ap.requests.every((q) => q.len <= TAGFW_PART));
  assert.deepEqual(seen.at(-1), [file.length, file.length]);
  assert.equal(seen.length, n);
});

test("tag firmware for one tag: every part names it", async () => {
  const ap = simAp();
  await sendTagFirmware(ap, tagFile(2, 290, 40000), undefined, "00000123456789A8");
  assert.equal(ap.requests.length, Math.ceil(40000 / TAGFW_PART));
  assert.ok(ap.requests.every((q) => q.to === "00000123456789A8"));
  const calls = [];
  await chooseTagFirmware({ call: async (...a) => calls.push(a) }, "1.54", "all");
  assert.deepEqual(calls, [["POST", "/api/tag-firmware/1.54?to=all"]]);
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
      { model: "1.54", version: "0.2.90", size: 54272, crc: "1234ABCD", to: ["00000123456789A8"],
        tags: { updated: 3, pending: 2, failed: 1, no_ota: 4, not_chosen: 5 }, progress: { received: 54272 * 4, total: 54272 * 5 } },
    ],
  });
  assert.deepEqual(rows, [
    { model: "2.9", version: null, text: "no firmware on the AP", to: null, progress: null, toAll: false },
    { model: "1.54", version: "0.2.90", text: "3 updated, 2 updating, 1 failed, 5 not chosen, 4 without radio updates",
      to: "1 chosen tag", progress: { fraction: 0.8, text: "80 %" }, toAll: true },
  ]);
  // for all tags, none updating; and an AP from before "to" and "progress"
  const all = { model: "2.9", version: "0.3.0", to: "all", tags: { updated: 2, pending: 0, failed: 0, no_ota: 0, not_chosen: 0 }, progress: { received: 2, total: 2 } };
  assert.deepEqual(tagFirmwareRows({ images: [all] })[0], { model: "2.9", version: "0.3.0", text: "2 updated, 0 updating, 0 failed", to: "all tags", progress: null, toAll: false });
  const waiting = { ...all, tags: { ...all.tags, pending: 1 }, progress: { received: 0, total: 1024 } };
  assert.equal(tagFirmwareRows({ images: [waiting] })[0].progress.fraction, null);
  const old = { model: "2.9", version: "0.3.0", tags: { updated: 1, pending: 1, failed: 0, no_ota: 0 } };
  assert.deepEqual(tagFirmwareRows({ images: [old] })[0], { model: "2.9", version: "0.3.0", text: "1 updated, 1 updating, 0 failed", to: "all tags", progress: null, toAll: false });
  assert.deepEqual(tagFirmwareRows(null), []);
});

test("how far a download is", () => {
  assert.deepEqual(fwProgress(0, 1024), { fraction: null, text: "waiting for its next poll" });
  assert.deepEqual(fwProgress(256, 1024), { fraction: 0.25, text: "25 %" });
  assert.deepEqual(fwProgress(1024, 1024), { fraction: null, text: "installing" });
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

test("update button for a tag with older firmware", () => {
  const imgs = { "1.54": firmwareKind(tagFile(2, 300, 1024)) };
  const tag = (extra) => ({ model: "1.54", fw: 200, ota: true, ...extra });
  assert.deepEqual(tagUpdate(tag(), imgs, null), { update: "0.3.0" });
  assert.equal(tagUpdate(tag({ fw: 300 }), imgs, null), null);           // already runs it
  assert.equal(tagUpdate(tag({ ota: false }), imgs, null), null);        // no radio updates: the jig
  assert.equal(tagUpdate(tag({ model: "2.9" }), imgs, null), null);      // nothing for its model on the page
  assert.equal(tagUpdate(tag({ fw: null }), imgs, null), null);          // not seen yet
  const offered = { images: [{ model: "1.54", version: "0.3.0" }] };          // AP firmware from before "to"
  assert.deepEqual(tagUpdate(tag(), imgs, offered), { text: "updating to 0.3.0" });
  const mac = "00000123456789A8";
  const other = { images: [{ model: "1.54", version: "0.3.0", to: ["0000000000000001"] }] };
  assert.deepEqual(tagUpdate(tag({ mac }), imgs, other), { choose: "0.3.0" });   // the AP has it, not for this tag
  const mine = { images: [{ model: "1.54", version: "0.3.0", to: [mac] }] };
  assert.deepEqual(tagUpdate(tag({ mac }), imgs, mine), { text: "0.3.0 failed" }); // for it, and no fw_update: rejected
  assert.deepEqual(tagUpdate(tag({ mac }), imgs, { images: [{ model: "1.54", version: "0.2.0", to: "all" }] }), { update: "0.3.0" });
  // fetching: how far, whatever the page has
  const fw_update = { version: "0.3.0", received: 512, size: 1024 };
  assert.deepEqual(tagUpdate(tag({ model: "2.9", fw_update }), imgs, mine), { updating: "0.3.0", fraction: 0.5, text: "50 %" });
});

test("the page's firmware for the ticked tags", () => {
  const imgs = { "1.54": firmwareKind(tagFile(2, 300, 1024)) };
  const t = (mac, extra) => ({ mac, model: "1.54", fw: 200, ota: true, ...extra });
  const tags = [t("01"), t("02"), t("03", { fw: 300 }), t("04", { ota: false }), t("05", { model: "2.9" }),
    t("06", { fw_update: { version: "0.3.0", received: 0, size: 1024 } })];
  let p = tagFirmwarePlan(tags, imgs, null);
  assert.deepEqual(p.rows.map((r) => [r.mac, r.action, r.text]), [
    ["01", "upload", "0.2.0 \u2192 0.3.0"],
    ["02", "upload", "0.2.0 \u2192 0.3.0"],
    ["03", null, "has 0.3.0 already"],
    ["04", null, "cannot update over the radio"],
    ["05", null, 'no 2.9" firmware on this page'],
    ["06", null, "updating to 0.3.0 (waiting for its next poll)"],
  ]);
  assert.deepEqual(p.upload, ["1.54"]);
  // the AP has the image already, for tag 01: the others are chosen, nothing is uploaded
  p = tagFirmwarePlan(tags.slice(0, 2), imgs, { images: [{ model: "1.54", version: "0.3.0", to: ["01"] }] });
  assert.deepEqual(p.rows.map((r) => r.action), [null, "choose"]);
  assert.deepEqual(p.upload, []);
});

test("the version in an AP image", () => {
  assert.equal(firmwareKind(apImage(4096, "0.4.3")).version, "0.4.3");
  assert.equal(firmwareKind(apImage(4096)).version, null); // no app description where it belongs
  // the radio coprocessor's: for the AP to pass on to its C6
  assert.deepEqual(firmwareKind(apImage(4096, "0.8.2", 13, "esl_radio")), { kind: "radio", size: 4096, version: "0.8.2", chip: "esp32c6" });
  // another app for the chip: neither
  const other = firmwareKind(apImage(4096, "0.5.0", 13, "esl_netradio"));
  assert.equal(other.kind, null);
  assert.match(other.error, /not AP firmware \(esl_netradio\)/);
});

test("the page's radio image for the AP's C6 radio", () => {
  const radios = [
    { key: "uart-c6", kind: "uart", display: false, ota: "radio-uart-c6-ota", version: "0.8.2" },
    { key: "uart-c6-lcd", kind: "uart", display: true, ota: "radio-uart-c6-lcd-ota", version: "0.8.2" },
    { key: "net-c6", kind: "net", display: false },
  ];
  const info = { version: "v0.8.2", release: true, radios };
  const ota = (radio) => ({ version: "0.8.2", radio });
  assert.deepEqual(radioImageFor(ota({ up: true, version: "0.8.1", ota: true, display: false }), info),
    { file: "radio-uart-c6-ota", version: "0.8.2", newer: true, display: false });
  assert.equal(radioImageFor(ota({ up: true, version: "0.8.1", ota: true, display: true }), info).file, "radio-uart-c6-lcd-ota");
  assert.equal(radioImageFor(ota({ up: true, version: "0.8.2", ota: true, display: false }), info).newer, false); // runs it
  assert.equal(radioImageFor(ota({ up: true, version: null, ota: false }), info), null);  // too old for the cable
  assert.equal(radioImageFor({ version: "0.8.2" }, info), null);                          // no radio on UART
  assert.equal(radioImageFor(ota({ up: true, version: "0.8.1", ota: true }), { ...info, radios: [radios[2]] }), null);
  assert.equal(radioImageFor(ota({ up: true, version: "0.8.1", ota: true, display: false }), { ...info, release: false }).newer, false);
});

test("what to ask before radio firmware goes out", () => {
  const kind = firmwareKind(apImage(4096, "0.8.2", 13, "esl_radio"));
  const radio = { up: true, version: "0.8.1", ota: true, display: false };
  let q = radioSendQuestion(kind, { radio }, false);
  assert.match(q, /Send radio firmware 0\.8\.2 to the AP's C6 radio\? It runs 0\.8\.1 now\./);
  assert.doesNotMatch(q, /screen/);
  assert.match(radioSendQuestion(kind, { radio }, true), /for a radio with a screen, and this radio has none/);
  assert.match(radioSendQuestion(kind, {}), /no radio on a cable/);
  assert.match(radioSendQuestion(kind, { radio: { ...radio, ota: false } }), /flash it once over USB/);
  assert.match(radioSendQuestion(kind, { radio: { ...radio, up: false } }), /does not answer the AP/);
  assert.match(radioSendQuestion(kind, { radio: { ...radio, version: "0.8.2" } }), /already runs/);
  assert.match(radioSendQuestion(firmwareKind(apImage(4096, "0.8.2", 9, "esl_radio")), { radio }), /for the ESP32-S3/);
});

test("waiting for the radio after an update", async () => {
  const sleep = async () => {};
  const answers = (list) => ({ call: async () => { const a = list.length > 1 ? list.shift() : list[0]; if (a instanceof Error) throw a; return a; } });
  const ota = (radio) => ({ slot: "ota_0", version: "0.8.2", radio });
  // the AP still reports the old radio for a moment, then the new one
  let r = await waitRadioBack(answers([ota({ up: true, version: "0.8.1" }), ota({ up: false, version: "0.8.1" }), ota({ up: true, version: "0.8.2" })]), "0.8.2", { sleep });
  assert.equal(r.ok, true);
  assert.match(r.text, /runs 0\.8\.2/);
  // back with the old version after the grace: it went back
  r = await waitRadioBack(answers([ota({ up: true, version: "0.8.1" })]), "0.8.2", { sleep, graceMs: 0 });
  assert.equal(r.ok, false);
  assert.match(r.text, /went back to the old one/);
  r = await waitRadioBack(answers([ota({ up: false, version: null })]), "0.8.2", { sleep, timeoutMs: 0 });
  assert.match(r.text, /USB port/);
  r = await waitRadioBack(answers([new Error("x")]), "0.8.2", { sleep, alive: () => false });
  assert.match(r.text, /Connect again/);
});

test("versions compare by their numbers", () => {
  assert.equal(cmpVersion("v0.4.3", "0.4.3"), 0);
  assert.ok(cmpVersion("0.4.10", "0.4.9") > 0);
  assert.ok(cmpVersion("0.4.2", "v0.5.0") < 0);
  assert.equal(cmpVersion("0.4.3-dev", "0.4.3"), 0);
  assert.equal(cmpVersion("", "0.4.3"), null);
});

test("the page's AP image that fits the AP", () => {
  const info = { version: "v0.4.4", release: true, ota: { "ap-openesl": true, "ap-openesl-lcd": true } };
  assert.deepEqual(apImageFor({ version: "0.4.3", display: true }, info), { file: "ap-openesl-lcd-ota", version: "0.4.4", newer: true, display: true });
  assert.deepEqual(apImageFor({ version: "0.4.3", display: false }, info), { file: "ap-openesl-ota", version: "0.4.4", newer: true, display: false });
  assert.equal(apImageFor({ version: "0.4.3" }, info).file, "ap-openesl-ota");        // older firmware does not say
  assert.equal(apImageFor({ version: "0.4.4", display: false }, info).newer, false);  // already runs it
  assert.equal(apImageFor({ version: "0.4.3" }, { ...info, version: "0.4.4-dev", release: false }).newer, false); // not a release
  assert.equal(apImageFor({ version: "0.4.3" }, { ...info, ota: {} }), null);
  assert.equal(apImageFor({ version: "0.4.3" }, null), null);
  // a release that changed only the tags: the AP image is still 0.4.3, so nothing new for the AP
  const parts = { ...info, version: "v0.4.4", ap_version: "0.4.3", tag_version: "0.4.4" };
  assert.deepEqual(apImageFor({ version: "0.4.3", display: false }, parts), { file: "ap-openesl-ota", version: "0.4.3", newer: false, display: false });
  assert.equal(apImageFor({ version: "0.4.2" }, parts).newer, true);
});

test("the page's AP image for the AP's chip and board (ap_boards)", () => {
  const ap_boards = [
    { id: "c6-supermini", target: "esp32c6", display: false, file: "ap-openesl" },
    { id: "c6-supermini-lcd", target: "esp32c6", display: true, file: "ap-openesl-lcd" },
    { id: "c5-wroom-1u-n16r8", target: "esp32c5", display: false, file: "ap-openesl-c5-n16r8" },
  ];
  const info = { version: "v0.4.8", release: true, ap_boards, ota: { "ap-openesl": true, "ap-openesl-lcd": true, "ap-openesl-c5-n16r8": true } };
  assert.equal(apImageFor({ version: "0.4.7", chip: "esp32c5", display: false }, info).file, "ap-openesl-c5-n16r8-ota");
  assert.equal(apImageFor({ version: "0.4.7", chip: "esp32c6", display: true }, info).file, "ap-openesl-lcd-ota");
  assert.equal(apImageFor({ version: "0.4.7", display: false }, info).file, "ap-openesl-ota");   // no chip: a C6
  assert.equal(apImageFor({ version: "0.4.7", chip: "esp32c5", display: true }, info).file, "ap-openesl-c5-n16r8-ota"); // no C5 with a screen
  assert.equal(apImageFor({ version: "0.4.7", chip: "esp32c5" }, { ...info, ota: { "ap-openesl": true } }), null); // no image for its chip
});

test("what is asked before AP firmware goes out", () => {
  const ota = { version: "0.4.3", display: false };
  let q = apSendQuestion(firmwareKind(apImage(4096, "0.4.4")), ota, false);
  assert.match(q, /Send AP firmware 0\.4\.4 to the AP\? It runs 0\.4\.3 now\./);
  assert.doesNotMatch(q, /already runs|older|status screen/);
  assert.match(apSendQuestion(firmwareKind(apImage(4096, "0.4.3")), ota), /already runs/);
  assert.match(apSendQuestion(firmwareKind(apImage(4096, "0.4.1")), ota), /older than/);
  assert.match(apSendQuestion(firmwareKind(apImage(4096, "0.4.4")), ota, true), /for an AP with a status screen/);
  assert.match(apSendQuestion(firmwareKind(apImage(4096, "0.4.4")), { ...ota, display: true }, false), /screen goes dark/);
  assert.doesNotMatch(apSendQuestion(firmwareKind(apImage(4096, "0.4.4")), { version: "0.4.3" }, true), /status screen/); // the AP does not say
  assert.match(apSendQuestion(firmwareKind(apImage(4096)), null), /^Send new firmware to the AP\?/);
  assert.equal(firmwareKind(apImage(4096, "0.4.4", 23)).chip, "esp32c5");
  assert.match(apSendQuestion(firmwareKind(apImage(4096, "0.4.4", 23)), { ...ota, chip: "esp32c6" }), /for the ESP32-C5, and this AP is an ESP32-C6/);
  assert.doesNotMatch(apSendQuestion(firmwareKind(apImage(4096, "0.4.4")), { ...ota, chip: "esp32c6" }), /refuse/);
});
