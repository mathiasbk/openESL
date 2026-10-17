// The "Send info to tags" card (js/sendtext.js), against a fake AP client.
import { test } from "node:test";
import assert from "node:assert/strict";
import { dateLines, textFields, textBytes, midnight, contentRows, ensureTime, sendRows, MAX_DAYS,
  infoValues, fillLine, infoRows, usesDate, maxDays, PRESETS, PLACEHOLDERS } from "../js/sendtext.js";

const fakeAp = (time = null) => {
  const calls = [];
  return {
    calls,
    async call(method, path, body) {
      calls.push([method, path, body]);
      if (path === "/api/time") return method === "GET" ? { time, source: time ? "ntp" : null } : { time: body.time, source: "api" };
      if (path === "/api/tags") return { tags: body.tags.map((r, i) => ({ mac: r.mac, content_id: i + 1, scheduled: !!r.at, at: r.at })) };
      return { mac: path.split("/").pop(), content_id: 1, changed: true, pending: true };
    },
  };
};

test("date lines in Norwegian", () => {
  assert.deepEqual(dateLines(new Date(2026, 9, 17)), ["Lørdag", "17. oktober 2026"]);
  assert.deepEqual(dateLines(new Date(2027, 0, 3)), ["Søndag", "3. januar 2027"]);
});

test("fields and bytes", () => {
  assert.deepEqual(textFields(["a", "b", "", " "]), ["a", "b"]);
  assert.deepEqual(textFields(["", "", "", ""]), [""]);
  assert.deepEqual(textFields(["", "x", "", ""]), ["", "x"]);
  assert.equal(textBytes(["Lørdag", "17. oktober 2026"]), 6 + 1 + 16 + 1);
});

test("one tag, plain text", () => {
  const rows = contentRows("00000123456789A8", ["Hei", "der", "", ""], { frame: true });
  assert.deepEqual(rows, [{ mac: "00000123456789A8", layout: "text", fields: ["Hei", "der"], frame: true }]);
});

test("today's date, then the next days at midnight", () => {
  const today = new Date(2026, 9, 17, 6, 14);
  const rows = contentRows("M", ["x", "y", "Velkommen", ""], { date: true, days: 3, today });
  assert.deepEqual(rows.map((r) => r.fields), [
    ["Lørdag", "17. oktober 2026", "Velkommen"],
    ["Søndag", "18. oktober 2026", "Velkommen"],
    ["Mandag", "19. oktober 2026", "Velkommen"],
  ]);
  assert.equal(rows[0].at, undefined);
  assert.deepEqual(rows.slice(1).map((r) => r.at), [midnight(new Date(2026, 9, 18)), midnight(new Date(2026, 9, 19))]);
  assert.equal(new Date(rows[1].at * 1000).getHours(), 0);
  // days only count with the date, and are capped
  assert.equal(contentRows("M", ["a"], { days: 5 }).length, 1);
  assert.equal(contentRows("M", ["a"], { date: true, days: 500 }).length, MAX_DAYS);
});

test("one row goes to PUT /api/tags/{mac}", async () => {
  const ap = fakeAp();
  const { replies } = await sendRows(ap, contentRows("M", ["Hei"]));
  assert.deepEqual(ap.calls, [["PUT", "/api/tags/M", { layout: "text", fields: ["Hei"] }]]);
  assert.equal(replies[0].pending, true);
});

test("several days set the AP's clock first when it has none", async () => {
  const ap = fakeAp(null);
  const { replies, note } = await sendRows(ap, contentRows("M", [], { date: true, days: 2 }));
  assert.deepEqual(ap.calls.map((c) => c.slice(0, 2)), [["GET", "/api/time"], ["PUT", "/api/time"], ["PUT", "/api/tags"]]);
  assert.match(note, /set from this computer/);
  assert.deepEqual(replies.map((r) => r.scheduled), [false, true]);
});

test("a clock that is off gives a warning", async () => {
  assert.equal(await ensureTime(fakeAp(1_000_000_000), 1_000_000_000_000), "");
  assert.match(await ensureTime(fakeAp(1_000_000_500), 1_000_000_000_000), /500 s off/);
});

// ---- Several tags, with placeholders ----

const AP = { fw: "0.4.4", channel: 11 };
const WIFI = { connected: true, ip: "192.168.1.40", ssid: "Butikken", hostname: "openesl" };
const T1 = { mac: "00000123456789A8", model: "2.9", battery_mv: 2950, rssi: -61, fw: 400, seen_s_ago: 75 };
const T2 = { mac: "00000123456789B0", model: "1.54", battery_mv: 0, rssi: null, fw: null, seen_s_ago: null };

test("placeholders are filled in for each tag", () => {
  const v = infoValues(T1, { ap: AP, wifi: WIFI, tags: 2, day: new Date(2026, 9, 17), now: new Date(2026, 9, 17, 9, 5) });
  assert.equal(fillLine("IP {ap_ip} ({ap_name}) {wifi}", v), "IP 192.168.1.40 (openesl) Butikken");
  assert.equal(fillLine("{mac} {model} {fw} {battery} {rssi} {seen}", v), '00000123456789A8 2.9" 0.4.0 2.95 V -61 dBm 1 min');
  assert.equal(fillLine("{weekday} {date} {time} kanal {channel} AP {ap_fw} av {tags}", v), "Lørdag 17. oktober 2026 09:05 kanal 11 AP 0.4.4 av 2");
  assert.equal(fillLine("{nope} {mac", v), "{nope} {mac", "unknown names and stray braces stay");
  // what is not known becomes "-" (Latin-1, so the tag draws it)
  const u = infoValues(T2, { wifi: { connected: false, ip: "1.2.3.4" } });
  assert.equal(fillLine("{ap_ip} {battery} {rssi} {fw} {seen} {wifi} {ap_fw}", u), "- - - - - - -");
  for (const [name] of PLACEHOLDERS) assert.ok(name in v, name);
});

test("one row per tag, the date only adds days", () => {
  const today = new Date(2026, 9, 17, 9, 5);
  const rows = infoRows([T1, T2], PRESETS.tag, { ap: AP, wifi: WIFI, days: 5, today });
  assert.deepEqual(rows.map((r) => [r.mac, r.fields[0], r.at]), [[T1.mac, T1.mac, undefined], [T2.mac, T2.mac, undefined]]);
  const dated = infoRows([T1, T2], ["{weekday}", "{date}", "IP {ap_ip}"], { wifi: WIFI, days: 2, frame: true, today });
  assert.deepEqual(dated.map((r) => [r.mac, r.fields[0], !!r.at, r.frame]), [
    [T1.mac, "Lørdag", false, true], [T1.mac, "Søndag", true, true],
    [T2.mac, "Lørdag", false, true], [T2.mac, "Søndag", true, true],
  ]);
  assert.equal(dated[1].at, midnight(new Date(2026, 9, 18)));
  assert.deepEqual(dated[0].fields, ["Lørdag", "17. oktober 2026", "IP 192.168.1.40"]);
});

test("days are capped so one PUT /api/tags holds them", () => {
  assert.equal(usesDate(["x", "{date}"]), true);
  assert.equal(usesDate(PRESETS.ap), false);
  assert.equal(maxDays(1), MAX_DAYS);
  assert.equal(maxDays(5), 10);
  assert.equal(maxDays(80), 1);
  const tags = Array.from({ length: 5 }, (_, i) => ({ mac: "M" + i }));
  assert.equal(infoRows(tags, PRESETS.date, { days: 31 }).length, 50);
});

test("several tags today go in one PUT /api/tags without asking for the time", async () => {
  const ap = fakeAp(null);
  const { replies, note } = await sendRows(ap, infoRows([T1, T2], PRESETS.ap, { ap: AP, wifi: WIFI }));
  assert.deepEqual(ap.calls.map((c) => c.slice(0, 2)), [["PUT", "/api/tags"]]);
  assert.equal(note, "");
  assert.equal(replies.length, 2);
  assert.deepEqual(ap.calls[0][2].tags[1].fields, ["OpenESL AP", "IP 192.168.1.40", "WiFi Butikken", "FW 0.4.4"]);
});
