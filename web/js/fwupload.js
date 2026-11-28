// fwupload.js - new firmware for the AP and the tags from the dashboard, as `esl-client ota`
// and `esl-client tag-ota` do it (docs/openapi.yaml): POST /api/ota and PUT /api/tag-firmware,
// in parts that each get an answer before the next. No DOM here, so test/fwupload.test.mjs
// can check it against a simulated AP.

// Parts of 16 KB for AP firmware: below the AP's 24 KB body limit, and its USB driver drops
// bytes when a bigger request fills its buffer (OTA_PART in esl_client.py). The AP writes
// those to flash as they come. Tag firmware parts are 8 KB (TAGFW_PART): AP firmware from
// before it wrote those in pieces too holds a part in RAM in one block, and a 16 KB block is
// often not to be had.
export const PART = 16384;
export const TAGFW_PART = 8192;

const TAGFW_MAGIC = [0x4f, 0x53, 0x46, 0x57]; // "OSFW" (shared/proto.h ESL_FWF_MAGIC)
import { MODEL_BY_CODE as TAG_MODELS, modelName } from "./models.js";
const dec = new TextDecoder();

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

export const fmtVer = (v) => `${Math.floor(v / 10000)}.${Math.floor(v / 100) % 100}.${v % 100}`;

// What a firmware file is: { kind: "tag", model, version, size } for openesl-tag-*-ota.bin,
// { kind: "ap", size, version, chip } for an ESP32 app image (esl_ap.bin, openesl-ap-*-ota.bin),
// { kind: "radio", size, version, chip } for the radio coprocessor's (esl_radio.bin,
// openesl-ap-radio-*-ota.bin: the AP passes it on to its C6 over the cable), or
// { kind: null, error } for anything else, such as the flasher's openesl.bin or the
// AP's merged image.
export function firmwareKind(data) {
  if (data.length >= 16 && TAGFW_MAGIC.every((x, i) => data[i] === x)) {
    const model = TAG_MODELS[data[5]];
    if (!model) return { kind: null, error: `a tag firmware file for an unknown model (${data[5]})` };
    return { kind: "tag", model, version: fmtVer(u16(data, 6)), ver: u16(data, 6), size: u32(data, 8) };
  }
  // The AP checks the rest (chip, app description) and refuses what it cannot boot
  if (data.length >= 32 && data[0] === 0xe9) {
    const project = appString(data, 80);
    const chip = CHIPS[u16(data, 12)] ?? null;
    if (project === "esl_radio") return { kind: "radio", size: data.length, version: appVersion(data), chip };
    // a radio image for another link (esl_radio_usb: the C6 on a Linux AP's USB port): an S3
    // would lose its radio with it, and the AP refuses it too
    if (project?.startsWith("esl_radio"))
      return { kind: null, error: `radio firmware for another link (${project}): the C6 on a Linux AP's USB port is flashed over USB from the Flash AP page, not through an AP` };
    if (project && project !== "esl_ap")
      return { kind: null, error: `an app for the chip, but not AP firmware (${project})` };
    return { kind: "ap", size: data.length, version: appVersion(data), chip };
  }
  // The AP as a Linux program (openesl-ap-linux-<arch>-<version>): an ELF; the AP checks that it
  // is for its machine and starts, and finds its version itself
  if (data.length >= 20 && data[0] === 0x7f && data[1] === 0x45 && data[2] === 0x4c && data[3] === 0x46) {
    const arch = data[5] === 1 ? ELF_ARCHS[`${data[4]}/${u16(data, 18)}`] : undefined;
    if (!arch) return { kind: null, error: "a Linux program, but not for a machine the Linux AP is built for (x86_64, aarch64, armv7)" };
    return { kind: "linux", size: data.length, version: null, arch };
  }
  return {
    kind: null,
    error: "not a firmware file for the API. Use openesl-tag-<model>-<version>-ota.bin for tags, or openesl-ap-<board>-<version>-ota.bin for the AP; openesl.bin is for the tag flasher",
  };
}

// ELF class/e_machine of the Linux AP's builds, by the names of its release files
const ELF_ARCHS = { "2/62": "x86_64", "2/183": "aarch64", "1/40": "armv7" };
export const LINUX_ARCHS = ["aarch64", "armv7", "x86_64"];

// chip_id in the ESP32 image header, as GET /api/ota names the chip ("chip")
const CHIPS = { 9: "esp32s3", 13: "esp32c6", 23: "esp32c5" };
const chipName = (c) => c.toUpperCase().replace(/^ESP32/, "ESP32-");

// The version in an ESP32 app image (the VERSION file it was built from), or null: the image
// header (24 bytes) and the first segment's header (8) come before esp_app_desc_t, whose
// magic word is followed by the version at offset 16 and the project name at 48.
function appVersion(data) {
  return appString(data, 48);
}

// A 32-byte string of esp_app_desc_t at off (48 the version, 80 the project name: "esl_ap"
// for the AP, "esl_radio" for ap/radio/), or null
function appString(data, off) {
  if (data.length < off + 32 || u32(data, 32) !== 0xabcd5432) return null;
  const v = data.subarray(off, off + 32);
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
// one for its chip ("chip"; an AP whose firmware does not say is an ESP32-C6), with the status
// screen when the AP has one ("display"; one that does not say gets the plain one, as most
// have no screen). The boards are ap_boards in info.json; one from before that has the C6
// images only. { file, version, newer, display } or null when the page has no AP image. version is the AP image's
// own (ap_version in info.json: the release the AP last changed in), so a release that
// changed only the tags is not newer for the AP; an info.json from before it has only the
// release's version.
export function apImageFor(ota, info) {
  if (isLinuxAp(ota)) return null; // a program, not an ESP32 image: linuxImageFor
  // The Bluetooth AP (app esl_ap_ble, docs/ble-tags.md) takes only its own image, and an
  // 802.15.4 AP only its own (ota.c): ble_aps in info.json for the one, ap_boards for the other.
  // One that does not say its app (1.0.0) but has the Bluetooth AP's larger slot is a 1.0.0
  // Bluetooth AP, built as esl_ap: it would take the 802.15.4 image and lose its partition
  // table's purpose, and refuses the new Bluetooth image, so it is offered nothing (update it
  // once over USB)
  if (isBleAp(ota)) {
    if (ota.app !== BLE_APP) return null;
    const b = (info?.ble_aps || []).find((x) => info?.ota?.[x.file] && x.target === (ota.chip || "esp32c6"));
    if (!b) return null;
    const version = String(b.version || info.ap_version || info.version).replace(/^v/, "");
    const c = cmpVersion(version, ota.version);
    return { file: b.file + "-ota", version, newer: info.release === true && c !== null && c > 0, display: false, ble: true };
  }
  if (ota?.app && ota.app !== "esl_ap") return null; // an app this page has no image for
  const boards = (info?.ap_boards || [
    { file: "ap-openesl", target: "esp32c6", display: false },
    { file: "ap-openesl-lcd", target: "esp32c6", display: true },
  ]).filter((b) => info?.ota?.[b.file] && b.target === (ota?.chip || "esp32c6"));
  const b = boards.find((x) => !!x.display === (ota?.display === true)) || boards.find((x) => !x.display);
  if (!b) return null;
  const version = String(info.ap_version || info.version).replace(/^v/, "");
  const c = cmpVersion(version, ota?.version);
  return { file: b.file + "-ota", version, newer: info.release === true && c !== null && c > 0, display: !!b.display };
}

// The Bluetooth AP's app (ap/CMakeLists.txt), and its app slot on a 4 MB C6
// (ap/ble.partitions.csv: 0x1C0000; the 802.15.4 AP's is 0x180000), which tells a 1.0.0
// Bluetooth AP, which does not say its app, from an 802.15.4 one
export const BLE_APP = "esl_ap_ble";
const BLE_SLOT = 0x1c0000;
export const isBleAp = (ota) => ota?.app === BLE_APP ||
  (!ota?.app && (ota?.chip || "esp32c6") === "esp32c6" && ota?.slot_size === BLE_SLOT);

// The AP runs as a Linux program (ap/linux): GET /api/ota says slot "binary"
export const isLinuxAp = (ota) => ota?.slot === "binary";

// The Linux AP program on this page for the AP's machine (linux_ap in info.json; "arch" in GET
// /api/ota, or the arch the user picked when the AP does not say, as 0.11.0 does not):
// { file, version, size, arch, newer } or null when the page has none for it.
export function linuxImageFor(ota, info, arch = ota?.arch) {
  if (!isLinuxAp(ota) || !arch) return null;
  const b = (info?.linux_ap || []).find((x) => x.arch === arch);
  if (!b) return null;
  const version = String(b.version || info.version).replace(/^v/, "");
  const c = cmpVersion(version, ota.version);
  return { file: b.file, version, size: b.size || null, arch, newer: info.release === true && c !== null && c > 0 };
}

// Why a Linux AP cannot take an update from here ("self_update_blocked"), or null when it can
export function linuxBlocked(ota) {
  if (!isLinuxAp(ota)) return null;
  if (ota.self_update === true) return null;
  const why = ota.self_update_blocked;
  if (why === "container") return "It runs in a container: pull the new image (docker pull) and start a new container with the same data folder.";
  if (why === "unguarded") return "It was not started by openesl-ap-run, which brings the old version back if a new one fails: replace the program by hand, or start it with openesl-ap-run (Linux AP guide).";
  return "It cannot replace its own program here: replace it by hand (Linux AP guide).";
}

// What to ask before a new Linux AP program goes out: kind is firmwareKind() of the file,
// version the release's (null for a file from this computer: the AP reads it from the file)
export function linuxSendQuestion(kind, ota, version = null) {
  const lines = [];
  const running = ota?.version;
  lines.push(version && running ? `Update the AP program to ${version}? It runs ${running} now.` : `Send this Linux AP program (${kind.arch}) to the AP?` + (running ? ` It runs ${running} now.` : ""));
  if (ota?.arch && kind.arch && kind.arch !== ota.arch) lines.push(`This file is for ${kind.arch}, and this AP runs on ${ota.arch}: it will refuse it.`);
  const c = cmpVersion(version, running);
  if (c === 0) lines.push("That is the version it already runs.");
  else if (c < 0) lines.push("That is older than what it runs.");
  lines.push("The AP checks the file, keeps the program it runs now, and restarts into the new one. If the new one does not start, or does not confirm itself within a minute, the AP goes back to the old one by itself.");
  return lines.join("\n\n");
}

// What to ask before new AP firmware goes out: kind is firmwareKind() of the image, ota
// GET /api/ota, lcd whether the image is the one with the status screen (null: not known).
export function apSendQuestion(kind, ota, lcd = null) {
  const lines = [];
  const running = ota?.version;
  const c = cmpVersion(kind.version, running);
  lines.push(kind.version && running ? `Send AP firmware ${kind.version} to the AP? It runs ${running} now.` : "Send new firmware to the AP?");
  if (kind.chip && ota?.chip && kind.chip !== ota.chip)
    lines.push(`This image is for the ${chipName(kind.chip)}, and this AP is an ${chipName(ota.chip)}: it will refuse it.`);
  if (c === 0) lines.push(`That is the version it already runs.`);
  else if (c < 0) lines.push(`That is older than what it runs.`);
  if (lcd !== null && typeof ota?.display === "boolean" && lcd !== ota.display)
    lines.push(ota.display ? "This image has no status screen, and this AP has one: the screen goes dark." : "This image is for an AP with a status screen, and this AP has none.");
  lines.push("It restarts into it when the upload is done, and goes back to the old one by itself if the new one cannot be reached.");
  return lines.join("\n\n");
}

// The radio image on this page that fits the AP's C6 radio on UART ("radio" in GET /api/ota, an
// AP with a radio coprocessor), from info.json's radios: the one with the screen when the radio
// has one. { file, version, newer, display } or null when the AP has no such radio, its radio
// cannot take firmware over the cable, or the page has no image for it.
export function radioImageFor(ota, info) {
  const radio = ota?.radio;
  if (!radio?.ota) return null;
  // the S3's own C6 images, c6 or c6-lcd by the screen; any other uart radio in a page's list
  // (one for another host) is never picked, whatever the order of the list
  const want = radio.display === true ? "c6-lcd" : "c6";
  const r = (info?.radios || []).find((x) => x.kind === "uart" && x.ota && x.id === want);
  if (!r) return null;
  const version = String(r.version || info.version).replace(/^v/, "");
  const c = cmpVersion(version, radio.version);
  return { file: r.ota, version, newer: info.release === true && c !== null && c > 0, display: !!r.display };
}

// What to ask before new radio firmware goes out: kind is firmwareKind() of the image, ota
// GET /api/ota, lcd whether the image is the one with the screen (null: not known).
export function radioSendQuestion(kind, ota, lcd = null) {
  const radio = ota?.radio;
  const lines = [];
  lines.push(kind.version && radio?.version
    ? `Send radio firmware ${kind.version} to the AP's C6 radio? It runs ${radio.version} now.`
    : "Send new firmware to the AP's C6 radio?");
  if (!radio) lines.push("This AP has no radio on a cable to pass it on to: it will refuse it.");
  else if (!radio.ota) lines.push("The radio's firmware is too old to take new firmware over the cable: flash it once over USB (Flash AP).");
  else if (!radio.up) lines.push("The radio does not answer the AP right now: the AP cannot pass it on.");
  if (kind.chip && kind.chip !== "esp32c6") lines.push(`This image is for the ${chipName(kind.chip)}, and the radio is an ESP32-C6: it will refuse it.`);
  const c = cmpVersion(kind.version, radio?.version);
  if (c === 0) lines.push("That is the version it already runs.");
  else if (c < 0) lines.push("That is older than what it runs.");
  if (lcd !== null && typeof radio?.display === "boolean" && lcd !== radio.display)
    lines.push(radio.display ? "This image has no screen, and this radio has one: the screen goes dark." : "This image is for a radio with a screen, and this radio has none.");
  lines.push("The AP passes it on to the radio over the cable, and the radio restarts into it. It goes back to the old one by itself if the new one cannot reach the AP.");
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
  return uploadParts(ap, "PUT", "/api/tag-firmware", data, progress, TAGFW_PART, to ? "&to=" + encodeURIComponent(to) : "");
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

// A new Linux AP program: the whole file in one request (no offset/total; the AP writes it to disk
// as it arrives), force to install one that is not newer. Returns { version, previous,
// restarting }; throws with .status as sendPart does (409 not newer, 422 not for this machine).
export function sendLinuxAp(ap, data, force = false) {
  return sendPart(ap, "POST", "/api/ota" + (force ? "?force=1" : ""), data);
}

// After a Linux AP update: asks GET /api/ota until the new version runs and has confirmed itself
// (it does that after about 10 s up), or the old one is back (openesl-ap-run puts it back when
// the new one dies or does not confirm within a minute). The old version still answers for a
// moment before the restart, so it counts only after graceMs.
export async function waitLinuxBack(ap, version, previous, { timeoutMs = 120000, everyMs = 2000, graceMs = 8000, sleep, alive = () => true, onTrial = () => {} } = {}) {
  sleep ||= (ms) => new Promise((r) => setTimeout(r, ms));
  const start = Date.now(), end = start + timeoutMs;
  await sleep(2000);
  for (;;) {
    let o = null;
    try {
      o = await ap.call("GET", "/api/ota");
    } catch {
      /* restarting */
    }
    if (o?.version === version && o.confirmed !== false) return { ok: true, text: `The AP runs ${version} and has confirmed it. ${previous ? previous + " is kept" : "The old version is kept"} on the AP for the next rollback.` };
    if (o?.version === version) onTrial(o);
    else if (o && Date.now() - start >= graceMs)
      return { ok: false, text: `The AP is back on ${o.version}: the new version did not start or did not confirm itself in time, so the AP went back to the old one. Its log (journalctl -u openesl-ap) says why.` };
    if (!alive()) return { ok: false, text: "The connection to the AP closed while it restarted. Connect again to check its version." };
    if (Date.now() >= end) return { ok: false, text: "The AP has not answered since the restart. Connect again to check its version." };
    await sleep(everyMs);
  }
}

// New radio firmware, for the AP's C6 radio on UART: the AP passes it on as it arrives, and the
// radio restarts into it after the last part. Returns { radio: true, version, ... }.
export function sendRadioFirmware(ap, data, progress) {
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

// The page's firmware for the ticked tags (the Send card in the dashboard): one row per tag,
// { mac, model, text, action }, action "upload" when the AP needs the page's image for it,
// "choose" when the AP has it and is to give it to this tag too, null when there is nothing
// to send (text says why); and the models to upload, in the order of the tags.
export function tagFirmwarePlan(tags, pageImgs, tagfw) {
  const rows = tags.map((tag) => {
    const row = (text, action = null) => ({ mac: tag.mac, model: tag.model, text, action });
    const img = pageImgs?.[tag.model];
    const up = tagUpdate(tag, pageImgs, tagfw);
    const from = tag.fw == null ? "" : fmtVer(tag.fw) + " \u2192 ";
    if (up?.updating) return row(`updating to ${up.updating} (${up.text})`);
    if (up?.update) return row(from + up.update, "upload");
    if (up?.choose) return row(from + up.choose, "choose");
    if (up?.text) return row(up.text);
    if (!img) return row(tag.model ? `no ${modelName(tag.model)} firmware on this page` : "model not known yet");
    if (!tag.ota) return row("cannot update over the radio");
    if (tag.fw == null) return row("firmware not known yet");
    return row(`has ${fmtVer(tag.fw)} already`);
  });
  const upload = [...new Set(rows.filter((r) => r.action === "upload").map((r) => r.model))];
  return { rows, upload };
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

// After a radio update: asks GET /api/ota until the AP hears its radio again, and says whether
// it runs the new image. A radio that comes back with its old version went back to it (the new
// one did not reach the AP); one that keeps quiet past the radio's own minute to go back has
// to be looked at over USB. Right after the upload the AP may still report the old radio for a
// moment (it learns of the restart from the radio's HELLO), so an old version counts only after
// graceMs.
export async function waitRadioBack(ap, version, { timeoutMs = 90000, everyMs = 2000, graceMs = 10000, sleep, alive = () => true } = {}) {
  sleep ||= (ms) => new Promise((r) => setTimeout(r, ms));
  const start = Date.now(), end = start + timeoutMs;
  await sleep(2000); // the radio restarts and the AP configures it again
  for (;;) {
    let o = null;
    try {
      o = await ap.call("GET", "/api/ota");
    } catch {
      /* a blip on the way: ask again */
    }
    const r = o?.radio;
    if (r?.up && r.version === version) return { ok: true, text: `The radio is back and runs ${version}.` };
    if (r?.up && r.version && Date.now() - start >= graceMs)
      return { ok: false, text: `The radio is back with ${r.version}: the new firmware did not reach the AP, so the radio went back to the old one.` };
    if (!alive()) return { ok: false, text: "The connection to the AP closed while the radio restarted. Connect again to check its version." };
    if (Date.now() >= end) return { ok: false, text: "The AP has not heard from its radio since the update. Check the radio's log on its USB port." };
    await sleep(everyMs);
  }
}
