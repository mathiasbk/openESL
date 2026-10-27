// What the dashboard shows (js/dashdata.js), from replies shaped like docs/openapi.yaml.
import { test } from "node:test";
import assert from "node:assert/strict";
import { fmtAgo, fmtTagFw, fmtBattery, tagState, tagRows, rowCounts, filterRows, healthLine, apFacts, fmtInterval, parsePoll, batteryYears, pollNote, fmtClock, lastCheckin, nextCheckin, decodeTagImage, screenKey, screenErrorFinal, txOptions } from "../js/dashdata.js";

const tag = (mac, extra = {}) => ({
  mac, seen_s_ago: 12, battery_mv: 2950, rssi: -61, fw: 100, model: "2.9", draw_failed: false,
  content_id: 7, shown_content_id: 7, pending: false, poll_s: null, config: {}, config_pending: false,
  content_error: null, image: true, image_id: 9, shown_image_id: 9, ...extra,
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
  assert.equal(f["Poll interval"], "tag default (1 min)");
  assert.equal(f["AP time"], "not set");
  assert.equal(f.Scheduled, "1 of 64");
  assert.ok(!("AP time" in Object.fromEntries(apFacts({ fw: "0.1.0", uptime_s: 1, channel: 25, max_tags: 64, poll_s: 60 }))));
  // the network radio: its address and when it was last heard, or that it is not connected
  const up = Object.fromEntries(apFacts({ fw: "0.1.0", uptime_s: 1, channel: 25, max_tags: 64, poll_s: 60, radio: { connected: true, ip: "192.168.3.249", seen_s: 3, fw: 1 } }));
  assert.equal(up["Radio (C6)"], "192.168.3.249, last seen 3 s ago");
  const down = Object.fromEntries(apFacts({ fw: "0.1.0", uptime_s: 1, channel: 25, max_tags: 64, poll_s: 60, radio: { connected: false } }));
  assert.equal(down["Radio (C6)"], "not connected");
  assert.ok(!("Radio (C6)" in Object.fromEntries(apFacts({ fw: "0.1.0", uptime_s: 1, channel: 25, max_tags: 64, poll_s: 60 }))));
});

test("poll interval for all tags", () => {
  assert.equal(fmtInterval(30), "30 s");
  assert.equal(fmtInterval(90), "90 s");
  assert.equal(fmtInterval(120), "2 min");
  assert.equal(fmtInterval(3600), "1 h");
  assert.deepEqual(parsePoll(" 300 "), { poll_s: 300 });
  assert.deepEqual(parsePoll("3600"), { poll_s: 3600 });
  for (const bad of ["", " ", "0", "9", "3601", "12.5", "abc", "-60"]) assert.ok(parsePoll(bad).error, bad);
  assert.equal(batteryYears(0), 9.3); // the tag default, 60 s
  assert.equal(batteryYears(300), 11.7);
  assert.equal(batteryYears(3600), 12.1);
  assert.equal(batteryYears(10), null);
  assert.equal(pollNote(0), "A new price shows within 1 min. About 9.3 years on one CR2450 (a 1.54\" tag, one update a day; a 2.9\" tag lasts longer).");
  assert.equal(pollNote(300), "A new price shows within 5 min. Over 10 years on one CR2450, so the cells' shelf life sets the limit, not the polls.");
  assert.match(pollNote(10), /drain the battery/);
  assert.match(pollNote(60, [tag("A", { config: { poll_s: 30 } }), tag("B")]), /One tag has an interval of its own set/);
  assert.match(pollNote(60, [tag("A", { config: { poll_s: 30 } }), tag("B", { config: { poll_s: 600 } })]), /2 tags have an interval of their own set/);
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

test("tag image: columns of rows, black plane then red", () => {
  // 2 x 9 pixels: 2 bytes per column; black at (0,0) and (1,8), red at (1,1)
  const bytes = new Uint8Array([0x80, 0x00, 0x00, 0x80, /* red */ 0x00, 0x00, 0x40, 0x00]);
  const data = btoa(String.fromCharCode(...bytes));
  const img = decodeTagImage({ width: 2, height: 9, red: true, data });
  assert.equal(img.pixels[0], 1);
  assert.equal(img.pixels[1 * 2 + 1], 2);
  assert.equal(img.pixels[8 * 2 + 1], 1);
  assert.equal(img.pixels.filter((v) => v).length, 3);
  assert.equal(decodeTagImage({ width: 2, height: 9, red: true, data: btoa("\0\0\0\0") }), null);
  assert.equal(decodeTagImage({ width: 2, height: 9, red: false, data }).pixels[1 * 2 + 1], 0);
});

test("tag screen: fetched again when what it depends on changes, failures that pass are retried", () => {
  const t = tag("A", { image_id: 0 });
  assert.notEqual(screenKey(t), screenKey({ ...t, image_id: 5 }));            // drawn, or a new layout file
  assert.notEqual(screenKey({ ...t, model: null }), screenKey(t));             // model known after its first poll
  assert.notEqual(screenKey(t), screenKey({ ...t, content_id: 8 }));
  assert.equal(screenKey(t), screenKey({ ...t, pending: true }));
  const files = (v) => ({ layouts: [{ model: t.model, version: v }, { model: "other", version: 9 }] });
  assert.notEqual(screenKey(t, files(1)), screenKey(t, files(2)));            // a new layout file, no image id
  assert.equal(screenKey(t, files(1)), screenKey(t, { layouts: [{ model: t.model, version: 1 }] }));
  assert.equal(screenKey(t), screenKey(t, null));
  assert.equal(screenErrorFinal(new Error("404: model not known yet")), true);
  assert.equal(screenErrorFinal(new Error("409: the content cannot be drawn")), true);
  assert.equal(screenErrorFinal({ status: 404, message: "not found" }), true);
  assert.equal(screenErrorFinal(new Error("503: out of memory")), false);
  assert.equal(screenErrorFinal(new Error("429: too many requests")), false);
  assert.equal(screenErrorFinal(new Error("no answer from http://ap")), false);
  assert.equal(screenErrorFinal({ status: 0, message: "no answer from the AP on the USB port" }), false);
});

test("which AP: its board name, or the chip of an older AP", async () => {
  const { apBoard, apIdentity } = await import("../js/dashdata.js");
  const ap = { fw: "0.6.1", board: "s3-n16r8-c6", board_name: "ESP32-S3 + C6 on cable" };
  assert.equal(apBoard(ap, { chip: "esp32s3" }), "ESP32-S3 + C6 on cable");
  assert.equal(apBoard({ fw: "0.5.0" }, { chip: "esp32s3" }), "ESP32-S3");
  assert.equal(apBoard({ fw: "0.5.0" }, null), "");
  assert.equal(apIdentity(ap, null, { hostname: "openesl-3f2a" }), "ESP32-S3 + C6 on cable · openesl-3f2a.local · 0.6.1");
  assert.equal(apIdentity({ fw: "0.5.0" }, null, null), "0.5.0");
});

test("txOptions: presets highest first, the AP's own value and the built-in one marked", () => {
  assert.deepEqual(txOptions(20, 20).map((o) => o[1]), ["20 dBm (chip or PCB antenna, built-in)", "17 dBm (3 dBi antenna)", "15 dBm (5 dBi antenna)", "12 dBm (8 dBi antenna)", "10 dBm", "5 dBm", "0 dBm"]);
  assert.deepEqual(txOptions(-7, 10).map((o) => o[0]), [20, 17, 15, 12, 10, 5, 0, -7]);
  assert.equal(txOptions(-7, 10)[4][1], "10 dBm (built-in)");
  assert.equal(txOptions(undefined, undefined).length, 7);
});
