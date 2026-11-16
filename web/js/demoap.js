// demoap.js - a pretend AP inside the page, for the demo on GitHub Pages (?demo=browser): six
// tags and the AP's HTTP API (docs/openapi.yaml) answered in JavaScript, so the dashboard and
// the designer can be tried without any hardware. Each screen is drawn by the AP's own code
// (js/renderer.js, a renderer of its own per tag model), so what the demo shows is what a tag
// would show. Nothing leaves the browser, and a reload starts over.
//
//   const ap = new DemoAp();                 // the same call() and send() as ApClient
//   await ap.call("GET", "/api/tags");
//
// The demo AP sets the shortest poll interval (10 s; a real tag polls once a minute), so new
// content goes from "waiting" to "shown" while one watches. Firmware, fonts, WiFi and data sources are not
// pretended: those calls answer as an AP without them does.
// main.js makes one when the page is opened with ?demo=browser and hands it to the dashboard
// and the designer. tools/demo.py is the full simulator instead (the AP's C code and real tag
// firmware), through the esl-client bridge.

import { MODELS, layoutModel } from "./models.js";
import { newRenderer, LAYOUT_PRICE, LAYOUT_TEXT, LAYOUT_FILE, FLAG_PRICE_HIGHLIGHT, FLAG_FRAME } from "./renderer.js";
import { packRaw } from "./sendpic.js";

// The query that starts the demo: ?demo=browser (?demo alone, or with an address, is the bridge
// of tools/demo.py)
export function demoWanted(search = globalThis.location?.search || "") {
  return new URLSearchParams(search).get("demo") === "browser";
}

// Storage that forgets: in the demo, localStorage and sessionStorage are this, so the demo's
// drafts, picks and settings never touch what the page keeps for real use (the designer's
// drafts, the dashboard's Send text). seed: keys copied in at the start.
export function memoryStorage(seed = {}) {
  const m = new Map(Object.entries(seed));
  return {
    get length() { return m.size; },
    key: (i) => [...m.keys()][i] ?? null,
    getItem: (k) => (m.has(String(k)) ? m.get(String(k)) : null),
    setItem: (k, v) => { m.set(String(k), String(v)); },
    removeItem: (k) => { m.delete(String(k)); },
    clear: () => m.clear(),
  };
}

// Puts memoryStorage() in place of the browser's storage for this page. Runs when this module
// is loaded, so main.js imports it first, before any module reads storage. The light/dark choice
// is copied in, so the demo looks as the page does.
function forgetStorage() {
  let theme = null;
  try { theme = localStorage.getItem("openesl.theme"); } catch { /* blocked */ }
  for (const [name, seed] of [["localStorage", theme ? { "openesl.theme": theme } : {}], ["sessionStorage", {}]])
    Object.defineProperty(globalThis, name, { value: memoryStorage(seed), configurable: true });
}
if (demoWanted()) forgetStorage();

export const DEMO_FW = "demo";
const TAG_FW = 1000;                    // 0.10.0, as fmtTagFw() shows it
const DEMO_POLL_S = 10;                 // the AP's poll interval in the demo (a real tag's default is 60 s)
const LOW_BATTERY_MV = 2500;            // ESL_HEALTH_BATTERY_MV_DEFAULT
const MAX_TAGS = 400;
const MAX_SCHEDULED = 16;
const REPEAT_S = { daily: 86400, weekly: 7 * 86400 };
// A recurring entry's first time after now
function nextTime(at, repeat, now) {
  const p = REPEAT_S[repeat];
  return at > now ? at : at + (Math.floor((now - at) / p) + 1) * p;
}
const MAX_LAYOUT_JSON = 8192;
const MAX_BATCH = 50;
const BUILTIN = { price: ["name", "price", "subtitle"], text: ["line1", "line2", "line3", "line4"] };
const CONFIG_KEYS = ["poll_s", "max_poll_s", "lost_ap_s", "long_poll_s", "long_after_min"];
// GET /api/fonts on an AP without font files (the built-in fonts of the AP's renderer)
const BUILTIN_FONTS = [
  { name: "small", height: 17, baseline: 13, first: 32, last: 255 },
  { name: "medium", height: 24, baseline: 19, first: 32, last: 255 },
  { name: "large", height: 52, baseline: 42, first: 32, last: 58 },
  { name: "mid", height: 36, baseline: 29, first: 32, last: 58 },
  { name: "text20", height: 20, baseline: 16, first: 32, last: 255 },
  { name: "price44", height: 44, baseline: 35, first: 32, last: 58 },
];

// The six tags: a small shop and an office. Made-up MACs (byte 6 is the model's, as on a real
// tag). One is new and has no content yet; one has a low battery; the 1.54" cannot measure its.
export const DEMO_TAGS = [
  { mac: "00000DE300013B01", model: "2.9", battery_mv: 2980, rssi: -52,
    content: { layout: "shelf", fields: ["Whole milk 1 L", "24.90", "NOK 24.90/L", "Organic, from a local farm"] } },
  { mac: "00000DE300023B02", model: "2.9", battery_mv: 2910, rssi: -61,
    content: { layout: "price", fields: ["Rye bread", "39.00", "NOK 52.00/kg"], price_highlight: true } },
  { mac: "00000DE300033B03", model: "2.9", battery_mv: 2390, rssi: -74,
    content: { layout: "sign", fields: ["Meeting room 2", "Free until 14:00"] } },
  { mac: "00000DE300043403", model: "1.54", battery_mv: 0, rssi: -58,
    content: { layout: "price", fields: ["Coffee 500 g", "79.90", "NOK 159.80/kg"] } },
  { mac: "00000DE300053404", model: "1.54", battery_mv: 0, rssi: -80, content: null },
  { mac: "00000DE300063C01", model: "2.9-bwr", battery_mv: 2840, rssi: -66,
    content: { layout: "shelf", fields: ["Apples", "29.90", "NOK 29.90/kg", "Offer this week"] } },
];

class ApiError extends Error {
  constructor(status, error, extra = {}) {
    super(`${status}: ${error}`);
    this.status = status;
    this.body = { error, ...extra };
  }
}
const fail = (status, error, extra) => { throw new ApiError(status, error, extra); };

let crcTable = null;
export function crc32(bytes) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let c = 0xffffffff;
  for (const b of bytes) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
const crcOf = (v) => crc32(new TextEncoder().encode(typeof v === "string" ? v : JSON.stringify(v))) || 1;

function base64(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

const rawSize = (m) => Math.ceil(m.height / 8) * m.width * (m.colors.includes("red") ? 2 : 1);

export class DemoAp {
  // now: the clock (tests pass their own); tags: the tags to start with (DEMO_TAGS);
  // layoutFile(model): the layout file the AP starts with for a model, or null; wasm: where
  // the renderer is (the node tests give dist/wasm/)
  constructor({ now = () => Date.now(), tags = DEMO_TAGS, layoutFile = fetchExample, wasm } = {}) {
    this.now = now;
    this.wasm = wasm;
    this.base = "";
    this.token = "";
    const t0 = now();
    this.startedAt = t0 - (3 * 86400 + 4 * 3600 + 17 * 60) * 1000;    // up for three days
    this.config = { poll_s: DEMO_POLL_S, builtin_poll_s: 0, webhook: null, channel: 25, builtin_channel: 25, channel_next: null,
                    channel_old_tags: 0, channel_unheard_tags: 0, channel_radio_old: false, channel_stopped: 0,
                    night: null, update_window: null, night_now: false, update_window_open: true };
    this.files = {};          // model -> the layout file as uploaded (only models that own a file)
    this.renderers = {};      // model -> Promise of its renderer
    this.schedule = [];       // [{ mac, at, body }], sorted by at
    this.all = tags.map((d, i) => this.newTag(d.mac, d.model, d, t0 - (i + 1) * 900));
    this.ready = this.start(tags, layoutFile);
  }

  newTag(mac, model, d = {}, lastSeen = this.now()) {
    const tag = {
      mac, model, battery_mv: d.battery_mv ?? 2900, baseRssi: d.rssi ?? -60, fw: TAG_FW,
      lastSeen, nextPoll: 0, polls: 4000 + Math.floor(Math.random() * 3000),
      rssi: [], lost: [], retry: [], lostTotal: 0, retries: 0,
      content: null, contentId: 0, shownContentId: 0, image: null, imageId: 0, shownImageId: 0, contentError: null,
      config: Object.fromEntries(CONFIG_KEYS.map((k) => [k, null])), configPending: false, deleted: false,
    };
    tag.nextPoll = lastSeen + this.interval(tag);
    for (let i = 0; i < 8; i++) this.hear(tag);
    return tag;
  }

  async start(tags, layoutFile) {
    for (const m of Object.keys(MODELS)) {
      if (layoutModel(m) !== m) continue;
      const file = await layoutFile(m);
      if (file) await this.storeLayouts(m, file);
    }
    for (const d of tags) {
      if (!d.content) continue;
      const t = this.find(d.mac);
      await this.setContent(t, d.content);
      t.shownContentId = t.contentId;   // shown long ago
      t.shownImageId = t.imageId;
    }
  }

  // ---- the API ----

  async call(method, path, body) {
    try {
      return await this.handle(method, path, body);
    } catch (e) {
      if (e instanceof ApiError) throw e;
      throw new ApiError(500, e.message || String(e));
    }
  }

  // The shortcuts ApClient has (js/apclient.js), which the designer uses
  tags() { return this.call("GET", "/api/tags"); }
  layouts() { return this.call("GET", "/api/layouts"); }
  putLayouts(model, file) { return this.call("PUT", "/api/layouts/" + encodeURIComponent(model), file); }
  putContent(mac, content) { return this.call("PUT", "/api/tags/" + encodeURIComponent(mac), content); }
  tag(mac) { return this.call("GET", "/api/tags/" + encodeURIComponent(mac)); }

  // A binary body (PUT /api/tags/{mac}/image, firmware): { status, body } as ApClient.send
  async send(method, path, bytes) {
    const out = (status, obj) => ({ status, body: new TextEncoder().encode(JSON.stringify(obj)) });
    try {
      return out(200, await this.handleBinary(method, path, new Uint8Array(bytes)));
    } catch (e) {
      return e instanceof ApiError ? out(e.status, e.body) : out(500, { error: e.message || String(e) });
    }
  }

  async handle(method, fullPath, body) {
    await this.ready;
    await this.tick();
    const path = fullPath.replace(/\?.*$/, "");
    const p = path.split("/").slice(1).map(decodeURIComponent);   // ["api", "tags", mac, ...]
    if (p[0] !== "api") fail(404, "not found");
    const route = `${method} /${p.slice(0, 2).join("/")}${p.length > 2 ? "/*" : ""}${p.length > 3 ? "/" + p.slice(3).join("/") : ""}`;
    switch (route) {
      case "GET /api/tags": return { ap: this.apInfo(), tags: this.live().map((t) => this.row(t)) };
      case "PUT /api/tags": return this.putBatch(body);
      case "GET /api/tags/*": return this.detail(this.tagOr404(p[2]));
      case "PUT /api/tags/*": return this.putOne(this.tagOr404(p[2]), body);
      case "DELETE /api/tags/*": {
        const t = this.tagOr404(p[2]);
        Object.assign(t, { deleted: true, content: null, contentId: 0, image: null, imageId: 0, contentError: null });
        t.config = Object.fromEntries(CONFIG_KEYS.map((k) => [k, null]));
        this.schedule = this.schedule.filter((e) => e.mac !== t.mac);
        return { mac: t.mac, deleted: true };
      }
      case "GET /api/tags/*/image": return this.imageOf(this.tagOr404(p[2]));
      case "GET /api/tags/*/config": return this.configOf(this.tagOr404(p[2]));
      case "PUT /api/tags/*/config": return this.putConfig(this.tagOr404(p[2]), body);
      case "GET /api/health": return this.health();
      case "GET /api/config": return { ...this.config };
      case "PUT /api/config": return this.putApConfig(body);
      case "GET /api/time": return { time: Math.floor(this.now() / 1000), source: "ntp" };
      case "PUT /api/time": return { time: Math.floor(this.now() / 1000), source: "ntp" };
      case "GET /api/layouts": return this.layoutList();
      case "GET /api/layouts/*": return this.fileOf(p[2]);
      case "PUT /api/layouts/*": return this.storeLayouts(p[2], body);
      case "DELETE /api/layouts/*": return this.deleteLayouts(p[2]);
      case "GET /api/fonts": return { max_fonts: 8, max_bytes: 32768, bytes: 0, builtin: BUILTIN_FONTS, fonts: [] };
      case "GET /api/fonts/*": return fail(404, `no font file "${p[2]}"`);
      case "PUT /api/fonts/*": return fail(501, "the demo AP keeps no font files");
      case "GET /api/tag-firmware":
        return { max_size: 61456, part_max: 24576, images: Object.keys(MODELS).map((model) => ({ model, version: null })) };
      case "GET /api/ota": return fail(501, "no OTA in the browser demo");
      default: return fail(404, "not found");
    }
  }

  async handleBinary(method, fullPath, bytes) {
    await this.ready;
    await this.tick();
    const p = fullPath.replace(/\?.*$/, "").split("/").slice(1).map(decodeURIComponent);
    if (method === "PUT" && p[0] === "api" && p[1] === "tags" && p.length === 4 && p[3] === "image")
      return this.putImage(this.tagOr404(p[2]), bytes);
    if (p[1] === "tag-firmware" || p[1] === "ota") fail(501, "the browser demo takes no firmware");
    return fail(404, "not found");
  }

  // ---- time: the tags poll ----

  // How long until a tag polls again: its own interval, the AP's, or the tag's default (60 s),
  // a little early rather than late
  interval(t) {
    return (t.config.poll_s || this.config.poll_s || 60) * 1000 * (0.9 + Math.random() * 0.1);
  }

  hear(t) {
    const push = (a, v) => { a.push(v); if (a.length > 8) a.shift(); };
    const retry = Math.random() < (t.baseRssi < -75 ? 0.15 : 0.02) ? 1 : 0;
    push(t.rssi, Math.round(t.baseRssi + (Math.random() - 0.5) * 6));
    push(t.lost, 0);
    push(t.retry, retry);
    t.retries += retry;
  }

  async tick() {
    const now = this.now();
    // scheduled content whose time has come
    while (this.schedule.length && this.schedule[0].at * 1000 <= now) {
      const e = this.schedule.shift();
      const t = this.find(e.mac);
      if (t) await this.setContent(t, e.body).catch(() => {});
      if (e.repeat) { // daily or weekly (#415): on to its next time (the demo has no daylight saving time)
        e.at = nextTime(e.at, e.repeat, Math.floor(now / 1000));
        this.schedule.push(e);
        this.schedule.sort((a, b) => a.at - b.at);
      }
    }
    for (const t of this.all) {
      while (t.nextPoll <= now) {
        t.lastSeen = t.nextPoll;
        t.nextPoll += this.interval(t);
        t.polls++;
        this.hear(t);
        if (t.deleted) t.deleted = false;           // it polls again: back, without content
        if (!t.contentError) {
          t.shownContentId = t.contentId;
          t.shownImageId = t.imageId;
        }
        t.configPending = false;
      }
    }
  }

  // ---- tags ----

  find(mac) { return this.all.find((t) => t.mac === String(mac).toUpperCase() && !t.deleted); }
  live() { return this.all.filter((t) => !t.deleted); }
  tagOr404(mac) {
    if (!/^[0-9A-Fa-f]{16}$/.test(mac)) fail(400, "bad MAC");
    return this.find(mac) || fail(404, "unknown tag");
  }

  apInfo() {
    return {
      fw: DEMO_FW, board: "demo", board_name: "Demo AP",
      uptime_s: Math.floor((this.now() - this.startedAt) / 1000), channel: this.config.channel,
      max_tags: MAX_TAGS, table_full: false, poll_s: this.config.poll_s, reset: "poweron",
      time: Math.floor(this.now() / 1000), scheduled: this.schedule.length, max_scheduled: MAX_SCHEDULED,
    };
  }

  row(t) {
    const r = {
      mac: t.mac, seen_s_ago: Math.max(0, Math.floor((this.now() - t.lastSeen) / 1000)), battery_mv: t.battery_mv,
      rssi: t.rssi[t.rssi.length - 1], fw: t.fw, model: t.model, draw_failed: false,
      content_id: t.contentId, shown_content_id: t.shownContentId, pending: t.contentId !== t.shownContentId,
      poll_s: t.config.poll_s ?? (this.config.poll_s || null), config: { ...t.config }, config_pending: t.configPending,
      content_error: t.contentError, ota: true,
      signal: { rssi: [...t.rssi], lost: [...t.lost], retry: [...t.retry], polls: t.polls, lost_total: t.lostTotal,
                retries: t.retries, tag_rssi: 200, tag_lqi: 110 },
      image: true, image_id: t.content ? t.imageId : null, shown_image_id: t.shownImageId, fw_update: null,
    };
    if (t.content) r.layout = t.content.layout;
    return r;
  }

  detail(t) {
    const d = this.row(t);
    if (t.content && t.content.layout !== "image") {
      d.fields = [...t.content.fields];
      d.price_highlight = !!t.content.price_highlight;
      d.frame = !!t.content.frame;
    }
    d.scheduled = this.schedule.filter((e) => e.mac === t.mac).map((e) => ({ at: e.at, ...e.body }));
    return d;
  }

  health() {
    const problems = [];
    const count = { silent: 0, unseen: 0, draw_failed: 0, low_battery: 0, content_error: 0 };
    for (const t of this.live()) {
      const p = [];
      if (t.battery_mv && t.battery_mv < LOW_BATTERY_MV) p.push("low_battery");
      if (t.contentError) p.push("content_error");
      for (const k of p) count[k]++;
      if (p.length) problems.push({ mac: t.mac, problems: p, seen_s_ago: this.row(t).seen_s_ago, battery_mv: t.battery_mv });
    }
    return { problems, radio_problems: [], ok: !problems.length, tags: this.live().length, with_problems: problems.length,
             radio_lost: 0, ...count };
  }

  configOf(t) { return { mac: t.mac, poll_s: this.row(t).poll_s, config: { ...t.config }, config_pending: t.configPending }; }

  putConfig(t, body) {
    if (!body || typeof body !== "object") fail(400, "the body is not a JSON object");
    const next = { ...t.config };
    for (const [k, v] of Object.entries(body)) {
      if (!CONFIG_KEYS.includes(k)) fail(422, `unknown setting "${k}"`);
      if (v !== null && !Number.isInteger(v)) fail(422, `${k} must be a whole number or null`);
      next[k] = v;
    }
    const changed = JSON.stringify(next) !== JSON.stringify(t.config);
    t.config = next;
    if (changed) t.configPending = true;
    return { ...this.configOf(t), changed };
  }

  putApConfig(body) {
    if (!body || typeof body !== "object") fail(400, "the body is not a JSON object");
    if ("poll_s" in body) {
      const v = body.poll_s;
      if (!Number.isInteger(v) || (v !== 0 && (v < 10 || v > 3600))) fail(422, "poll_s must be 0 or 10 to 3600");
      this.config.poll_s = v;
    }
    if ("webhook" in body) this.config.webhook = body.webhook || null;
    // night mode and update window (#415): kept and shown, not acted on (the demo's tags never sleep)
    const hhmm = (v) => typeof v === "string" && /^([01]\d|2[0-3]):[0-5]\d$/.test(v);
    for (const k of ["night", "update_window"]) {
      if (!(k in body)) continue;
      const v = body[k];
      if (v !== null && (!v || !hhmm(v.from) || !hhmm(v.to) || v.from === v.to ||
          (k === "night" && (!Number.isInteger(v.poll_s) || v.poll_s < 60 || v.poll_s > 3600))))
        fail(422, `${k} must be null or {from, to${k === "night" ? ", poll_s" : ""}}`);
      this.config[k] = v && (k === "night" ? { from: v.from, to: v.to, poll_s: v.poll_s } : { from: v.from, to: v.to });
    }
    return { ...this.config };
  }

  // ---- content ----

  // The content as the AP keeps it, checked, with its image: { content, image } or throws 422
  async prepare(t, body) {
    if (!body || typeof body !== "object") fail(400, "the body is not a JSON object");
    const layout = body.layout;
    if (typeof layout !== "string" || !layout) fail(422, "layout is missing");
    const file = this.files[layoutModel(t.model)];
    const names = BUILTIN[layout] || file?.layouts.find((l) => l.name === layout)?.fields;
    if (!names) fail(422, `layout "${layout}" is not in the layout file for model ${t.model}`);
    let fields = body.fields;
    if (Array.isArray(fields)) {
      if (fields.length > names.length) fail(422, `layout "${layout}" takes at most ${names.length} fields`);
    } else if (fields && typeof fields === "object") {
      for (const k of Object.keys(fields)) if (!names.includes(k)) fail(422, `layout "${layout}" has no field "${k}"`);
      fields = names.map((n) => fields[n]);
      while (fields.length > 1 && fields[fields.length - 1] == null) fields.pop();
    } else fail(422, "fields is missing");
    fields = fields.map((f) => (f == null ? "" : String(f)));
    const content = { layout, fields };
    if (body.price_highlight) content.price_highlight = true;
    if (body.frame) content.frame = true;
    const image = await this.draw(t.model, content);
    if (image.error) fail(422, image.error);
    return { content, image: image.raw };
  }

  // Draws content with the AP's renderer: { raw } or { error }
  async draw(model, content) {
    const r = await this.renderer(model);
    const m = MODELS[model];
    let n;
    if (content.layout === "price") n = LAYOUT_PRICE;
    else if (content.layout === "text") n = LAYOUT_TEXT;
    else {
      const i = this.files[layoutModel(model)]?.layouts.findIndex((l) => l.name === content.layout) ?? -1;
      if (i < 0) return { error: `layout "${content.layout}" is not in the layout file for model ${model}` };
      n = LAYOUT_FILE + i;
    }
    const flags = (content.price_highlight ? FLAG_PRICE_HIGHLIGHT : 0) | (content.frame ? FLAG_FRAME : 0);
    const res = r.render(n, flags, content.fields);
    if (!res.ok) return { error: "cannot draw it: " + res.error };
    if (res.refused) return { error: res.refused };
    return { raw: packRaw(res.pixels, m.width, m.height, m.colors.includes("red")) };
  }

  renderer(model) {
    if (!this.renderers[model]) this.renderers[model] = newRenderer(model, this.wasm);
    return this.renderers[model];
  }

  async setContent(t, body) {
    const { content, image } = await this.prepare(t, body);
    return this.apply(t, content, image, crcOf([content.layout, content.fields, !!content.price_highlight, !!content.frame]));
  }

  apply(t, content, image, contentId) {
    const changed = contentId !== t.contentId || !!t.contentError;
    if (changed) {
      t.content = content;
      t.contentId = contentId;
      t.image = image;
      t.imageId = crc32(image) || 1;
      t.contentError = null;
    }
    return { mac: t.mac, content_id: t.contentId, changed, pending: t.contentId !== t.shownContentId };
  }

  async putOne(t, body) {
    if (body?.at != null) return this.scheduleOne(t, body);
    return this.setContent(t, body);
  }

  async scheduleOne(t, body) {
    let { at, repeat, ...rest } = body;
    if (!Number.isInteger(at) || at < 1600000000) fail(422, "at must be Unix seconds");
    if (repeat === "once") repeat = null;
    if (repeat != null && !REPEAT_S[repeat]) fail(422, 'repeat must be null, "once", "daily" or "weekly"');
    await this.prepare(t, rest);     // checked now, drawn again when it is due
    if (repeat) at = nextTime(at, repeat, Math.floor(this.now() / 1000));
    else if (at * 1000 <= this.now()) return this.setContent(t, rest);
    if (this.schedule.length >= MAX_SCHEDULED) fail(507, "schedule full");
    this.schedule = this.schedule.filter((e) => !(e.mac === t.mac && e.at === at));
    this.schedule.push({ mac: t.mac, at, body: rest, ...(repeat ? { repeat } : {}) });
    this.schedule.sort((a, b) => a.at - b.at);
    return { mac: t.mac, scheduled: true, at, ...(repeat ? { repeat } : {}) };
  }

  async putBatch(body) {
    const rows = body?.tags;
    if (!Array.isArray(rows)) fail(400, "tags is missing");
    if (rows.length > MAX_BATCH) fail(413, `at most ${MAX_BATCH} tags per call`);
    // all or nothing: every row is checked before any is set
    const plan = [];
    for (const [index, row] of rows.entries()) {
      try {
        const { mac, ...rest } = row || {};
        const t = this.tagOr404(String(mac || ""));
        if (rest.at != null) plan.push(() => this.scheduleOne(t, rest));
        else {
          const { content, image } = await this.prepare(t, rest);
          plan.push(() => this.apply(t, content, image, crcOf([content.layout, content.fields, !!content.price_highlight, !!content.frame])));
        }
      } catch (e) {
        if (e instanceof ApiError) fail(422, e.body.error, { index });
        throw e;
      }
    }
    const tags = [];
    for (const step of plan) tags.push(await step());
    return { tags };
  }

  putImage(t, bytes) {
    const m = MODELS[t.model];
    if (bytes.length !== rawSize(m))
      fail(422, `the demo AP takes the raw image only: ${rawSize(m)} bytes for a ${m.width} x ${m.height} tag`);
    this.schedule = this.schedule.filter((e) => e.mac !== t.mac);
    return this.apply(t, { layout: "image", fields: [] }, bytes.slice(), crc32(bytes) || 1);
  }

  imageOf(t) {
    if (!t.content || !t.image) fail(404, "no content");
    if (t.contentError) fail(409, t.contentError);
    const m = MODELS[t.model];
    return { mac: t.mac, model: t.model, width: m.width, height: m.height, red: m.colors.includes("red"),
             content_id: t.contentId, image_id: t.imageId, data: base64(t.image) };
  }

  // ---- layout files ----

  usedBy(model) { return Object.keys(MODELS).filter((m) => layoutModel(m) === model); }

  fileEntry(model) {
    const f = this.files[model];
    return { model, same_as: MODELS[model].sameAs, used_by: this.usedBy(model), version: f ? f.version : null,
             size: f ? f.size : 0, binary_size: f ? f.binarySize : 0, layouts: f ? f.layouts.map((l) => l.name) : [] };
  }

  // The limits are those of the board the renderer has built in (c6-supermini)
  async layoutList() {
    const x = (await this.renderer(Object.keys(MODELS)[0])).limits;
    return {
      max_size: MAX_LAYOUT_JSON, max_binary_size: x.fileBytes, board: "demo",
      limits: { runs: x.runs, rects: x.rects, lines: x.lines, icons: x.icons, barcodes: x.barcodes, qrs: x.qrs,
                fileBytes: x.fileBytes, jsonBytes: x.jsonBytes, fields: x.fields, fixedTextBytes: x.fixedTextBytes,
                textBytes: x.textBytes, shapes: x.shapes, polygonPoints: x.polygonPoints },
      layouts: Object.keys(MODELS).filter((m) => layoutModel(m) === m).map((m) => this.fileEntry(m)),
    };
  }

  knownModel(model) {
    if (!MODELS[model]) fail(404, `unknown model ${model}`);
    if (layoutModel(model) !== model) fail(409, `model ${model} uses the layout file of ${layoutModel(model)}`);
  }

  fileOf(model) {
    this.knownModel(model);
    const f = this.files[model];
    if (!f) fail(404, `no layout file for model ${model}`);
    return structuredClone(f.doc);
  }

  async storeLayouts(model, doc) {
    this.knownModel(model);
    if (!doc || typeof doc !== "object" || Array.isArray(doc)) fail(400, "the body is not a JSON object");
    const text = JSON.stringify(doc);
    if (text.length > MAX_LAYOUT_JSON) fail(413, `the file is ${text.length} bytes, over ${MAX_LAYOUT_JSON}`);
    if (doc.model !== model) fail(422, `the file is for model ${doc.model}, not ${model}`);
    const r = await this.renderer(model);
    const res = r.loadLayouts(text);
    if (!res.ok) {
      const old = this.files[model];   // the renderer keeps the file it had
      if (old) r.loadLayouts(JSON.stringify(old.doc));
      else r.clearLayouts();
      fail(422, res.error);
    }
    const changed = this.files[model]?.version !== res.version;
    this.files[model] = { doc: structuredClone(doc), version: res.version, size: text.length, binarySize: res.size,
                          layouts: (doc.layouts || []).map((l) => ({ name: l.name, fields: l.fields || [] })) };
    for (const m of this.usedBy(model)) if (m !== model) (await this.renderer(m)).loadLayouts(text);
    const errors = changed ? await this.redraw(model) : [];
    return { ...this.fileEntry(model), changed, content_errors: errors.slice(0, 8), content_errors_total: errors.length };
  }

  async deleteLayouts(model) {
    this.knownModel(model);
    if (!this.files[model]) fail(404, `no layout file for model ${model}`);
    delete this.files[model];
    for (const m of this.usedBy(model)) (await this.renderer(m)).clearLayouts();
    await this.redraw(model);
    return { model, deleted: true };
  }

  // The tags that show a layout of the file draw again; content that no longer fits is held back
  async redraw(fileModel) {
    const errors = [];
    for (const t of this.live()) {
      if (layoutModel(t.model) !== fileModel || !t.content || BUILTIN[t.content.layout] || t.content.layout === "image") continue;
      const res = await this.draw(t.model, t.content);
      if (res.error) {
        t.contentError = res.error;
        errors.push({ mac: t.mac, error: res.error });
      } else {
        t.contentError = null;
        t.image = res.raw;
        t.imageId = crc32(res.raw) || 1;
      }
    }
    return errors;
  }
}

// The example layout file of a model, as the page has it (web/build.py copies them to
// designs/layouts/); null when it is not there
async function fetchExample(model) {
  try {
    const r = await fetch(new URL(`../designs/layouts/${model}.json`, import.meta.url));
    return r.ok ? await r.json() : null;
  } catch {
    return null;
  }
}
