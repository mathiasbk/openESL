// The template catalog (templates/): every template passes the check web/build.py publishes it
// with (web/templates.mjs), and the check refuses what it must. Also the picker's helpers in
// js/catalog.js: adding a template to a layout file, names, icons and same_as.
//
//   python web/build.py && node --test web/test/
import { test } from "node:test";
import assert from "node:assert/strict";
import { readCatalog, checkCatalog, missingVariants } from "../templates.mjs";
import { addTemplate, variantFor, soloFile, search, freeName } from "../js/catalog.js";
import { loadRenderer } from "../js/renderer.js";

const WASM = new URL("../dist/wasm/", import.meta.url);
const real = () => {
  const errors = [];
  const cat = readCatalog(undefined, errors);
  assert.deepEqual(errors, []);
  return cat;
};
const find = (cat, id) => cat.templates.find((x) => x.t.id === id);

test("every template in templates/ passes the check", async () => {
  const cat = real();
  const { errors, index } = await checkCatalog(cat, { wasm: WASM });
  assert.deepEqual(errors, []);
  assert.ok(index.templates.length >= 12);
  for (const c of index.categories) assert.ok(index.templates.filter((t) => t.category === c.id).length >= 3, c.id);
  for (const t of index.templates) {
    assert.ok(t.models["2.9"], `${t.id} has a 2.9" variant`);
    assert.ok(t.models["2.9"].bytes > 0 && t.models["2.9"].bytes <= 1024, `${t.id} fits well within one file`);
  }
  assert.deepEqual(missingVariants(index)["2.9"], []);
});

test("the 3d-printer template has the six fields of docs/home-assistant.md on both models", () => {
  const { t } = find(real(), "3d-printer");
  assert.deepEqual(Object.keys(t.fields), ["printer", "name", "pct", "left", "done", "bar"]);
  for (const m of ["2.9", "1.54"]) {
    const l = variantFor(t, m).layout;
    assert.equal(l.name, "3d-printer");
    // no fixed brand text: the only fixed text is the % sign
    assert.deepEqual(l.elements.filter((e) => e.text !== undefined).map((e) => e.text), ["%"]);
  }
});

// A copy of the real catalog with one change, which the check must refuse with the given text
async function refuses(change, pattern) {
  const cat = structuredClone(real());
  change(cat);
  const { errors } = await checkCatalog(cat, { wasm: WASM });
  assert.ok(errors.some((e) => pattern.test(e)), `expected ${pattern}, got:\n${errors.join("\n")}`);
}

test("the check refuses a broken catalog", async () => {
  await refuses((c) => { find(c, "sign").t.category = "nowhere"; }, /sign\.json: category "nowhere" is not in categories\.json/);
  await refuses((c) => { c.categories.categories.pop(); }, /category "home" is not in categories\.json/);
  await refuses((c) => { c.templates.push({ file: "signs/sign-2.json", t: { ...find(c, "sign").t } }); }, /id "sign" is also in/);
  await refuses((c) => { delete find(c, "notice").t.fields.detail; }, /notice\.json: variant 2\.9: the layout's fields must be/);
  await refuses((c) => { find(c, "notice").t.fields.extra = "x"; }, /notice\.json: variant 2\.9: the layout's fields must be/);
  await refuses((c) => { find(c, "notice").t.fields.detail = ""; }, /fields\.detail needs a description/);
  await refuses((c) => { find(c, "notice").t.format = "esl-layouts"; }, /needs "format": "esl-template"/);
  await refuses((c) => { find(c, "notice").t.colour = "x"; }, /unknown key "colour"/);
  await refuses((c) => { find(c, "notice").t.docs = "docs/none.md"; }, /docs: no file docs\/none\.md/);
  await refuses((c) => { find(c, "notice").t.variants["9.9"] = find(c, "notice").t.variants["2.9"]; }, /no tag model "9\.9"/);
  // a variant that does not compile, with the template and model in the message
  await refuses((c) => { variantFor(find(c, "notice").t, "1.54").layout.elements[1].w = 500; },
    /templates\/signs\/notice\.json: variant 1\.54: the AP would refuse it: .*elements\[1\]/);
  // the same fields in the same order on every model
  await refuses((c) => { const l = variantFor(find(c, "sign").t, "1.54").layout; l.fields.reverse(); }, /variant 1\.54: the layout's fields must be the template's/);
  await refuses((c) => { variantFor(find(c, "sign").t, "1.54").layout.name = "other"; }, /same name in every variant/);
  await refuses((c) => { variantFor(find(c, "notice").t, "2.9").layout._sample.pop(); }, /_sample needs one sample text per field/);
  await refuses((c) => { variantFor(find(c, "notice").t, "2.9").layout.elements[1].color = "red"; }, /element 1 is red, which the 2\.9" has not/);
  // same_as only between models with the same screen
  await refuses((c) => { find(c, "notice").t.variants["1.54"] = { same_as: "2.9" }; }, /same_as "2\.9" needs the same screen/);
  // the starting file's layouts and the catalog's stay the same
  await refuses((c) => { variantFor(find(c, "vare").t, "2.9").layout.elements[0].h = 25; }, /tag\/designs\/layouts\/2\.9\.json has a layout "vare"/);
  // a sample text the AP would refuse (too long for the QR code)
  await refuses((c) => { variantFor(find(c, "guest-wifi").t, "1.54").layout._sample[3] = "x".repeat(90); }, /refuse the sample text/);
});

test("same_as reuses another model's variant", () => {
  const t = { id: "x", variants: { "2.9": { layout: { name: "x", elements: [] }, note: "a" }, "1.54": { same_as: "2.9" } } };
  assert.equal(variantFor(t, "1.54").layout.name, "x");
  assert.equal(variantFor(t, "1.54").note, "a");
  assert.equal(variantFor(t, "7.5"), null);
  assert.equal(soloFile(t, "1.54").model, "1.54");
});

test("adding a template: unique name, its id, its icons, and the file stays as it was", async () => {
  const { t } = find(real(), "notice");
  const doc = { format: "esl-layouts", version: 1, model: "2.9", icons: { notice: { w: 1, h: 1, data: "gA==" } },
    layouts: [{ name: "notice", fields: ["a"], elements: [{ type: "icon", icon: "notice", x: 0, y: 0 }] }] };
  const before = JSON.stringify(doc);
  const a = addTemplate(doc, t, "2.9");
  assert.equal(JSON.stringify(doc), before);
  assert.equal(a.index, 1);
  assert.equal(a.name, "notice-2");
  const l = a.doc.layouts[1];
  assert.equal(l._template, "notice");
  // the file's own "notice" picture is another one: the template's gets another name
  const icon = l.elements.find((e) => e.type === "icon").icon;
  assert.equal(icon, "notice_2");
  assert.deepEqual(a.doc.icons.notice, doc.icons.notice);
  assert.deepEqual(a.doc.icons[icon], t.variants["2.9"].icons.notice);
  // the same template again reuses its picture
  const b = addTemplate(a.doc, t, "2.9");
  assert.equal(b.name, "notice-3");
  assert.equal(b.doc.layouts[2].elements.find((e) => e.type === "icon").icon, "notice_2");
  assert.equal(Object.keys(b.doc.icons).length, 2);
  // and the result is a file the AP takes
  const r = await loadRenderer("2.9", WASM);
  r.setLimits(null);
  const res = r.loadLayouts(JSON.stringify(b.doc));
  assert.ok(res.ok, res.error);
  assert.equal(addTemplate(doc, { id: "y", variants: {} }, "2.9"), null);
  // an example layout offered when there is no catalog is not marked as a template
  const ex = addTemplate(doc, { id: "example-x", example: true, variants: { "2.9": { layout: { name: "x", fields: ["a"], elements: [] } } } }, "2.9");
  assert.equal(ex.doc.layouts[1]._template, undefined);
});

test("names and search", () => {
  assert.equal(freeName("a", new Set()), "a");
  assert.equal(freeName("a", new Set(["a", "a-2"])), "a-3");
  assert.equal(freeName("x".repeat(31), new Set(["x".repeat(31)])).length, 31);
  const list = [{ id: "a", title: "Guest WiFi", description: "", tags: ["qr"], fields: ["ssid"] },
    { id: "b", title: "Shelf price", description: "price", tags: [], fields: ["price"] }];
  assert.deepEqual(search(list, "").map((t) => t.id), ["a", "b"]);
  assert.deepEqual(search(list, "wifi").map((t) => t.id), ["a"]);
  assert.deepEqual(search(list, "QR ssid").map((t) => t.id), ["a"]);
  assert.deepEqual(search(list, "price").map((t) => t.id), ["b"]);
});
