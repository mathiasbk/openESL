// renderer.js - the label renderer (web/wasm/web_api.c built to WebAssembly) for one tag model.
// The AP's own code: its layout compiler and its drawing, the image it sends to a tag.
//
//   const r = await loadRenderer("2.9");        // or "1.54"
//   r.loadLayouts(jsonText)  -> { ok, error, version, size, bin }
//   r.render(layout, flags, fields) -> { ok, error, pixels (Uint8Array, 1 = black, 2 = red),
//                                       refused (why the AP would refuse these fields, if it would) }
//   r.setLimits(apLimits)    the limits of the AP the page talks to ("limits" from GET /api/layouts),
//                            or null for the default board's; r.limits then has them
//   r.setFonts({name: text}) the font files layouts may name (GET /api/fonts/{name}, js/apfonts.js),
//                            as on the AP: the ones loaded before are dropped. -> { errors }
//                            r.limits.fontHeight then has their heights next to the built-in fonts'

import { MODELS as REGISTRY } from "./models.js";

// Per model name: the renderer file (render-<file>.js), the name to show, the screen size, its colours
export const MODELS = Object.fromEntries(
  Object.entries(REGISTRY).map(([n, m]) => [n, { file: m.build, name: m.short, width: m.width, height: m.height, colors: m.colors }]),
);

export const LAYOUT_PRICE = 1;
export const LAYOUT_TEXT = 2;
export const LAYOUT_FILE = 0x10;         // layout n of the file is 0x10 + n
export const FLAG_PRICE_HIGHLIGHT = 0x01;
export const FLAG_FRAME = 0x02;

const cache = {};

// The limits an AP board sets (esl_limits in esl_layout.h), by their web_limit number
const SETTABLE = { runs: 0, rects: 1, fileBytes: 2, jsonBytes: 14, lines: 17, icons: 18 };

// Days since 1970-01-01 of d's local date (web_set_today)
export function localDays(d) {
  return Math.floor(Date.UTC(d.getFullYear(), d.getMonth(), d.getDate()) / 86400000);
}

export function loadRenderer(model, base = new URL("../wasm/", import.meta.url)) {
  if (!MODELS[model]) throw new Error("unknown model " + model);
  if (!cache[model]) cache[model] = create(model, base);
  return cache[model];
}

async function create(model, base) {
  const file = MODELS[model].file;
  const mod = await import(new URL("render-" + file + ".js", base).href);
  const locateFile = (p) => {
    const u = new URL(p, base);
    return u.protocol === "file:" ? decodeURIComponent(u.pathname) : u.href;   // file: for the node tests
  };
  const M = await mod.default({ locateFile });
  const call = (name, ret, types, args) => M.ccall(name, ret, types, args);
  const width = call("web_width", "number", [], []);
  const height = call("web_height", "number", [], []);
  const error = () => M.UTF8ToString(call("web_error", "number", [], []));
  const limit = (what) => call("web_limit", "number", ["number"], [what]);
  const fileHeights = {};   // the font files' heights, by name (setFonts)
  const readLimits = (l) => Object.assign(l, {
    runs: limit(0),
    rects: limit(1),
    fileBytes: limit(2),
    invertPad: limit(3),
    linesPerText: limit(4),
    fontHeight: { small: limit(5), medium: limit(6), mid: limit(7), large: limit(8), text20: limit(15), price44: limit(16),
                  ...fileHeights },
    barcodes: limit(9),
    barcodeMinH: limit(10),
    barcodeModuleMax: limit(11),
    fixedTextBytes: limit(12),
    fields: limit(13),
    jsonBytes: limit(14),
    lines: limit(17),
    icons: limit(18),
    lineWidthMax: limit(19),
    qrs: limit(20),
    qrModuleMax: limit(21),
  });
  // the default board's, for setLimits(null)
  const defaults = Object.fromEntries(Object.entries(SETTABLE).map(([key, what]) => [key, limit(what)]));

  return {
    model,
    width,
    height,
    // Text and one length byte per field sent share this many bytes in an update.
    fieldSpace: call("web_max_text", "number", [], []),
    // The compiler's limits (web_limit in web_api.c): the default AP board's until setLimits.
    // The same object throughout, so a reference to it sees the AP's limits once they are set.
    limits: readLimits({}),
    // An older renderer (the published one on an AP's own page) cannot take an AP's limits
    canSetLimits: typeof M._web_set_limit === "function",

    setLimits(apLimits) {
      if (!this.canSetLimits) return;
      for (const [key, what] of Object.entries(SETTABLE))
        call("web_set_limit", "number", ["number", "number"], [what, apLimits?.[key] ?? defaults[key]]);
      readLimits(this.limits);
    },

    loadLayouts(text) {
      const len = M.lengthBytesUTF8(text);
      const p = M._malloc(len + 1);
      M.stringToUTF8(text, p, len + 1);
      const ok = call("web_load_layouts", "number", ["number", "number"], [p, len]) === 1;
      M._free(p);
      if (!ok) return { ok, error: error() };
      const size = call("web_layout_size", "number", [], []);
      const at = call("web_layout_bin", "number", [], []);
      return {
        ok,
        version: call("web_layout_version", "number", [], []) >>> 0,
        size,
        bin: M.HEAPU8.slice(at, at + size),
      };
    },

    clearLayouts() {
      call("web_clear_layouts", null, [], []);
    },

    // A renderer from before font files (the published one on an older AP's page) has none
    canLoadFonts: typeof M._web_load_font === "function",
    fontKey: "",

    setFonts(files = {}) {
      const errors = [];
      if (!this.canLoadFonts) return { errors };
      const names = Object.keys(files).sort();
      const key = JSON.stringify(names.map((n) => [n, files[n]]));
      if (key === this.fontKey) return { errors };
      call("web_clear_fonts", null, [], []);
      for (const n of Object.keys(fileHeights)) delete fileHeights[n];
      for (const n of names) {
        const text = files[n];
        const len = M.lengthBytesUTF8(text);
        const p = M._malloc(len + 1);
        M.stringToUTF8(text, p, len + 1);
        const ok = call("web_load_font", "number", ["number", "number"], [p, len]) === 1;
        M._free(p);
        if (ok) fileHeights[n] = JSON.parse(text).height;
        else errors.push(`${n}: ${error()}`);
      }
      this.fontKey = key;
      readLimits(this.limits);
      return { errors };
    },

    textBytes(s) {
      return call("web_text_bytes", "number", ["string"], [s]);
    },

    render(layout, flags, fields, today = new Date()) {
      // today's date for date fields in fixed texts ({date}, {week}), as the AP fills them in;
      // a renderer from before them has no web_set_today and draws them as written
      if (typeof M._web_set_today === "function")
        call("web_set_today", null, ["number"], [localDays(today)]);
      const f = fields.slice(0, 6).map((s) => String(s ?? ""));
      const n = f.length;
      while (f.length < 6) f.push("");
      const res = call("web_render", "number",
        ["number", "number", "number", "string", "string", "string", "string", "string", "string"],
        [layout, flags, n, ...f]);
      if (!res) return { ok: false, error: error() };
      const at = call("web_fb", "number", [], []);
      const out = { ok: true, pixels: M.HEAPU8.slice(at, at + width * height) };
      if (res === 2) out.refused = error();   // drawn, but PUT /api/tags would refuse these fields
      return out;
    },
  };
}
