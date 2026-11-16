// The previews' renderer on the AP's own page (js/renderframe.js) and the one place the
// published site is named (js/site.js). The frame itself needs a browser; this checks what
// runs the same in both: the drawing in one go, the note, and that the copies of SITE agree.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { draw, rendererNote, LAYOUT_TEXT, FLAG_FRAME } from "../js/renderframe.js";
import { SITE, SITE_ORIGIN } from "../js/site.js";
import { WEB_ORIGIN } from "../js/handoff.js";
import { PAGES } from "../js/catalog.js";

const read = (p) => readFileSync(new URL("../../" + p, import.meta.url), "utf8");

test("draw sets the AP's limits and fonts, loads the file and draws it, in that order", () => {
  const calls = [];
  const r = {
    setLimits: (l) => calls.push(["limits", l]),
    setFonts: (f, s) => calls.push(["fonts", f, s]),
    loadLayouts: (t) => (calls.push(["load", JSON.parse(t).model]), { ok: true }),
    render: (l, fl, fields) => (calls.push(["render", l, fl, fields]), { ok: true, pixels: new Uint8Array(4), refused: "x" }),
  };
  const out = draw(r, { file: { model: "2.9" }, limits: { runs: 9 }, fonts: { files: { a: "{}" }, slots: { a: 1 } }, layout: 0x10, flags: 2, fields: ["A"] });
  assert.equal(out.ok, true);
  assert.equal(out.pixels.length, 4);
  assert.deepEqual(calls, [["limits", { runs: 9 }], ["fonts", { a: "{}" }, { a: 1 }], ["load", "2.9"], ["render", 0x10, 2, ["A"]]]);
});

test("draw without a file only draws; a file that does not compile is not drawn", () => {
  const calls = [];
  const r = { loadLayouts: () => ({ ok: false, error: "too big" }), render: (...a) => (calls.push(a), { ok: true, pixels: new Uint8Array(1) }) };
  assert.equal(draw(r, { layout: LAYOUT_TEXT, flags: FLAG_FRAME, fields: ["x"] }).ok, true);
  assert.deepEqual(calls, [[2, 2, ["x"]]]);
  assert.deepEqual(draw(r, { file: {}, layout: 0x10 }), { ok: false, error: "too big" });
  assert.equal(calls.length, 1);
});

test("the constants are the renderer's", () => {
  const src = read("web/js/renderer.js");
  assert.match(src, new RegExp(`export const LAYOUT_TEXT = ${LAYOUT_TEXT};`));
  assert.match(src, /export const FLAG_FRAME = 0x02;/);
  assert.equal(FLAG_FRAME, 2);
});

test("rendererNote: nothing for the AP's own renderer or the same version", () => {
  assert.equal(rendererNote(null, "", "0.10.0", "0.11.0"), "");
  assert.equal(rendererNote({}, SITE, "0.10.0", "v0.10.0"), "");
  assert.equal(rendererNote({}, SITE, "0.10.0", undefined), "");
});

test("rendererNote: says when the page's renderer is for other firmware, or missing", () => {
  assert.match(rendererNote({}, SITE, "0.10.0", "0.11.0"), /AP firmware 0\.11\.0 from mathiasbk\.github\.io\/openESL; this AP runs 0\.10\.0/);
  assert.match(rendererNote(null, SITE, "0.10.0", "0.10.0"), /^No previews: .*could not load the one from mathiasbk\.github\.io\/openESL/);
});

test("SITE is the one place the published page is named", () => {
  assert.match(SITE, /^https:\/\/[^/]+\/.*\/$/);
  assert.equal(SITE_ORIGIN, new URL(SITE).origin);
  assert.equal(WEB_ORIGIN, SITE_ORIGIN);
  assert.equal(PAGES, SITE);
  // the copies outside the web page: the bridge's allowed origin and the radio's page
  assert.ok(read("tools/esl-client/esl_client.py").includes(`BRIDGE_ORIGINS = ("${SITE_ORIGIN}",)`));
  assert.ok(read("ap/netradio/main/netradio_main.c").includes(`<a href=\\"${SITE}#ap\\">`));
  for (const f of ["web/js/apdash.js", "web/js/catalog.js", "web/js/handoff.js", "ap/main/webui.py"])
    assert.ok(!read(f).includes(SITE_ORIGIN), `${f} names the site itself; use js/site.js`);
});
