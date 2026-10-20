// What the dashboard shows (js/dashdata.js), from replies shaped like docs/openapi.yaml.
import { test } from "node:test";
import assert from "node:assert/strict";
import { fmtAgo, fmtTagFw, fmtBattery, tagState, tagRows, rowCounts, filterRows, healthLine, apFacts, fmtClock, lastCheckin, nextCheckin } from "../js/dashdata.js";

const tag = (mac, extra = {}) => ({
  mac, seen_s_ago: 12, battery_mv: 2950, rssi: -61, fw: 100, model: "2.9", draw_failed: false,
  content_id: 7, shown_content_id: 7, pending: false, poll_s: null, config: {}, config_pending: false,
  layout_version: 1, layout_pending: false, layout_failed: false, content_error: null, ...extra,
});

test("formats", () => {
  assert.equal(fmtAgo(null), "never");
  assert.equal(fmtAgo(42), "42 s");
  assert.equal(fmtAgo(125), "2 min");
  assert.equal(fmtAgo(3 * 3600 + 5 * 60), "3 h 5 min");
  assert.equal(fmtAgo(2 * 86400 + 3600), "2 d 1 h");
  assert.equal(fmtTagFw(100), "0.1.0");
  assert.equal(fmtTagFw(10203), "1.2.3");
  assert.equal(fmtTagFw(null), "–");
  assert.equal(fmtBattery(2950), "2.95 V");
  assert.equal(fmtBattery(0), "–");
});

test("tag state: problems first, then what is still on its way", () => {
  assert.deepEqual(tagState(tag("A")), { level: "ok", text: "OK" });
  assert.equal(tagState(tag("A", { pending: true })).text, "new content waiting");
  assert.equal(tagState(tag("A", { pending: true, content_id: 0 })).text, "no content yet");
  assert.equal(tagState(tag("A", { layout_pending: true, pending: true })).text, "downloading the layout file");
  assert.deepEqual(tagState(tag("A", { pending: true }), ["silent", "low_battery"]), { level: "bad", text: "silent, low battery" });
  // overdue but not silent yet: not "OK" next to a late check-in
  assert.deepEqual(tagState(tag("A"), [], { late: true }), { level: "busy", text: "late" });
  assert.equal(tagState(tag("A", { fw_update: { version: "0.4.6", received: 1024, size: 54272 } })).text, "updating firmware");
});

test("rows: tags that need attention first, then by MAC", () => {
  const list = { ap: {}, tags: [tag("0003"), tag("0001", { pending: true }), tag("0002")] };
  const health = { problems: [{ mac: "0002", problems: ["silent"], seen_s_ago: 900, battery_mv: 2900 }] };
  assert.deepEqual(tagRows(list, health).map((r) => r.tag.mac), ["0002", "0001", "0003"]);
  assert.deepEqual(tagRows(list, null).map((r) => r.state.level), ["busy", "ok", "ok"]);
  assert.equal(tagRows({ tags: [tag("0004", { seen_s_ago: 300 })] }, null)[0].state.text, "late");
});

test("filters and their counts", () => {
  const list = { tags: [tag("00AA"), tag("00BB", { pending: true, model: "1.54" }), tag("00CC"), tag("00DD", { model: null, seen_s_ago: null })] };
  const rows = tagRows(list, { problems: [{ mac: "00CC", problems: ["low_battery"] }] });
  assert.deepEqual(rowCounts(rows), { all: 4, bad: 1, busy: 1, models: { "2.9": 2, "1.54": 1 } });
  const macs = (f, find) => filterRows(rows, f, find).map((r) => r.tag.mac);
  assert.deepEqual(macs("all"), ["00CC", "00BB", "00AA", "00DD"]);
  assert.deepEqual(macs("bad"), ["00CC"]);
  assert.deepEqual(macs("busy"), ["00BB"]);
  assert.deepEqual(macs("model:2.9"), ["00CC", "00AA"]);
  assert.deepEqual(macs("all", "bb"), ["00BB"]);
});

test("health line", () => {
  assert.equal(healthLine(null), "");
  assert.equal(healthLine({ ok: true, tags: 12 }), "All 12 tags are fine.");
  assert.equal(healthLine({ ok: false, tags: 12, with_problems: 2, silent: 1, unseen: 0, low_battery: 1 }),
    "2 of 12 tags need attention: 1 silent, 1 low battery.");
  assert.equal(healthLine({ ok: false, tags: 12, with_problems: 1, silent: 1 }), "1 of 12 tags needs attention: 1 silent.");
  assert.equal(healthLine({ ok: true, tags: 1 }), "The tag is fine.");
  assert.equal(healthLine({ ok: false, tags: 1, with_problems: 1, low_battery: 1 }), "The tag needs attention: 1 low battery.");
  assert.equal(healthLine({ ok: true, tags: 0 }), ""); // the table says there are no tags yet
});

test("AP facts", () => {
  const f = Object.fromEntries(apFacts({ fw: "0.1.0", uptime_s: 90000, channel: 25, max_tags: 256, poll_s: 0, reset: "task_wdt", time: null, scheduled: 1, max_scheduled: 64 }));
  assert.equal(f.Firmware, "0.1.0");
  assert.equal(f["Running for"], "1 d 1 h");
  assert.equal(f["Last start"], "watchdog (stuck task)");
  assert.equal(f["Poll interval"], "tags' own");
  assert.equal(f["AP time"], "not set");
  assert.equal(f.Scheduled, "1 of 64");
  assert.ok(!("AP time" in Object.fromEntries(apFacts({ fw: "0.1.0", uptime_s: 1, channel: 25, max_tags: 64, poll_s: 60 }))));
});

test("last and next check-in", () => {
  const now = new Date(2026, 9, 17, 12, 0, 0).getTime();
  assert.deepEqual(lastCheckin(tag("A", { seen_s_ago: null }), now), { text: "never", at: "" });
  assert.deepEqual(lastCheckin(tag("A", { seen_s_ago: 125 }), now), { text: "2 min ago", at: fmtClock(-125, now) });
  assert.equal(nextCheckin(tag("A", { seen_s_ago: null }), now), null);
  // poll_s null: the tag's own 60 s
  const n = nextCheckin(tag("A", { seen_s_ago: 15 }), now);
  assert.deepEqual([n.left_s, n.late], [45, false]);
  assert.deepEqual([n.text, n.at], ["in 45 s", fmtClock(45, now)]);
  assert.equal(nextCheckin(tag("A", { seen_s_ago: 100, poll_s: 300 }), now).left_s, 200);
  assert.equal(nextCheckin(tag("A", { seen_s_ago: 70 }), now).text, "any moment now");
  const late = nextCheckin(tag("A", { seen_s_ago: 245 }), now);
  assert.deepEqual([late.late, late.text], [true, "3 min late"]);
  // another day: the date too
  assert.ok(fmtClock(-86400, now).length > fmtClock(0, now).length);
});
