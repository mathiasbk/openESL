// #307: the backup files of earlier releases still go back with today's dashboard. The files are
// what each release wrote: from the dashboard (compat/<release>-dashboard-backup.json,
// compat/make_backup.mjs) and from esl-client (tools/esl-client/compat/<release>-backup.json),
// which writes the same format. Each is restored on a new simulated AP, and the AP must then hold
// everything in the file.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { parseBackup, restoreBackup, backupSummary } from "../js/backup.js";
import { FakeAp } from "./fake_backup_ap.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "..", "..");
const DASH = join(HERE, "compat");
const CLIENT = join(ROOT, "tools", "esl-client", "compat");
const CONTENT_KEYS = ["layout", "fields", "price_highlight", "frame"];
const cmp = (a, b) => {
  const x = a.slice(1).split(".").map(Number), y = b.slice(1).split(".").map(Number);
  return x[0] - y[0] || x[1] - y[1] || x[2] - y[2];
};
const files = (dir, re) => readdirSync(dir).filter((f) => re.test(f)).sort().map((f) => join(dir, f));
const pick = (o, keys) => Object.fromEntries(keys.filter((k) => o[k] != null).map((k) => [k, o[k]]));

const dashboard = files(DASH, /^v[\d.]+-dashboard-backup\.json$/);
const client = files(CLIENT, /^v[\d.]+-backup\.json$/);

test("every release since 0.9.0 with compatibility files has a dashboard backup file", () => {
  const releases = readdirSync(join(ROOT, "ap", "test", "compat")).filter((f) => /^v[\d.]+\.txt$/.test(f)).map((f) => f.slice(0, -4));
  const want = releases.filter((r) => cmp(r, "v0.9.0") >= 0);
  assert.ok(want.length >= 2);
  assert.deepEqual(want.filter((r) => !dashboard.some((f) => f.endsWith(`/${r}-dashboard-backup.json`))), [],
    "add it in the VERSION PR: node web/test/compat/make_backup.mjs v<previous>");
});

for (const path of [...dashboard, ...client]) {
  const name = path.slice(ROOT.length + 1);
  test(`${name} goes back on a new AP with today's dashboard`, async () => {
    const b = parseBackup(readFileSync(path, "utf8"));
    const sum = backupSummary(b);
    // not an empty file: what make_backup.mjs and make_backup.py put in it
    assert.ok(sum.layouts.length >= 1 && sum.tags >= 3 && sum.pictures === 1 && sum.configs === 1 && sum.scheduled === 1, JSON.stringify(sum));

    const ap = new FakeAp();
    ap.fonts = {}; // an AP that takes font files
    for (const t of b.tags) if ("image" in t) ap.tag(t.mac).model = "2.9"; // the tag with the picture has polled it
    const r = await restoreBackup(ap, b);
    assert.deepEqual(r.failed, []);
    assert.deepEqual(r.waiting, []);

    for (const k of ["poll_s", "webhook", "tz"]) if (b.config[k] != null) assert.equal(ap.config[k], b.config[k], k);
    assert.deepEqual(ap.fonts, b.fonts || {});
    for (const [model, file] of Object.entries(b.layouts)) assert.deepEqual(ap.layouts[model], file, model);
    for (const t of b.tags) {
      const got = ap.tags.get(t.mac);
      if ("layout" in t) assert.deepEqual(got.content, pick(t, CONTENT_KEYS), t.mac);
      if ("image" in t) assert.equal(got.content.picture, t.image, t.mac);
      if (t.config) assert.deepEqual(got.config, t.config, t.mac);
    }
    assert.deepEqual(ap.scheduled.map((e) => [e.mac, e.at]), b.scheduled.map((e) => [e.mac, e.at]));
    if (b.tag_firmware?.some((f) => f.waiting)) assert.ok(r.notes.some((n) => /tag firmware/.test(n)));
  });
}
