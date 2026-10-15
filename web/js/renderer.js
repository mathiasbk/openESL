// renderer.js - the label renderer (web/wasm/web_api.c built to WebAssembly) for one tag model.
// The same code the tags and the AP run: the AP's layout compiler and the tag's drawing.
//
//   const r = await loadRenderer("2.9");        // or "1.54"
//   r.loadLayouts(jsonText)  -> { ok, error, version, size, bin }
//   r.render(layout, flags, fields) -> { ok, error, pixels (Uint8Array, 1 = black) }

export const MODELS = {
  "2.9": { file: "29", name: '2.9"', width: 296, height: 128 },
  "1.54": { file: "154", name: '1.54"', width: 200, height: 200 },
};

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
    maxText: call("web_max_text", "number", [], []),

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
      const f = fields.slice(0, 4).map((s) => String(s ?? ""));
      const n = f.length;
      while (f.length < 4) f.push("");
      const ok = call("web_render", "number",
        ["number", "number", "number", "string", "string", "string", "string"],
        [layout, flags, n, ...f]) === 1;
      if (!ok) return { ok, error: error() };
      const at = call("web_fb", "number", [], []);
      return { ok, pixels: M.HEAPU8.slice(at, at + width * height) };
    },
  };
}
