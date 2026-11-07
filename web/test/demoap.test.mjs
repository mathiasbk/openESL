// The browser demo's pretend AP (js/demoap.js) must answer as the AP's API does
// (docs/openapi.yaml), so the dashboard and the designer work against it as against an AP.
//
//   python web/build.py && node --test web/test/demoap.test.mjs
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";
import { DemoAp, DEMO_TAGS, demoWanted, crc32, memoryStorage } from "../js/demoap.js";
import { decodeTagImage, healthLine, mergeTagList } from "../js/dashdata.js";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const WASM = new URL("../dist/wasm/", import.meta.url);
const example = async (m) => JSON.parse(readFileSync(path.join(ROOT, "tag", "designs", "layouts", m + ".json"), "utf8"));

function demo() {
  let now = Date.UTC(2026, 10, 7, 12, 0, 0);
  const ap = new DemoAp({ now: () => now, layoutFile: example, wasm: WASM });
  ap.wait = (s) => { now += s * 1000; };
  return ap;
}

const status = async (p) => {
  try {
    await p;
  } catch (e) {
    return e.status;
  }
  return 200;
};

const MILK = DEMO_TAGS[0].mac;      // 2.9", shelf layout from the file
const NEW = DEMO_TAGS.find((t) => !t.content).mac;
const RED = DEMO_TAGS.find((t) => t.model === "2.9-bwr").mac;

test("only ?demo=browser starts it", () => {
  assert.equal(demoWanted("?demo=browser"), true);
  assert.equal(demoWanted("?demo"), false);
  assert.equal(demoWanted("?demo=http://localhost:8080"), false);
  assert.equal(demoWanted(""), false);
});

test("the demo's storage forgets and keeps only its seed", () => {
  const s = memoryStorage({ "openesl.theme": "dark" });
  assert.equal(s.getItem("openesl.theme"), "dark");
  assert.equal(s.getItem("openesl.doc.2.9"), null);
  s.setItem("openesl.doc.2.9", 1);
  assert.equal(s.getItem("openesl.doc.2.9"), "1");
  assert.equal(s.length, 2);
  s.removeItem("openesl.doc.2.9");
  assert.equal(s.key(1), null);
  s.clear();
  assert.equal(s.length, 0);
});

test("crc32 is the usual one", () => {
  assert.equal(crc32(new TextEncoder().encode("123456789")), 0xcbf43926);
});

test("six tags, one new, one with a low battery", async () => {
  const ap = demo();
  const r = await ap.call("GET", "/api/tags");
  assert.equal(r.tags.length, 6);
  assert.equal(r.ap.max_tags, 400);
  assert.ok(r.ap.uptime_s > 86400);
  const fresh = r.tags.find((t) => t.mac === NEW);
  assert.equal(fresh.content_id, 0);
  assert.equal("layout" in fresh, false);
  for (const t of r.tags.filter((t) => t.mac !== NEW)) {
    assert.equal(t.pending, false, t.mac);
    assert.equal(t.signal.rssi.length, 8);
  }
  // the designer's shortcuts, as ApClient has them
  assert.equal((await ap.tags()).tags.length, 6);
  assert.equal((await ap.layouts()).layouts.length, 3);
  // the dashboard's own reading of the list
  assert.equal(mergeTagList(null, r, new Map()).tags.length, 6);
  const h = await ap.call("GET", "/api/health");
  assert.equal(h.low_battery, 1);
  assert.match(healthLine(h), /1 of 6 tags needs attention/);
});

test("every tag with content has a screen drawn by the AP's renderer", async () => {
  const ap = demo();
  for (const d of DEMO_TAGS.filter((t) => t.content)) {
    const img = decodeTagImage(await ap.call("GET", `/api/tags/${d.mac}/image`));
    assert.ok(img, d.mac);
    assert.ok(img.pixels.some((p) => p === 1), d.mac + " has ink");
  }
  const red = decodeTagImage(await ap.call("GET", `/api/tags/${RED}/image`));
  assert.ok(red.pixels.some((p) => p === 2), "the red band is red");
  assert.equal(await status(ap.call("GET", `/api/tags/${NEW}/image`)), 404);
});

test("new content waits for the next poll, then is shown", async () => {
  const ap = demo();
  const r = await ap.call("PUT", `/api/tags/${NEW}`, { layout: "text", fields: ["Hello", "from the demo"] });
  assert.equal(r.changed, true);
  assert.equal(r.pending, true);
  const again = await ap.call("PUT", `/api/tags/${NEW}`, { layout: "text", fields: ["Hello", "from the demo"] });
  assert.equal(again.changed, false);
  const d = await ap.call("GET", `/api/tags/${NEW}`);
  assert.deepEqual(d.fields, ["Hello", "from the demo"]);
  assert.equal(d.layout, "text");
  ap.wait(10);
  const t = (await ap.call("GET", "/api/tags")).tags.find((x) => x.mac === NEW);
  assert.equal(t.pending, false);
  assert.equal(t.shown_image_id, t.image_id);
  assert.ok(t.seen_s_ago < 10);
});

test("fields by name, and what the AP refuses", async () => {
  const ap = demo();
  await ap.call("PUT", `/api/tags/${MILK}`, { layout: "shelf", fields: { name: "Cheese", price: "99.00" } });
  assert.deepEqual((await ap.call("GET", `/api/tags/${MILK}`)).fields, ["Cheese", "99.00"]);
  assert.equal(await status(ap.call("PUT", `/api/tags/${MILK}`, { layout: "nope", fields: [] })), 422);
  assert.equal(await status(ap.call("PUT", `/api/tags/${MILK}`, { layout: "price", fields: ["a", "b", "c", "d"] })), 422);
  assert.equal(await status(ap.call("PUT", `/api/tags/${MILK}`, { layout: "shelf", fields: { colour: "x" } })), 422);
  assert.equal(await status(ap.call("PUT", "/api/tags/0000000000000000", { layout: "text", fields: ["x"] })), 404);
  assert.equal(await status(ap.call("GET", "/api/nothing")), 404);
});

test("a batch is all or nothing", async () => {
  const ap = demo();
  const before = (await ap.call("GET", `/api/tags/${MILK}`)).content_id;
  await assert.rejects(ap.call("PUT", "/api/tags", { tags: [
    { mac: MILK, layout: "text", fields: ["changed"] },
    { mac: NEW, layout: "nope", fields: [] },
  ] }), (e) => e.status === 422 && e.message.includes("nope"));
  assert.equal((await ap.call("GET", `/api/tags/${MILK}`)).content_id, before);
  const r = await ap.call("PUT", "/api/tags", { tags: [
    { mac: MILK, layout: "text", fields: ["changed"] },
    { mac: NEW, layout: "price", fields: ["Tea", "19.90"] },
  ] });
  assert.equal(r.tags.length, 2);
  assert.ok(r.tags.every((x) => x.changed));
});

test("scheduled content is set when its time comes", async () => {
  const ap = demo();
  const at = Math.floor(ap.now() / 1000) + 60;
  const r = await ap.call("PUT", `/api/tags/${NEW}`, { layout: "text", fields: ["Later"], at });
  assert.deepEqual(r, { mac: NEW, scheduled: true, at });
  assert.equal((await ap.call("GET", "/api/tags")).ap.scheduled, 1);
  assert.equal((await ap.call("GET", `/api/tags/${NEW}`)).scheduled.length, 1);
  ap.wait(61);
  const d = await ap.call("GET", `/api/tags/${NEW}`);
  assert.deepEqual(d.fields, ["Later"]);
  assert.equal(d.scheduled.length, 0);
});

test("layout files: upload, a new version, and removal holds back the tags that used it", async () => {
  const ap = demo();
  const list = await ap.call("GET", "/api/layouts");
  assert.equal(list.layouts.length, 3);
  const e29 = list.layouts.find((l) => l.model === "2.9");
  assert.ok(e29.version && e29.layouts.includes("shelf"));
  assert.ok(list.limits.textBytes > 0);
  // ellipses and polygons: an AP without these limits is taken as too old for them
  assert.ok(list.limits.shapes > 0 && list.limits.polygonPoints >= 3);
  const file = await ap.call("GET", "/api/layouts/2.9");
  assert.equal(file.model, "2.9");
  const same = await ap.call("PUT", "/api/layouts/2.9", file);
  assert.equal(same.changed, false);
  file.layouts[0].elements = file.layouts[0].elements.slice(0, 1);
  const r = await ap.call("PUT", "/api/layouts/2.9", file);
  assert.equal(r.changed, true);
  assert.notEqual(r.version, e29.version);
  assert.equal(await status(ap.call("PUT", "/api/layouts/2.9", { ...file, model: "1.54" })), 422);
  assert.equal(await status(ap.call("PUT", "/api/layouts/2.9", { format: "esl-layouts", version: 1, model: "2.9", layouts: [{ name: "x" }] })), 422);
  assert.equal((await ap.call("GET", "/api/layouts/2.9")).layouts[0].elements.length, 1, "a refused file keeps the old one");
  assert.deepEqual(await ap.call("DELETE", "/api/layouts/2.9"), { model: "2.9", deleted: true });
  const t = (await ap.call("GET", "/api/tags")).tags.find((x) => x.mac === MILK);
  assert.match(t.content_error, /shelf/);
  assert.equal(await status(ap.call("GET", `/api/tags/${MILK}/image`)), 409);
  assert.equal((await ap.call("GET", "/api/health")).content_error, 2, "the shelf and the sign");
});

test("a picture in the raw format, and firmware is refused", async () => {
  const ap = demo();
  const raw = new Uint8Array(296 * 16);   // 296 x 128: 16 bytes per column
  raw.fill(0xff, 0, 16);
  const r = await ap.send("PUT", `/api/tags/${MILK}/image?dither=false`, raw);
  assert.equal(r.status, 200);
  assert.equal((await ap.call("GET", `/api/tags/${MILK}`)).layout, "image");
  assert.equal((await ap.send("PUT", `/api/tags/${MILK}/image`, new Uint8Array(10))).status, 422);
  assert.equal((await ap.send("POST", "/api/tag-firmware/2.9", new Uint8Array(10))).status, 501);
});

test("a deleted tag comes back without content when it polls", async () => {
  const ap = demo();
  await ap.call("DELETE", `/api/tags/${MILK}`);
  assert.equal((await ap.call("GET", "/api/tags")).tags.length, 5);
  ap.wait(10);
  const t = (await ap.call("GET", "/api/tags")).tags.find((x) => x.mac === MILK);
  assert.equal(t.content_id, 0);
});

test("tag settings wait for the next poll", async () => {
  const ap = demo();
  const r = await ap.call("PUT", `/api/tags/${MILK}/config`, { poll_s: 120 });
  assert.equal(r.changed, true);
  assert.equal(r.config_pending, true);
  ap.wait(10);
  assert.equal((await ap.call("GET", `/api/tags/${MILK}/config`)).config_pending, false);
});
