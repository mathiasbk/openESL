// The "Send text to a tag" card (js/sendtext.js), against a fake AP client.
import { test } from "node:test";
import assert from "node:assert/strict";
import { dateLines, textFields, textBytes, midnight, contentRows, ensureTime, sendRows, MAX_DAYS } from "../js/sendtext.js";

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
