// Making a font file in the browser (js/fontmaker.js, #177): the part that turns drawn glyphs
// into the font file format; the renderer draws the result as the AP would. The canvas part is
// checked in a browser (the dashboard's Layouts card, Make a font…).
//
//   python web/build.py && node --test web/test/
import { test } from "node:test";
import assert from "node:assert/strict";
import { packFont, codesFor, CHARSETS, ELLIPSIS } from "../js/fontmaker.js";
import { loadRenderer, LAYOUT_FILE } from "../js/renderer.js";

const WASM = new URL("../dist/wasm/", import.meta.url);

test("character sets: Latin-1 with the ellipsis, prices, or the characters given", () => {
  assert.equal(CHARSETS.latin1.length, 95 + 1 + 96);
  assert.ok(CHARSETS.latin1.includes(ELLIPSIS));
  assert.deepEqual(codesFor("price"), CHARSETS.price);
  assert.deepEqual(codesFor("10,-"), [0x20, 0x2c, 0x2d, 0x30, 0x31, ELLIPSIS]);
  assert.deepEqual(codesFor("æ…"), [0x20, ELLIPSIS, 0xe6]);
  assert.throws(() => codesFor("€"), /Latin-1/);
});

test("packFont packs columns MSB first, ceil(height / 8) bytes a column", () => {
  // "A" is 2 columns: the first all black, the second only its top and bottom rows
  const f = packFont({
    name: "t", height: 10, baseline: 8, codes: [0x41, 0x43], source: "test ø",
    glyph: (c) => (c === 0x41 ? { width: 2, ink: (x, y) => x === 0 || y === 0 || y === 9 } : { width: 1, ink: () => false }),
  });
  assert.deepEqual([f.format, f.version, f.first, f.last, f.height, f.baseline], ["esl-font", 1, 0x41, 0x43, 10, 8]);
  assert.deepEqual(f.widths, [2, 0, 1]);
  assert.deepEqual([...Buffer.from(f.bitmap, "base64")], [0xff, 0xc0, 0x80, 0x40, 0, 0]);
  assert.equal(f.source, "test ?");
  assert.throws(() => packFont({ name: "t", height: 70, baseline: 8, codes: [65], glyph: () => null }), /height/);
});

test("the renderer draws a made font as packed", async () => {
  const f = packFont({
    name: "made", height: 12, baseline: 9, codes: [0x20, 0x31],
    glyph: (c) => (c === 0x31 ? { width: 3, ink: (x) => x === 1 } : { width: 4, ink: () => false }),
  });
  const r = await loadRenderer("2.9", WASM);
  assert.deepEqual(r.setFonts({ made: JSON.stringify(f) }).errors, []);
  const res = r.loadLayouts(JSON.stringify({
    format: "esl-layouts", version: 1, model: "2.9",
    layouts: [{ name: "m", fields: ["a"], elements: [{ type: "text", field: "a", x: 0, y: 0, w: 60, h: 12, fonts: ["made"] }] }],
  }));
  assert.equal(res.ok, true, res.error);
  const out = r.render(LAYOUT_FILE, 0, ["1 1"]);
  assert.equal(out.ok, true);
  // two strokes of 12 rows: a 1 is 3 wide with its middle column black, a space 4 wide
  assert.equal(out.pixels.reduce((n, v) => n + (v ? 1 : 0), 0), 24);
  r.setFonts({});
});
