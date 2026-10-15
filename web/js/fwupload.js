// fwupload.js - new firmware for the AP and the tags from the dashboard, as `esl-client ota`
// and `esl-client tag-ota` do it (docs/openapi.yaml): POST /api/ota and PUT /api/tag-firmware,
// in parts that each get an answer before the next. No DOM here, so test/fwupload.test.mjs
// can check it against a simulated AP.

// Parts of 16 KB: below the AP's 24 KB body limit, and its USB driver drops bytes when a
// bigger request fills its buffer (OTA_PART and TAGFW_PART in esl_client.py)
export const PART = 16384;

const TAGFW_MAGIC = [0x4f, 0x53, 0x46, 0x57]; // "OSFW" (shared/proto.h ESL_FWF_MAGIC)
const TAG_MODELS = { 1: "2.9", 2: "1.54" };
const dec = new TextDecoder();

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

export const fmtVer = (v) => `${Math.floor(v / 10000)}.${Math.floor(v / 100) % 100}.${v % 100}`;

// What a firmware file is: { kind: "tag", model, version, size } for openesl-tag-*-ota.bin,
// { kind: "ap", size } for an ESP32 app image (esl_ap.bin, openesl-ap-*-ota.bin), or
// { kind: null, error } for anything else, such as the flasher's openesl.bin or the
// AP's merged image.
export function firmwareKind(data) {
  if (data.length >= 16 && TAGFW_MAGIC.every((x, i) => data[i] === x)) {
    const model = TAG_MODELS[data[5]];
    if (!model) return { kind: null, error: `a tag firmware file for an unknown model (${data[5]})` };
    return { kind: "tag", model, version: fmtVer(u16(data, 6)), ver: u16(data, 6), size: u32(data, 8) };
  }
  // The AP checks the rest (chip, app description) and refuses what it cannot boot
  if (data.length >= 32 && data[0] === 0xe9) return { kind: "ap", size: data.length };
  return {
    kind: null,
    error: "not a firmware file for the API. Use openesl-tag-<model>-<version>-ota.bin for tags, or openesl-ap-esp32c6-<version>-ota.bin for the AP; openesl.bin is for the tag flasher",
  };
}

// One request with a binary body through anything with send(method, path, bytes) that
// answers { status, body } (SerialAp in js/apserial.js, ApClient in js/apclient.js)
async function sendPart(ap, method, path, part) {
  const r = await ap.send(method, path, part);
  let json = null;
  try {
    json = JSON.parse(dec.decode(r.body));
  } catch {
    /* empty or not JSON */
  }
  if (r.status >= 400) {
    const e = new Error(`${r.status}: ${json?.error || "error"}`);
    e.status = r.status;
    throw e;
  }
  return json;
}

// Sends data in parts: a part at offset 0 starts the upload, every other part goes on from
// the last. progress(sent, total) after each. Returns the AP's answer to the last part.
export async function uploadParts(ap, method, path, data, progress = () => {}, part = PART) {
  const total = data.length;
  let reply = null;
  for (let off = 0; off < total; off += part) {
    const chunk = data.subarray(off, Math.min(off + part, total));
    reply = await sendPart(ap, method, `${path}?offset=${off}&total=${total}`, chunk);
    progress(Math.min(off + part, total), total);
  }
  return reply;
}

// New tag firmware: the AP keeps it and offers it to the tags of its model.
// Returns { model, version, size, crc, ... }.
export function sendTagFirmware(ap, data, progress) {
  return uploadParts(ap, "PUT", "/api/tag-firmware", data, progress);
}

// New AP firmware: written to the other app slot; the AP restarts into it a second after
// the last part. Returns { slot, version, ... }.
export function sendApFirmware(ap, data, progress) {
  return uploadParts(ap, "POST", "/api/ota", data, progress);
}

// For a tag in the dashboard's table: null when there is nothing to offer it, { update: version }
// when the page has newer firmware for its model than it runs and it takes firmware over the
// radio, or { text } when the AP already offers that version. pageImgs: model -> firmwareKind()
// of the page's file; tagfw: GET /api/tag-firmware.
export function tagUpdate(tag, pageImgs, tagfw) {
  const img = pageImgs?.[tag.model];
  if (!tag.ota || !img || tag.fw == null || tag.fw >= img.ver) return null;
  const onAp = (tagfw?.images || []).find((i) => i.model === tag.model);
  if (onAp?.version === img.version) return { text: `updating to ${img.version}` };
  return { update: img.version };
}

// Rows for the tag firmware table from GET /api/tag-firmware
export function tagFirmwareRows(reply) {
  return (reply?.images || []).map((i) => {
    const t = i.tags || {};
    return {
      model: i.model,
      version: i.version || null,
      text: i.version
        ? `${t.updated || 0} updated, ${t.pending || 0} waiting, ${t.failed || 0} failed` +
          (t.no_ota ? `, ${t.no_ota} without radio updates` : "")
        : "no firmware on the AP",
    };
  });
}

// After an AP update: asks GET /api/ota until the AP answers again, and says whether it
// runs the new image (as wait_back in esl_client.py). sleep is replaceable for the tests.
// alive() false (the page closed the connection meanwhile) ends the wait at once.
export async function waitApBack(ap, slot, version, { timeoutMs = 90000, everyMs = 2000, sleep, alive = () => true } = {}) {
  sleep ||= (ms) => new Promise((r) => setTimeout(r, ms));
  const end = Date.now() + timeoutMs;
  await sleep(3000); // the restart itself takes a few seconds
  for (;;) {
    let o = null;
    try {
      o = await ap.call("GET", "/api/ota");
    } catch {
      /* still restarting */
    }
    if (o) {
      if (o.slot === slot && o.version === version) return { ok: true, text: `The AP is back and runs ${version}.` };
      return {
        ok: false,
        text: `The AP is back on ${o.slot} with ${o.version}: the new firmware did not confirm itself, so the AP went back to the old one.`,
      };
    }
    if (!alive()) return { ok: false, text: "The connection to the AP closed while it restarted. Connect again to check its version." };
    if (Date.now() >= end) return { ok: false, text: "The AP has not answered since the restart. Connect again to check its version." };
    await sleep(everyMs);
  }
}
