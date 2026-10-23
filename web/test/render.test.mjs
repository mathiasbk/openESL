// The WebAssembly renderer must draw exactly what the same C code draws when built natively
// (tag/build/web-<model>/web_render, from `python web/build.py --native`), and must accept
// and reject the same layout files.
//
//   python web/build.py && python web/build.py --native && node --test web/test/
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";
import path from "node:path";
import { loadRenderer, MODELS } from "../js/renderer.js";
import { layoutUsage } from "../js/limits.js";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const WASM = new URL("../dist/wasm/", import.meta.url);
const layoutPath = (m) => path.join(ROOT, "tag", "designs", "layouts", m + ".json");

function native(model, layoutFile, layout, flags, fields) {
  const exe = path.join(ROOT, "tag", "build", "web-" + MODELS[model].file, "web_render");
  let out;
  try {
    out = execFileSync(exe, [layoutFile || "-", String(layout), String(flags), ...fields], { encoding: "utf8" });
  } catch (e) {
    out = e.stdout;
  }
  const lines = out.trimEnd().split("\n");
  if (!lines[0].startsWith("ok")) return { ok: false, error: lines[0].replace(/^error /, "") };
  return { ok: true, pixels: Uint8Array.from(lines.slice(1).join(""), (c) => c.charCodeAt(0) - 48) };
}

const CASES = [
  [1, 1, ["Lettmelk 1 L", "24.90", "kr 24,90/l"]],
  [1, 0, ["Kjøttdeig av storfe 14 % fett, ekstra stor familiepakning", "129,90", "kr/kg 108,25"]],
  [1, 3, ["Rømme lett 18 % Tine", "19,90", "Ærlig økologisk – kr 66,33/l"]],
  [2, 2, ["Åpningstider", "Man–fre 9–21", "Lørdag 9–18", "Søndag stengt"]],
  [2, 0, ["ÆØÅ æøå é ü ß ©", "", "…", "“sitat”"]],
];

for (const model of Object.keys(MODELS)) {
  test(`built-in layouts, ${model}`, async () => {
    const r = await loadRenderer(model, WASM);
    r.clearLayouts();
    for (const [layout, flags, fields] of CASES) {
      const a = r.render(layout, flags, fields);
      const b = native(model, null, layout, flags, fields);
      assert.ok(a.ok && b.ok, `${layout} ${fields[0]}: ${a.error || b.error}`);
      assert.deepEqual(a.pixels, b.pixels, `${layout} ${fields[0]}`);
    }
  });

  test(`layout file, ${model}`, async () => {
    const r = await loadRenderer(model, WASM);
    const text = readFileSync(layoutPath(model), "utf8");
    const res = r.loadLayouts(text);
    assert.ok(res.ok, res.error);
    assert.ok(res.size > 4 && res.version > 0);
    const layouts = JSON.parse(text).layouts;
    layouts.forEach((lay, n) => {
      for (const flags of [0, 1, 3]) {
        const fields = lay.fields.map((f, i) => (i === 1 ? "49,90" : f + " tekst æøå"));
        const a = r.render(0x10 + n, flags, fields);
        const b = native(model, layoutPath(model), 0x10 + n, flags, fields);
        assert.ok(a.ok && b.ok, `${lay.name}: ${a.error || b.error}`);
        assert.deepEqual(a.pixels, b.pixels, `${lay.name} flags ${flags}`);
      }
    });
  });

  test(`rejected layout file gives the AP's message, ${model}`, async () => {
    const r = await loadRenderer(model, WASM);
    const other = model === "2.9" ? "1.54" : "2.9";
    const res = r.loadLayouts(readFileSync(layoutPath(other), "utf8"));
    assert.equal(res.ok, false);
    assert.equal(res.error, native(model, layoutPath(other), 1, 0, []).error);
    assert.match(res.error, /model/);
  });

  test(`too much text, ${model}`, async () => {
    const r = await loadRenderer(model, WASM);
    r.clearLayouts();
    // the fields share 96 bytes with one length byte each; the empty one at the end is not sent
    const res = r.render(2, 0, ["x".repeat(40), "y".repeat(40), "z".repeat(20), ""]);
    assert.equal(res.ok, false);
    assert.match(res.error, /at most 93 with 3 fields/);
    assert.ok(r.render(2, 0, ["x".repeat(40), "y".repeat(40), "z".repeat(13), ""]).ok);
  });

  test(`a QR field too long for its box is drawn, with the AP's reason, ${model}`, async () => {
    const r = await loadRenderer(model, WASM);
    const file = { format: "esl-layouts", version: 1, model, layouts: [{ name: "q", fields: ["url"],
      elements: [{ type: "qr", field: "url", x: 0, y: 0, w: 25, h: 25 }] }] };
    assert.ok(r.loadLayouts(JSON.stringify(file)).ok);
    const fits = r.render(0x10, 0, ["x".repeat(14)]);   // version 1 at M holds 14 bytes
    assert.ok(fits.ok && !fits.refused);
    const long = r.render(0x10, 0, ["x".repeat(15)]);
    assert.ok(long.ok);
    assert.match(long.refused, /too long for its QR code on model .* \(15 of 14 bytes\)/);
  });

  test(`lines, round and open boxes and icons, ${model}`, async () => {
    const r = await loadRenderer(model, WASM);
    const { width, height } = MODELS[model];
    const file = {
      format: "esl-layouts", version: 1, model,
      icons: { star: { w: 9, h: 3, data: "gAA+AP+A" } },
      layouts: [{
        name: "shapes", fields: ["a"],
        elements: [
          { type: "rect", x: 2, y: 2, w: width - 4, h: height - 4, radius: 12, border: 3 },
          { type: "line", x: 10, y: 10, w: width - 20, h: height - 20, width: 4 },
          { type: "line", x: 10, y: 10, w: width - 20, h: height - 20, direction: "up", color: "white" },
          { type: "line", x: 10, y: Math.floor(height / 2), w: width - 20, h: 1 },
          { type: "icon", icon: "star", x: Math.floor(width / 2) - 4, y: 30 },
          { type: "text", field: "a", x: 20, y: 30, w: 100, h: 24, fonts: ["medium"] },
        ],
      }],
    };
    const text = JSON.stringify(file);
    const res = r.loadLayouts(text);
    assert.ok(res.ok, res.error);
    const u = layoutUsage(file.layouts[0], r.limits);
    assert.deepEqual([u.rects, u.lines, u.icons, u.runs], [1, 3, 1, 1]);
    const tmp = path.join(mkdtempSync(path.join(tmpdir(), "esl-")), "shapes.json");
    writeFileSync(tmp, text);
    const a = r.render(0x10, 0, ["Hei"]);
    const b = native(model, tmp, 0x10, 0, ["Hei"]);
    assert.ok(a.ok && b.ok, a.error || b.error);
    assert.deepEqual(a.pixels, b.pixels);
    // the icon's first row: one pixel at its left edge
    const ix = Math.floor(width / 2) - 4;
    assert.equal(a.pixels[30 * width + ix], 1);
    assert.equal(a.pixels[30 * width + ix + 1], 0);
    // an icon that is not in the file is the AP's message
    file.layouts[0].elements.push({ type: "icon", icon: "moon", x: 0, y: 0 });
    assert.match(r.loadLayouts(JSON.stringify(file)).error, /icon "moon" is not in the file's icons/);
  });
}
