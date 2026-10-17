// The USB-port API client (js/apserial.js) against a fake AP that answers "@ESL" frames
// like ap/main/serial_api.c, with log lines in between.
import { test } from "node:test";
import assert from "node:assert/strict";
import { SerialAp, describeWifi, checkWifi, checkToken, checkHostname, newToken } from "../js/apserial.js";

const enc = new TextEncoder();
const dec = new TextDecoder();

// A transport with the interface of js/zbs.js. handle(req) returns the bytes the AP writes,
// or null for no answer.
function fakeAp(handle) {
  let rx = [];
  const waiters = [];
  const seen = [];
  let pending = new Uint8Array(0);
  const push = (bytes) => {
    rx.push(...bytes);
    waiters.splice(0).forEach((f) => f());
  };
  return {
    seen,
    push,
    async write(data) {
      const b = new Uint8Array(pending.length + data.length);
      b.set(pending);
      b.set(data, pending.length);
      pending = b;
      for (;;) {
        const nl = pending.indexOf(10);
        if (nl < 0) return;
        const [, seq, method, uri, len] = dec.decode(pending.subarray(0, nl)).split(" ");
        if (pending.length < nl + 1 + Number(len)) return;
        const body = dec.decode(pending.subarray(nl + 1, nl + 1 + Number(len)));
        pending = pending.slice(nl + 1 + Number(len));
        const req = { seq, method, uri, body: body ? JSON.parse(body) : null };
        seen.push(req);
        const out = handle(req);
        if (out) setTimeout(() => push(enc.encode(out)), 1);
      }
    },
    async read(n, timeoutMs) {
      if (!rx.length) {
        await new Promise((r) => {
          const t = setTimeout(r, timeoutMs);
          waiters.push(() => {
            clearTimeout(t);
            r();
          });
        });
      }
      return Uint8Array.from(rx.splice(0, n));
    },
    async flushInput() {
      rx = [];
    },
  };
}

const frame = (seq, status, body, more = false) =>
  `@ESL ${seq} ${status} ${enc.encode(body).length}${more ? "+" : ""} ${body}\n`;

const WIFI = { mode: "setup", ssid: null, connected: false, setup_ssid: "openesl-1A2B", networks: [{ ssid: "Butikken", rssi: -58 }] };

test("a request gets its reply out of the log", async () => {
  const t = fakeAp((r) => "I (123) wifi: scan: 1 networks\n" + frame(r.seq, 200, JSON.stringify(WIFI)) + "I (130) more log\n");
  const ap = new SerialAp(t, { timeoutMs: 500 });
  assert.deepEqual(await ap.wifi(), WIFI);
  assert.deepEqual(t.seen[0], { seq: "1", method: "GET", uri: "/api/wifi", body: null });
});

test("PUT /api/wifi sends ssid and password as JSON; a body may hold newlines and UTF-8", async () => {
  const t = fakeAp((r) => frame(r.seq, 200, JSON.stringify({ ...WIFI, mode: "sta", ssid: r.body.ssid }, null, 1)));
  const ap = new SerialAp(t, { timeoutMs: 500 });
  const w = await ap.setWifi("Kjøkken ✓", "hemmelig123");
  assert.equal(w.ssid, "Kjøkken ✓");
  assert.deepEqual(t.seen[0].body, { ssid: "Kjøkken ✓", password: "hemmelig123" });
});

test("PUT /api/wifi with a token, or only a token", async () => {
  const t = fakeAp((r) => frame(r.seq, 200, JSON.stringify({ ...WIFI, token_set: true })));
  const ap = new SerialAp(t, { timeoutMs: 500 });
  await ap.setWifi("Butikken", "hemmelig123", "lang-hemmelig");
  await ap.setWifi(undefined, "", "lang-hemmelig");
  await ap.setWifi("Butikken", "hemmelig123", "");
  await ap.setWifi(undefined, "", "", "butikk");
  assert.deepEqual(t.seen.map((r) => r.body), [
    { ssid: "Butikken", password: "hemmelig123", token: "lang-hemmelig" },
    { token: "lang-hemmelig" },
    { ssid: "Butikken", password: "hemmelig123" },
    { hostname: "butikk" },
  ]);
});

test("a reply in pieces is joined, with log lines between the pieces", async () => {
  const json = JSON.stringify(WIFI);
  const t = fakeAp((r) => frame(r.seq, 200, json.slice(0, 20), true) + "W (1) log line\n" + frame(r.seq, 200, json.slice(20)));
  const ap = new SerialAp(t, { timeoutMs: 500 });
  assert.deepEqual(await ap.wifi(), WIFI);
});

test("an error status throws with the AP's message", async () => {
  const t = fakeAp((r) => frame(r.seq, 422, '{"error":"password must be empty (open network) or 8 to 63 characters"}'));
  const ap = new SerialAp(t, { timeoutMs: 500 });
  await assert.rejects(ap.setWifi("Butikken", "kort"), /422: password must be empty/);
});

test("a stale reply to an earlier request is skipped", async () => {
  const t = fakeAp((r) => frame(Number(r.seq) - 1, 200, '{"old":true}') + frame(r.seq, 200, JSON.stringify(WIFI)));
  const ap = new SerialAp(t, { timeoutMs: 500 });
  ap.seq = 4;
  assert.deepEqual(await ap.wifi(), WIFI);
});

test("waitReady asks again while the AP restarts after the port opened", async () => {
  let n = 0;
  const t = fakeAp((r) => (++n < 3 ? "ESP-ROM:esp32c6-20220919\nboot log\n" : frame(r.seq, 200, JSON.stringify(WIFI))));
  const ap = new SerialAp(t);
  assert.deepEqual(await ap.waitReady({ tries: 5, timeoutMs: 100 }), WIFI);
  assert.equal(t.seen.length, 3);
  const dead = new SerialAp(fakeAp(() => null));
  await assert.rejects(dead.waitReady({ tries: 2, timeoutMs: 50 }), /does not answer/);
});

test("describeWifi and checkWifi", () => {
  assert.equal(describeWifi(WIFI), 'No network saved. Setup network "openesl-1A2B" is open.');
  assert.equal(describeWifi({ mode: "sta", ssid: "Butikken", connected: true, ip: "192.168.1.40", rssi: -58 }),
    'Connected to "Butikken", IP 192.168.1.40, -58 dBm.');
  assert.equal(checkWifi("Butikken", ""), null);
  assert.equal(checkWifi("Butikken", "hemmelig123"), null);
  assert.match(checkWifi("", "hemmelig123"), /network name/);
  assert.match(checkWifi("x".repeat(33), ""), /32 bytes/);
  assert.match(checkWifi("Butikken", "kort"), /8 to 63/);
  assert.match(checkWifi("Butikken", "hemmelig æøå"), /ASCII/);
  assert.equal(describeWifi({ ...WIFI, token_set: false }), 'No network saved. Setup network "openesl-1A2B" is open. No API token set yet.');
  assert.equal(describeWifi({ ...WIFI, token_set: true }), 'No network saved. Setup network "openesl-1A2B" is open.');
  assert.equal(checkToken("lang-hemmelig"), null);
  assert.match(checkToken("kort"), /8 to 128/);
  assert.match(checkToken("x".repeat(129)), /8 to 128/);
  assert.match(checkToken("med mellomrom"), /without spaces/);
  assert.match(checkToken("hemmelig-æøå"), /ASCII/);
  assert.equal(checkHostname("openesl"), null);
  assert.equal(checkHostname("Butikk-AP-2"), null);
  assert.equal(checkHostname("a"), null);
  for (const bad of ["", "-ap", "ap-", "my ap", "butikk.local", "æøå", "x".repeat(33)]) assert.ok(checkHostname(bad), bad);
  assert.equal(checkHostname("x".repeat(32)), null);
  const tok = newToken();
  assert.equal(tok.length, 24);
  assert.equal(checkToken(tok), null);
  assert.notEqual(newToken(), tok);
  // bytes of 228 and up are skipped, so every character is equally likely
  let calls = 0;
  const rnd = (n) => (++calls === 1 ? new Uint8Array(n).fill(250) : Uint8Array.from({ length: n }, (_, i) => i + 57));
  assert.equal(newToken(rnd), "ABCDEFGHJKLMNPQRSTUVWXYZ");
  assert.equal(calls, 2);
});
