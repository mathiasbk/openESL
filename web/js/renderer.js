// renderer.js - the label renderer (web/wasm/web_api.c built to WebAssembly) for one tag model.
// The AP's own code: its layout compiler and its drawing, the image it sends to a tag.
//
//   const r = await loadRenderer("2.9");        // or "1.54"
//   r.loadLayouts(jsonText)  -> { ok, error, version, size, bin }
//   r.render(layout, flags, fields) -> { ok, error, pixels (Uint8Array, 1 = black, 2 = red),
//                                       refused (why the AP would refuse these fields, if it would) }

import { MODELS as REGISTRY } from "./models.js";

// Per model name: the renderer file (render-<file>.js), the name to show, the screen size, its colours
export const MODELS = Object.fromEntries(
  Object.entries(REGISTRY).map(([n, m]) => [n, { file: m.build, name: n + '"', width: m.width, height: m.height, colors: m.colors }]),
);

export const LAYOUT_PRICE = 1;
export const LAYOUT_TEXT = 2;
export const LAYOUT_FILE = 0x10;         // layout n of the file is 0x10 + n
export const FLAG_PRICE_HIGHLIGHT = 0x01;
export const FLAG_FRAME = 0x02;

const cache = {};

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

  return {
    model,
    width,
    height,
    // Text and one length byte per field sent share this many bytes in an update.
    fieldSpace: call("web_max_text", "number", [], []),
    // The compiler's limits (web_limit in web_api.c).
    limits: {
      runs: call("web_limit", "number", ["number"], [0]),
      rects: call("web_limit", "number", ["number"], [1]),
      fileBytes: call("web_limit", "number", ["number"], [2]),
      invertPad: call("web_limit", "number", ["number"], [3]),
      linesPerText: call("web_limit", "number", ["number"], [4]),
      fontHeight: {
        small: call("web_limit", "number", ["number"], [5]),
        medium: call("web_limit", "number", ["number"], [6]),
        mid: call("web_limit", "number", ["number"], [7]),
        large: call("web_limit", "number", ["number"], [8]),
        text20: call("web_limit", "number", ["number"], [15]),
        price44: call("web_limit", "number", ["number"], [16]),
      },
      barcodes: call("web_limit", "number", ["number"], [9]),
      barcodeMinH: call("web_limit", "number", ["number"], [10]),
      barcodeModuleMax: call("web_limit", "number", ["number"], [11]),
      fixedTextBytes: call("web_limit", "number", ["number"], [12]),
      fields: call("web_limit", "number", ["number"], [13]),
      jsonBytes: call("web_limit", "number", ["number"], [14]),
      lines: call("web_limit", "number", ["number"], [17]),
      icons: call("web_limit", "number", ["number"], [18]),
      lineWidthMax: call("web_limit", "number", ["number"], [19]),
      qrs: call("web_limit", "number", ["number"], [20]),
      qrModuleMax: call("web_limit", "number", ["number"], [21]),
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

    textBytes(s) {
      return call("web_text_bytes", "number", ["string"], [s]);
    },

    render(layout, flags, fields) {
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
