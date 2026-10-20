// The designer's budget meter (js/limits.js) must count lines of text and rectangles the
// same way the AP's compiler does: the compiler's error names the same numbers.
//
//   python web/build.py && node --test web/test/
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { loadRenderer, MODELS } from "../js/renderer.js";
import { layoutUsage } from "../js/limits.js";

const WASM = new URL("../dist/wasm/", import.meta.url);
const example = (m) => JSON.parse(readFileSync(new URL(`../../tag/designs/layouts/${m}.json`, import.meta.url)));

for (const model of Object.keys(MODELS)) {
  test(`${model}: the example layouts are within the limits`, async () => {
    const r = await loadRenderer(model, WASM);
    assert.equal(r.loadLayouts(JSON.stringify(example(model))).ok, true);
    for (const l of example(model).layouts) {
      const u = layoutUsage(l, r.limits);
      assert.ok(u.runs <= r.limits.runs && u.rects <= r.limits.rects, l.name);
    }
  });

  test(`${model}: over the limits, the compiler names the same counts`, async () => {
    const r = await loadRenderer(model, WASM);
    const { width } = MODELS[model];
    const fonts = [["small"], ["medium", "small"], ["large", "mid"], ["mid"]];
    const cases = [];
    for (let n = 1; n <= r.limits.rects + 1; n++) {
      cases.push([
        { type: "text", field: "a", x: 0, y: 0, w: width, h: 32, fonts: ["medium"], color: n % 2 ? "invert" : "black" },
        ...Array.from({ length: n }, (_, i) => ({ type: "rect", x: i, y: 50, w: 4, h: 4 })),
      ]);
    }
    for (let n = 1; n <= 2; n++) {
      cases.push(Array.from({ length: n }, (_, i) => ({
        type: "barcode", field: "a", symbology: "code128", x: 0, y: 20 * i, w: width, h: 16,
      })));
    }
    for (let seed = 1; seed < 40; seed++) {
      const elements = [];
      cases.push(elements);
      for (let i = 0; i < 2 + (seed % 15); i++) {
        const k = (seed * 7 + i * 3) % 5;
        if (k === 4) elements.push({ type: "rect", x: 0, y: i, w: 10, h: 2 });
        else elements.push({
          type: "text", field: "a", x: 0, y: 0, w: width, h: 60 + ((seed * 13 + i * 11) % 68),
          fonts: fonts[k], lines: (seed + i) % 4, color: ["black", "invert", "highlight", "white"][(seed + i) % 4],
        });
      }
    }
    for (const elements of cases) {
      const layout = { name: "t", fields: ["a"], elements };
      const res = r.loadLayouts(JSON.stringify({ format: "esl-layouts", version: 1, model, layouts: [layout] }));
      const u = layoutUsage(layout, r.limits);
      if (u.barcodes > r.limits.barcodes) {
        assert.match(res.error, /at most \d+ barcode/, JSON.stringify(layout));
      } else if (u.runs > r.limits.runs) {
        assert.match(res.error, new RegExp(`needs up to ${u.runs} lines`), JSON.stringify(layout));
      } else if (u.rects > r.limits.rects) {
        assert.match(res.error, new RegExp(`needs ${u.rects} rectangles`), JSON.stringify(layout));
      } else {
        assert.equal(res.ok, true, res.error + " " + JSON.stringify(layout));
      }
    }
  });
}
