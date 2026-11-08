// The AP's backup file (js/backup.js) against a simulated AP with the calls it uses
// (docs/openapi.yaml): read from one AP, put on a new one, and run again.
import { test } from "node:test";
import assert from "node:assert/strict";
import { makeBackup, restoreBackup, parseBackup, backupSummary, backupName, BACKUP_FORMAT } from "../js/backup.js";
import { FakeAp, filledAp, PIC } from "./fake_backup_ap.mjs";

test("a backup holds settings, layouts, content, pictures, own settings and scheduled content", async () => {
  const steps = [];
  const b = await makeBackup(filledAp(), { progress: (p) => steps.push(p), now: () => new Date("2026-10-26T17:00:00.123Z") });
  assert.equal(b.format, BACKUP_FORMAT);
  assert.equal(b.version, 1);
  assert.equal(b.taken, "2026-10-26T17:00:00+00:00", "as esl-client writes it");
  assert.deepEqual(b.ap, { fw: "0.8.1", channel: 25, board: "s3-n16r8-c6", hostname: "openesl" });
  assert.deepEqual(b.config, { poll_s: 120, webhook: "http://hook.local/", ntp: null, tz: "CET-1CEST,M3.5.0,M10.5.0/3" });
  assert.deepEqual(Object.keys(b.layouts), ["2.9"]);
  assert.deepEqual(b.tags, [
    { mac: "0000000000000001", layout: "shelf", fields: ["Milk", "24.90", "1 L"], price_highlight: true, frame: false },
    { mac: "0000000000000002", image: PIC },
    { mac: "0000000000000003", config: { poll_s: 600 } },
  ]);
  assert.deepEqual(b.scheduled, [{ mac: "0000000000000001", at: 1792065600 + 3600, layout: "shelf", fields: ["Milk", "19.90", "Offer"], price_highlight: true, frame: false }]);
  assert.deepEqual(b.tag_firmware, [{ model: "2.9", version: 82, crc: 1234, waiting: 1 }]);
  assert.equal(steps.at(-1).done, steps.at(-1).total);
  assert.equal(backupName(b), "openesl-backup-openesl-2026-10-26.json");
  assert.deepEqual(backupSummary(b), { fonts: [], layouts: ["2.9"], tags: 1, pictures: 1, configs: 1, scheduled: 1 });
  assert.ok(!JSON.stringify(b).includes("token"), "never the token");
});

test("restored on a new AP, and run again once the tag with the picture has polled", async () => {
  const b = parseBackup(JSON.stringify(await makeBackup(filledAp())));
  const ap = new FakeAp();
  ap.tag("0000000000000009").model = "2.9"; // a tag the backup does not have is left as it is
  const r = await restoreBackup(ap, b);
  assert.deepEqual(r.failed, []);
  assert.deepEqual(r.waiting, ["0000000000000002"], "the AP has not heard that tag: its model is not known");
  assert.equal(r.notes.length, 1);
  assert.match(r.notes[0], /tag firmware 82, 1 tags/);
  assert.equal(ap.config.poll_s, 120);
  assert.equal(ap.config.tz, "CET-1CEST,M3.5.0,M10.5.0/3");
  assert.equal(ap.config.ntp, "pool.ntp.org");
  assert.deepEqual(ap.layouts["2.9"], { layouts: { shelf: { fields: 3 } } });
  assert.deepEqual(ap.tags.get("0000000000000001").content, { layout: "shelf", fields: ["Milk", "24.90", "1 L"], price_highlight: true, frame: false });
  assert.deepEqual(ap.tags.get("0000000000000003").config, { poll_s: 600 });
  assert.equal(ap.tags.get("0000000000000009").content, null);
  assert.equal(ap.scheduled.length, 1);
  // the layout file goes on before the content that names it
  assert.ok(ap.calls.indexOf("PUT /api/layouts/2.9") < ap.calls.indexOf("PUT /api/tags"));

  ap.tag("0000000000000002").model = "2.9";
  const again = await restoreBackup(ap, b, { parts: { settings: false, layouts: false, scheduled: false } });
  assert.deepEqual(again.waiting, []);
  assert.deepEqual(again.failed, []);
  assert.equal(ap.tags.get("0000000000000002").content.picture, PIC);
});

test("a file from esl_client.py backup goes back too, and scheduled entries in the past are skipped", async () => {
  // as tools/esl-client/esl_client.py backup() writes it (no ntp or tz)
  const file = JSON.stringify({
    format: "openesl-backup", version: 1, taken: "2026-10-20T10:00:00+00:00", ap: { fw: "0.7.0", channel: 25, hostname: "butikk" },
    config: { poll_s: 60, webhook: null }, layouts: {},
    tags: [{ mac: "0000000000000001", layout: "text", fields: ["Hello"] }],
    scheduled: [{ mac: "0000000000000001", at: 1000, layout: "text", fields: ["Old"] }],
    tag_firmware: [],
  });
  const ap = new FakeAp();
  const r = await restoreBackup(ap, parseBackup(file));
  assert.deepEqual(r.failed, []);
  assert.equal(ap.config.poll_s, 60);
  assert.equal(ap.config.tz, null, "left as it was");
  assert.deepEqual(ap.tags.get("0000000000000001").content, { layout: "text", fields: ["Hello"] });
  assert.equal(ap.scheduled.length, 0);
  assert.ok(r.notes.some((n) => /1 scheduled entries skipped/.test(n)));
  assert.ok(r.notes.some((n) => /called butikk/.test(n)));
});

test("an older AP: no clock settings, no webhook; nothing asked of it that it lacks", async () => {
  const b = await makeBackup(filledAp());
  b.config.webhook = null;
  const ap = new FakeAp({ clock: false, webhook: false });
  const r = await restoreBackup(ap, b, { parts: { layouts: false, tags: false, scheduled: false } });
  assert.deepEqual(r.failed, []);
  assert.equal(ap.config.poll_s, 120);
  assert.ok(!("tz" in ap.config));
  assert.equal(r.done[0], "Settings: poll_s");
});

test("one bad entry in a batch does not stop the others, and is named", async () => {
  const b = await makeBackup(filledAp());
  b.tags.push({ mac: "00000000000000AA", layout: "gone", fields: [] });
  const ap = new FakeAp();
  const r = await restoreBackup(ap, b, { parts: { scheduled: false } });
  assert.equal(r.failed.length, 1);
  assert.match(r.failed[0], /00000000000000AA: 422/);
  assert.ok(ap.tags.get("0000000000000001").content);
});

test("files that are not a backup are turned down", () => {
  const ok = { format: BACKUP_FORMAT, version: 1, config: {}, layouts: {}, tags: [], scheduled: [] };
  assert.throws(() => parseBackup("nope"), /not JSON/);
  assert.throws(() => parseBackup('{"a":1}'), /not an OpenESL backup/);
  assert.throws(() => parseBackup(JSON.stringify({ ...ok, version: 2 })), /version 2/);
  assert.throws(() => parseBackup(JSON.stringify({ ...ok, tags: {} })), /"tags" is missing/);
  assert.throws(() => parseBackup(JSON.stringify({ ...ok, tags: [{ mac: "x" }] })), /no valid MAC/);
  assert.throws(() => parseBackup(JSON.stringify({ ...ok, tags: [{ mac: "0000000000000001", layout: "text" }] })), /no fields/);
  assert.throws(() => parseBackup(JSON.stringify({ ...ok, tags: [{ mac: "0000000000000001", image: "!!" }] })), /not base64/);
  assert.deepEqual(parseBackup(JSON.stringify(ok)).tags, []);
});

test("a layout file for a model that now uses another model's file (same_as) is skipped with a note", async () => {
  const b = await makeBackup(filledAp());
  b.layouts["2.9b"] = b.layouts["2.9"];
  const ap = new FakeAp();
  const call = ap.call.bind(ap);
  ap.call = (method, path, body) =>
    method === "PUT" && path === "/api/layouts/2.9b"
      ? Promise.reject(new Error("409: model 2.9b uses the layout file of model 2.9 (same_as): upload it with PUT /api/layouts/2.9"))
      : call(method, path, body);
  const r = await restoreBackup(ap, b, { parts: { scheduled: false } });
  assert.deepEqual(r.failed, []);
  assert.ok(r.notes.some((n) => n === 'The 2.9b" layout file was skipped: model 2.9b uses the layout file of model 2.9 (same_as): upload it with PUT /api/layouts/2.9'));
});

test("font files go in the backup and back before the layout files", async () => {
  const font = { format: "esl-font", version: 1, name: "text28", height: 10, baseline: 8, first: 65, last: 65, widths: [1], bitmap: "/8A=" };
  const old = filledAp();
  old.fonts = { text28: font };
  old.layouts["2.9"] = { layouts: { shelf: { fields: 3 } }, fonts: ["text28"] };
  const b = parseBackup(JSON.stringify(await makeBackup(old)));
  assert.deepEqual(b.fonts, { text28: font });
  assert.deepEqual(backupSummary(b).fonts, ["text28"]);
  const ap = new FakeAp();
  ap.fonts = {};
  const r = await restoreBackup(ap, b);
  assert.deepEqual(r.failed, []);
  assert.deepEqual(ap.fonts, { text28: font });
  assert.ok(ap.calls.indexOf("PUT /api/fonts/text28") < ap.calls.indexOf("PUT /api/layouts/2.9"));
  assert.throws(() => parseBackup(JSON.stringify({ ...b, fonts: [] })), /fonts/);
});

test("the date language (AP 0.11.0) is kept, and left out on an AP without it", async () => {
  const from = filledAp();
  from.config.lang = "nb";
  const b = parseBackup(JSON.stringify(await makeBackup(from)));
  assert.equal(b.config.lang, "nb");
  const ap = new FakeAp();
  ap.config.lang = "en";
  await restoreBackup(ap, b);
  assert.equal(ap.config.lang, "nb");
  const old = new FakeAp(); // firmware before 0.11.0: no lang, and nothing sent for it
  await restoreBackup(old, b);
  assert.ok(!("lang" in old.config));
});
