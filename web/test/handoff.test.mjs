// Handing a layout file from the designer to the AP's own page (js/handoff.js)
import { test } from "node:test";
import assert from "node:assert/strict";
import { apPageUrl, trustedOrigin, layoutsMessage, readLayoutsMessage, compareFiles, LAYOUTS } from "../js/handoff.js";

test("apPageUrl gives the AP's dashboard on http", () => {
  assert.deepEqual(apPageUrl(""), { url: "http://openesl.local/dashboard?from=designer" });
  assert.deepEqual(apPageUrl(" 192.168.0.170 "), { url: "http://192.168.0.170/dashboard?from=designer" });
  assert.deepEqual(apPageUrl("http://butikk.local/"), { url: "http://butikk.local/dashboard?from=designer" });
  assert.deepEqual(apPageUrl("http://192.168.0.170:8080/dashboard"), { url: "http://192.168.0.170:8080/dashboard?from=designer" });
  assert.match(apPageUrl("https://openesl.local").error, /http:\/\/openesl\.local/);
  assert.match(apPageUrl("http://").error, /not an address/);
});

test("trustedOrigin: the published page and pages on this computer", () => {
  assert.equal(trustedOrigin("https://mathiasbk.github.io"), true);
  assert.equal(trustedOrigin("http://localhost:8000"), true);
  assert.equal(trustedOrigin("http://127.0.0.1:5500"), true);
  assert.equal(trustedOrigin("https://evil.example"), false);
  assert.equal(trustedOrigin("http://mathiasbk.github.io"), false);
  assert.equal(trustedOrigin("null"), false);
});

const file = { format: "esl-layouts", version: 1, model: "2.9", layouts: [{ name: "shelf" }, { name: "sign" }] };

test("a message from the designer is read back", () => {
  const preview = "data:image/png;base64,iVBORw0KGgo=";
  const content = { layout: "shelf", fields: ["Milk", "24,90"] };
  const m = readLayoutsMessage(layoutsMessage({ model: "2.9", file, layout: "shelf", content, preview }));
  assert.deepEqual(m, { model: "2.9", file, layout: "shelf", content, preview });
});

test("a message that is not one is refused, a bad preview or content left out", () => {
  assert.ok(readLayoutsMessage(null).error);
  assert.ok(readLayoutsMessage({ type: "other" }).error);
  assert.match(readLayoutsMessage({ type: LAYOUTS, v: 2, model: "2.9", file }).error, /another version/);
  assert.match(readLayoutsMessage({ type: LAYOUTS, v: 1, model: "2.9", file: { layouts: [] } }).error, /not a layout file/);
  assert.match(readLayoutsMessage({ type: LAYOUTS, v: 1, model: "1.54", file }).error, /for 2\.9", sent for 1\.54"/);
  const m = readLayoutsMessage({ type: LAYOUTS, v: 1, model: "2.9", file, preview: "javascript:alert(1)", content: { layout: 1 } });
  assert.equal(m.preview, "");
  assert.equal(m.content, null);
});

test("compareFiles says what a new file adds and removes", () => {
  const current = { layouts: [{ name: "shelf" }, { name: "guest-wifi" }] };
  assert.deepEqual(compareFiles(file, current), { names: ["shelf", "sign"], added: ["sign"], removed: ["guest-wifi"], kept: ["shelf"] });
  assert.deepEqual(compareFiles(file, null), { names: ["shelf", "sign"], added: ["shelf", "sign"], removed: [], kept: [] });
});
