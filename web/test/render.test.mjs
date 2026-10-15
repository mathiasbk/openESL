// The WebAssembly renderer must draw exactly what the same C code draws when built natively
// (tag/build/web-<model>/web_render, from `python web/build.py --native`), and must accept
// and reject the same layout files.
//
//   python web/build.py && python web/build.py --native && node --test web/test/
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";
import { loadRenderer, MODELS } from "../js/renderer.js";

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
    const res = r.render(2, 0, ["x".repeat(40), "y".repeat(40), "z".repeat(20), ""]);
    assert.equal(res.ok, false);
    assert.match(res.error, /at most 92/);
  });
}
