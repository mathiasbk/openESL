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
// { kind: "ap", size, version } for an ESP32 app image (esl_ap.bin, openesl-ap-*-ota.bin), or
// { kind: null, error } for anything else, such as the flasher's openesl.bin or the
// AP's merged image.
export function firmwareKind(data) {
  if (data.length >= 16 && TAGFW_MAGIC.every((x, i) => data[i] === x)) {
    const model = TAG_MODELS[data[5]];
    if (!model) return { kind: null, error: `a tag firmware file for an unknown model (${data[5]})` };
    return { kind: "tag", model, version: fmtVer(u16(data, 6)), ver: u16(data, 6), size: u32(data, 8) };
  }
  // The AP checks the rest (chip, app description) and refuses what it cannot boot
  if (data.length >= 32 && data[0] === 0xe9) return { kind: "ap", size: data.length, version: appVersion(data) };
  return {
    kind: null,
    error: "not a firmware file for the API. Use openesl-tag-<model>-<version>-ota.bin for tags, or openesl-ap-esp32c6-<version>-ota.bin for the AP; openesl.bin is for the tag flasher",
  };
}

// The version in an ESP32 app image (the VERSION file it was built from), or null: the image
// header (24 bytes) and the first segment's header (8) come before esp_app_desc_t, whose
// magic word is followed by the version at offset 16.
function appVersion(data) {
  if (data.length < 80 || u32(data, 32) !== 0xabcd5432) return null;
  const v = data.subarray(48, 80);
  const end = v.indexOf(0);
  return dec.decode(end < 0 ? v : v.subarray(0, end)) || null;
}

// Compares two versions such as "0.4.3", "v0.4.3" or "0.4.3-dev" by their numbers: < 0, 0 or
// > 0, or null when either is not one
export function cmpVersion(a, b) {
  const nums = (v) => {
    const m = /^v?(\d+)\.(\d+)\.(\d+)/.exec(String(v ?? ""));
    return m && m.slice(1).map(Number);
  };
  const x = nums(a), y = nums(b);
  if (!x || !y) return null;
  for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] - y[i];
  return 0;
}

// The AP image on this page that fits the AP, from GET /api/ota and the page's info.json:
// the one with the status screen when the AP has one ("display" in GET /api/ota; an AP
// whose firmware does not say gets the plain one, as most have no screen).
// { file, version, newer } or null when the page has no AP image.
export function apImageFor(ota, info) {
  const lcd = ota?.display === true && info?.ota?.["ap-openesl-lcd"];
  const file = lcd ? "ap-openesl-lcd-ota" : info?.ota?.["ap-openesl"] ? "ap-openesl-ota" : null;
  if (!file) return null;
  const c = cmpVersion(info.version, ota?.version);
  return { file, version: String(info.version).replace(/^v/, ""), newer: info.release === true && c !== null && c > 0 };
}

// What to ask before new AP firmware goes out: kind is firmwareKind() of the image, ota
// GET /api/ota, lcd whether the image is the one with the status screen (null: not known).
export function apSendQuestion(kind, ota, lcd = null) {
  const lines = [];
  const running = ota?.version;
  const c = cmpVersion(kind.version, running);
  lines.push(kind.version && running ? `Send AP firmware ${kind.version} to the AP? It runs ${running} now.` : "Send new firmware to the AP?");
  if (c === 0) lines.push(`That is the version it already runs.`);
  else if (c < 0) lines.push(`That is older than what it runs.`);
  if (lcd !== null && typeof ota?.display === "boolean" && lcd !== ota.display)
    lines.push(ota.display ? "This image has no status screen, and this AP has one: the screen goes dark." : "This image is for an AP with a status screen, and this AP has none.");
  lines.push("It restarts into it when the upload is done, and goes back to the old one by itself if the new one cannot be reached.");
  return lines.join("\n\n");
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
// query: more of the query string for every part, such as "&to=...".
export async function uploadParts(ap, method, path, data, progress = () => {}, part = PART, query = "") {
  const total = data.length;
  let reply = null;
  for (let off = 0; off < total; off += part) {
    const chunk = data.subarray(off, Math.min(off + part, total));
    reply = await sendPart(ap, method, `${path}?offset=${off}&total=${total}${query}`, chunk);
    progress(Math.min(off + part, total), total);
  }
  return reply;
}

// New tag firmware: the AP keeps it and offers it to the tags of its model, or with to (a
// tag's MAC) to that tag alone. Returns { model, version, size, crc, ... }.
export function sendTagFirmware(ap, data, progress, to = null) {
  return uploadParts(ap, "PUT", "/api/tag-firmware", data, progress, PART, to ? "&to=" + encodeURIComponent(to) : "");
}

// The image the AP has for a model goes to one more tag (its MAC), or to all of them ("all").
// Returns GET /api/tag-firmware/{model}.
export function chooseTagFirmware(ap, model, to) {
  return ap.call("POST", `/api/tag-firmware/${encodeURIComponent(model)}?to=${encodeURIComponent(to)}`);
}

// The AP's image is for this tag ("to" in GET /api/tag-firmware)
const isFor = (img, mac) => img?.to === "all" || (Array.isArray(img?.to) && img.to.includes(mac));

// How far a download is: { fraction, text }, fraction null while nothing has arrived
export function fwProgress(received, size) {
  if (!size) return { fraction: null, text: "waiting" };
  if (received >= size) return { fraction: null, text: "installing" };
  if (!received) return { fraction: null, text: "waiting for its next poll" };
  const fraction = received / size;
  return { fraction, text: `${Math.floor(fraction * 100)} %` };
}

// New AP firmware: written to the other app slot; the AP restarts into it a second after
// the last part. Returns { slot, version, ... }.
export function sendApFirmware(ap, data, progress) {
  return uploadParts(ap, "POST", "/api/ota", data, progress);
}

// For a tag in the dashboard's table, or null when there is nothing to say:
// { updating: version, fraction, text } while it fetches the AP's image (fw_update in GET
// /api/tags), { update: version } when the page has newer firmware for its model and the AP
// does not have it yet, { choose: version } when the AP has it but not for this tag, and
// { text } when it is for the tag and the tag rejected it. pageImgs: model -> firmwareKind()
// of the page's file; tagfw: GET /api/tag-firmware.
export function tagUpdate(tag, pageImgs, tagfw) {
  const u = tag.fw_update;
  if (u) return { updating: u.version, ...fwProgress(u.received, u.size) };
  const img = pageImgs?.[tag.model];
  if (!tag.ota || !img || tag.fw == null || tag.fw >= img.ver) return null;
  const onAp = (tagfw?.images || []).find((i) => i.model === tag.model);
  if (onAp?.version !== img.version) return { update: img.version };
  // AP firmware from before "to" and fw_update: it offers the image to every tag, and says no more
  if (!onAp.to) return { text: `updating to ${img.version}` };
  // fw_update is null for a tag the image is for: it rejected it
  if (isFor(onAp, tag.mac)) return { text: `${img.version} failed` };
  return { choose: img.version };
}

// Rows for the tag firmware table from GET /api/tag-firmware
// { model, version, text, to, progress, toAll }: to says which tags the image is for,
// progress how far they are together (fwProgress, or null when none is updating), toAll
// whether other tags of the model could still get it.
export function tagFirmwareRows(reply) {
  return (reply?.images || []).map((i) => {
    const t = i.tags || {};
    if (!i.version) return { model: i.model, version: null, text: "no firmware on the AP", to: null, progress: null, toAll: false };
    const p = i.progress;
    return {
      model: i.model,
      version: i.version,
      text:
        `${t.updated || 0} updated, ${t.pending || 0} updating, ${t.failed || 0} failed` +
        (t.not_chosen ? `, ${t.not_chosen} not chosen` : "") +
        (t.no_ota ? `, ${t.no_ota} without radio updates` : ""),
      to: Array.isArray(i.to) ? `${i.to.length} chosen ${i.to.length === 1 ? "tag" : "tags"}` : "all tags",
      progress: !t.pending || !p?.total ? null : p.received ? fwProgress(p.received, p.total) : { fraction: null, text: "waiting for their polls" },
      toAll: Array.isArray(i.to) && !!t.not_chosen,
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
