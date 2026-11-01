// Font files (#177): the renderer takes the AP's font files (renderer.setFonts) and draws a
// layout that names one; js/apfonts.js reads them from an AP once per version.
//
//   python web/build.py && node --test web/test/
import { test } from "node:test";
import assert from "node:assert/strict";
import { loadRenderer, LAYOUT_FILE } from "../js/renderer.js";
import { apFonts, checkFontFile } from "../js/apfonts.js";

const WASM = new URL("../dist/wasm/", import.meta.url);

// "A" is a block 4 columns wide and 10 rows high; "B" is not in the font
const block = (name, w = 4) => JSON.stringify({
  format: "esl-font", version: 1, name, height: 10, baseline: 8, first: 65, last: 66,
  widths: [w, 0], bitmap: Buffer.from(Array(w).fill([0xff, 0xc0]).flat()).toString("base64"),
});
const layout = (font) => JSON.stringify({
  format: "esl-layouts", version: 1, model: "2.9",
  layouts: [{ name: "f", fields: ["a"], elements: [{ type: "text", field: "a", x: 10, y: 10, w: 50, h: 12, fonts: [font] }] }],
});

test("a layout draws with a font file the renderer has", async () => {
  const r = await loadRenderer("2.9", WASM);
  r.setFonts({});
  const no = r.loadLayouts(layout("blk"));
  assert.equal(no.ok, false);
  assert.match(no.error, /unknown font "blk"/);

  assert.deepEqual(r.setFonts({ blk: block("blk") }).errors, []);
  assert.equal(r.limits.fontHeight.blk, 10);
  assert.equal(r.limits.fontHeight.small, 17);
  const res = r.loadLayouts(layout("blk"));
  assert.equal(res.ok, true, res.error);
  const out = r.render(LAYOUT_FILE, 0, ["A"]);
  assert.equal(out.ok, true);
  assert.equal(out.pixels.reduce((n, v) => n + (v ? 1 : 0), 0), 40);
  assert.equal(out.pixels[10 * r.width + 10], 1);
  assert.equal(out.pixels[19 * r.width + 13], 1);
  assert.equal(out.pixels[10 * r.width + 14], 0);

  // setLimits keeps the font files' heights; another set replaces them
  r.setLimits(null);
  assert.equal(r.limits.fontHeight.blk, 10);
  r.setFonts({ wide: block("wide", 6) });
  assert.equal(r.limits.fontHeight.blk, undefined);
  assert.equal(r.loadLayouts(layout("blk")).ok, false);
  assert.equal(r.loadLayouts(layout("wide")).ok, true);
  const bad = r.setFonts({ bad: '{"format":"esl-font","version":1}' });
  assert.equal(bad.errors.length, 1);
  r.setFonts({});
});

test("font files go into the AP's slots, so a layout file compiles to the AP's version", async () => {
  const r = await loadRenderer("2.9", WASM);
  const two = JSON.stringify({
    format: "esl-layouts", version: 1, model: "2.9",
    layouts: [{ name: "f", fields: ["a"], elements: [{ type: "text", field: "a", x: 10, y: 10, w: 50, h: 12, fonts: ["bb", "aa"] }] }],
  });
  const files = { aa: block("aa"), bb: block("bb", 3) };
  r.setFonts(files, { aa: 1, bb: 0 });
  const v1 = r.loadLayouts(two);
  r.setFonts(files, { aa: 0, bb: 1 });
  const v2 = r.loadLayouts(two);
  r.setFonts(files, { aa: 1, bb: 0 });
  const v3 = r.loadLayouts(two);
  assert.ok(v1.ok && v2.ok && v3.ok);
  assert.notEqual(v1.version, v2.version);   // the slot is in the compiled file
  assert.equal(v1.version, v3.version);
  r.setFonts({});
});

test("apFonts reads each font file once per version, and an older AP has none", async () => {
  const calls = [];
  let version = 1;
  const client = {
    async call(method, path) {
      calls.push(method + " " + path);
      if (path === "/api/fonts") return { max_fonts: 8, max_bytes: 32768, bytes: 24, builtin: [], fonts: [{ name: "blk", version, slot: 2 }] };
      return JSON.parse(block("blk"));
    },
  };
  const f = apFonts();
  const a = await f.read(client);
  assert.equal(a.supported, true);
  assert.deepEqual(Object.keys(a.files), ["blk"]);
  assert.deepEqual(a.slots, { blk: 2 });
  await f.read(client);
  assert.equal(calls.filter((c) => c === "GET /api/fonts/blk").length, 1);
  version = 2;
  await f.read(client);
  assert.equal(calls.filter((c) => c === "GET /api/fonts/blk").length, 2);

  const old = { async call() { throw new Error("404: not found"); } };
  assert.deepEqual(await f.read(old), { supported: false, info: null, files: {}, slots: {} });

  assert.equal(checkFontFile(JSON.parse(block("blk"))), null);
  assert.match(checkFontFile({ format: "esl-layouts" }), /not a font file/);
  assert.match(checkFontFile({ format: "esl-font", name: "Big" }), /name/);
});
