// The radio key (js/radiokey.js): the key file is a backup that restores only the key, and the
// browser's copies are only added to, never replaced
import { test } from "node:test";
import assert from "node:assert/strict";
import { fingerprint, groupKey, apLabel, keyBackup, keyFileName, keptKeys, keepKey, dropKey, isKept, isSaved, markSaved, readKey } from "../js/radiokey.js";
import { parseBackup, restoreBackup, backupSummary } from "../js/backup.js";

const KEY = "00112233445566778899AABBCCDDEEFF";
const OTHER = "FFEEDDCCBBAA99887766554433221100";

const memory = () => {
  const m = new Map();
  return { get: (k) => m.get(k) ?? null, set: (k, v) => (m.set(k, v), true), m };
};

test("the key is shown in groups of four, and its fingerprint does not give it away", () => {
  assert.equal(groupKey(KEY.toLowerCase()), "0011 2233 4455 6677 8899 AABB CCDD EEFF");
  assert.equal(fingerprint(KEY), fingerprint(KEY.toLowerCase()));
  assert.notEqual(fingerprint(KEY), fingerprint(OTHER));
  assert.match(fingerprint(KEY), /^[0-9a-f]{8}$/);
});

test("the key file is a backup with only the key, and restores only the key", async () => {
  const b = keyBackup(KEY.toLowerCase(), { hostname: "openesl", ip: "192.168.0.170", board: "C6", fw: "0.9.0", junk: 1 }, new Date("2026-10-30T12:34:56.789Z"));
  assert.equal(keyFileName(b), "openesl-radio-key-openesl-2026-10-30.json");
  assert.deepEqual(b.ap, { hostname: "openesl", ip: "192.168.0.170", board: "C6", fw: "0.9.0" });
  const back = parseBackup(JSON.stringify(b));
  assert.equal(back.radio_key, KEY);
  assert.deepEqual(backupSummary(back), { layouts: [], tags: 0, pictures: 0, configs: 0, scheduled: 0 });

  // onto an AP with another key and its own settings: only the key changes
  const calls = [];
  let apKey = OTHER;
  const ap = {
    call: async (method, path, body) => {
      calls.push(`${method} ${path}`);
      if (path === "/api/tags") return { ap: { fw: "0.9.0" }, tags: [] };
      if (path === "/api/radio-key") {
        if (method === "PUT") apKey = body.key.toUpperCase();
        return { key: apKey };
      }
      if (path === "/api/config" && method === "GET") return { poll_s: 600, webhook: "http://ha/x", radio_secure: true };
      if (path === "/api/wifi") return { hostname: "openesl" };
      throw new Error("unexpected " + method + " " + path);
    },
  };
  const r = await restoreBackup(ap, back);
  assert.equal(apKey, KEY);
  assert.deepEqual(r.failed, []);
  assert.ok(!calls.includes("PUT /api/config"), calls.join(", "));
  assert.equal(keyFileName(keyBackup(KEY, { setup_ssid: "openesl-A3AC" }, new Date("2026-10-30T00:00:00Z"))), "openesl-radio-key-openesl-A3AC-2026-10-30.json");
});

test("copies in the browser are added, never replaced, and can be removed", () => {
  const s = memory();
  assert.deepEqual(keptKeys(s), []);
  assert.equal(keepKey("not a key", {}, s), false);
  assert.ok(keepKey(KEY.toLowerCase(), { hostname: "openesl", ip: "192.168.0.170" }, s, new Date("2026-10-30T10:00:00Z")));
  // the same AP again with a new address: one entry, the newer details, the first date
  assert.ok(keepKey(KEY, { ip: "192.168.0.171" }, s, new Date("2026-10-30T11:00:00Z")));
  // the AP got another key (a restore or an erase): the old one stays
  assert.ok(keepKey(OTHER, { hostname: "openesl" }, s, new Date("2026-10-30T12:00:00Z")));
  const list = keptKeys(s);
  assert.deepEqual(list.map((e) => e.key), [OTHER, KEY]);
  assert.deepEqual(list[1].ap, { hostname: "openesl", ip: "192.168.0.171" });
  assert.equal(list[1].saved, "2026-10-30T10:00:00.000Z");
  assert.equal(apLabel(list[1].ap), "openesl · 192.168.0.171");
  assert.ok(isKept(KEY.toLowerCase(), s));
  dropKey(KEY, s);
  assert.ok(!isKept(KEY, s));
  assert.equal(keptKeys(s).length, 1);
  // a broken entry does not break the list
  s.set("openesl.radiokeys", "{oops");
  assert.deepEqual(keptKeys(s), []);
});

test("saved is remembered by fingerprint only", () => {
  const s = memory();
  assert.ok(!isSaved(KEY, s));
  markSaved(KEY, s);
  assert.ok(isSaved(KEY.toLowerCase(), s));
  assert.ok(!isSaved(OTHER, s));
  assert.ok(![...s.m.values()].some((v) => v.includes(KEY)), "the key itself is not stored");
});

test("an AP without radio security has no key; other errors are thrown", async () => {
  const err = (status) => ({ call: async () => { throw Object.assign(new Error("x"), { status }); } });
  assert.equal(await readKey({ call: async () => ({ key: KEY.toLowerCase() }) }), KEY);
  assert.equal(await readKey(err(404)), null);
  assert.equal(await readKey({ call: async () => { throw new Error("404: not found"); } }), null);
  await assert.rejects(readKey(err(500)));
});
