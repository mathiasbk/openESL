// The AP's backup file (js/backup.js) against a simulated AP with the calls it uses
// (docs/openapi.yaml): read from one AP, put on a new one, and run again.
import { test } from "node:test";
import assert from "node:assert/strict";
import { makeBackup, restoreBackup, parseBackup, backupSummary, backupName, BACKUP_FORMAT } from "../js/backup.js";

const PIC = Buffer.from(Uint8Array.from({ length: 16 }, (_, i) => i)).toString("base64");

class FakeAp {
  constructor({ clock = true, webhook = true, radioKey = null } = {}) {
    this.clock = clock;
    this.radioKey = radioKey; // an AP with radio security has a key and radio_secure
    this.hasWebhook = webhook;
    this.config = { poll_s: 0, builtin_poll_s: 0, webhook: null };
    if (clock) Object.assign(this.config, { ntp: "pool.ntp.org", builtin_ntp: "pool.ntp.org", tz: null });
    if (radioKey) this.config.radio_secure = false;
    this.layouts = {};   // model -> file
    this.tags = new Map(); // mac -> { mac, model, content, config }
    this.scheduled = [];
    this.time = 1792065600;
    this.calls = [];
  }
  tag(mac) {
    if (!this.tags.has(mac)) this.tags.set(mac, { mac, model: null, content: null, config: {} });
    return this.tags.get(mac);
  }
  check(c) {
    if (c.layout !== "text" && c.layout !== "price" && c.layout !== "image" && !Object.values(this.layouts).some((f) => f.layouts?.[c.layout]))
      throw new Error("422: layout not in the file");
  }
  setContent(mac, c) {
    this.check(c);
    const { at, ...content } = c;
    if (at && at > this.time) this.scheduled.push({ mac, at, ...content });
    else this.tag(mac).content = content;
  }
  async call(method, path, body) {
    this.calls.push(method + " " + path);
    let m;
    if (method === "GET" && path === "/api/tags")
      return {
        ap: { fw: "0.8.1", board: "s3-n16r8-c6", board_name: "ESP32-S3 + C6 on cable", channel: 25, time: this.time, poll_s: this.config.poll_s },
        tags: [...this.tags.values()].map((t) => ({ mac: t.mac, model: t.model, content_id: t.content ? 7 : 0,
          config: { poll_s: null, max_poll_s: null, lost_ap_s: null, long_poll_s: null, long_after_min: null, ...t.config } })),
      };
    if (method === "GET" && path === "/api/config") return { ...this.config };
    if (method === "PUT" && path === "/api/config") {
      for (const k of Object.keys(body)) {
        if (!(k in this.config) || k.startsWith("builtin")) throw new Error("422: unknown");
        if (k === "webhook" && !this.hasWebhook) throw new Error("422: this AP has no webhook");
      }
      for (const [k, v] of Object.entries(body)) this.config[k] = v === null && "builtin_" + k in this.config ? this.config["builtin_" + k] : v;
      return { ...this.config };
    }
    if (path === "/api/radio-key" && this.radioKey) {
      if (method === "PUT") this.radioKey = body.key.toUpperCase();
      return { key: this.radioKey };
    }
    if (method === "GET" && path === "/api/wifi") return { hostname: "openesl", ssid: "home" };
    if (method === "PUT" && path === "/api/time") return {};
    if (method === "GET" && path === "/api/layouts")
      return { layouts: ["2.9", "1.54"].map((model) => ({ model, version: this.layouts[model] ? 1 : null })) };
    if ((m = path.match(/^\/api\/layouts\/(.+)$/))) {
      const model = decodeURIComponent(m[1]);
      if (method === "GET") return this.layouts[model];
      this.layouts[model] = body;
      return { model, changed: true };
    }
    if (method === "GET" && path === "/api/tag-firmware")
      return { images: [{ model: "2.9", version: 82, crc: 1234, tags: { pending: 1, failed: 0, done: 3 } }, { model: "1.54", version: null }] };
    if (method === "GET" && path === "/api/schedule") return { time: this.time, scheduled: this.scheduled };
    if (method === "PUT" && path === "/api/tags") {
      body.tags.forEach((t) => this.check(t)); // all or nothing, as the AP
      for (const { mac, ...c } of body.tags) this.setContent(mac, c);
      return { results: [] };
    }
    if ((m = path.match(/^\/api\/tags\/([0-9A-F]{16})\/config$/)) && method === "PUT") {
      this.tag(m[1]).config = { ...body };
      return {};
    }
    if ((m = path.match(/^\/api\/tags\/([0-9A-F]{16})\/image$/)) && method === "GET") {
      const t = this.tags.get(m[1]);
      return { mac: t.mac, width: 4, height: 32, red: false, data: t.content.picture };
    }
    if ((m = path.match(/^\/api\/tags\/([0-9A-F]{16})$/))) {
      if (method === "GET") {
        const t = this.tags.get(m[1]);
        return { mac: t.mac, fields: [], price_highlight: false, frame: false, ...t.content, picture: undefined };
      }
      this.setContent(m[1], body);
      return {};
    }
    throw new Error("404: " + method + " " + path);
  }
  async send(method, path, bytes) {
    this.calls.push(method + " " + path);
    const m = path.match(/^\/api\/tags\/([0-9A-F]{16})\/image$/);
    const t = this.tags.get(m[1]);
    if (!t?.model) return { status: 409, body: new TextEncoder().encode('{"error":"model not known"}') };
    t.content = { layout: "image", fields: [], picture: Buffer.from(bytes).toString("base64") };
    return { status: 200, body: new TextEncoder().encode("{}") };
  }
}

function filledAp() {
  const ap = new FakeAp();
  ap.config = { ...ap.config, poll_s: 120, webhook: "http://hook.local/", tz: "CET-1CEST,M3.5.0,M10.5.0/3" };
  ap.layouts["2.9"] = { layouts: { shelf: { fields: 3 } } };
  Object.assign(ap.tag("0000000000000001"), { model: "2.9", content: { layout: "shelf", fields: ["Milk", "24.90", "1 L"], price_highlight: true } });
  Object.assign(ap.tag("0000000000000002"), { model: "2.9", content: { layout: "image", fields: [], picture: PIC } });
  Object.assign(ap.tag("0000000000000003"), { model: "1.54", config: { poll_s: 600 } });
  ap.tag("0000000000000004").model = "1.54"; // nothing of its own: not in the backup
  ap.scheduled.push({ mac: "0000000000000001", at: ap.time + 3600, layout: "shelf", fields: ["Milk", "19.90", "Offer"], price_highlight: true, frame: false });
  return ap;
}

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
  assert.deepEqual(backupSummary(b), { layouts: ["2.9"], tags: 1, pictures: 1, configs: 1, scheduled: 1 });
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

test("the radio key and \"only paired tags\" go along, the key first", async () => {
  const old = filledAp();
  old.radioKey = "00112233445566778899AABBCCDDEEFF";
  old.config.radio_secure = true;
  const b = parseBackup(JSON.stringify(await makeBackup(old)));
  assert.equal(b.radio_key, "00112233445566778899AABBCCDDEEFF");
  assert.equal(b.config.radio_secure, true);
  const ap = new FakeAp({ radioKey: "FFEEDDCCBBAA99887766554433221100" });
  const r = await restoreBackup(ap, b);
  assert.deepEqual(r.failed, []);
  assert.equal(ap.radioKey, "00112233445566778899AABBCCDDEEFF");
  assert.equal(ap.config.radio_secure, true);
  assert.ok(ap.calls.indexOf("PUT /api/radio-key") < ap.calls.indexOf("PUT /api/config"), "the key before radio_secure");
  // an AP without radio security, and one already with the key: nothing to put
  const plain = new FakeAp();
  assert.ok(!("radio_key" in await makeBackup(plain)));
  const same = new FakeAp({ radioKey: b.radio_key });
  await restoreBackup(same, b, { parts: { layouts: false, tags: false, scheduled: false } });
  assert.ok(!same.calls.includes("PUT /api/radio-key"));
  assert.throws(() => parseBackup(JSON.stringify({ ...b, radio_key: "12" })), /radio key/);
});
