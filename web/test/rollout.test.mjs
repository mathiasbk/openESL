// One tag first for new tag firmware (js/rollout.js, #306)
import { test } from "node:test";
import assert from "node:assert/strict";
import { verNum, trackRollout, pollsOn, modelTrial, firstTag, apCanChoose, rolloutQuestion, ROLLOUT_POLLS } from "../js/rollout.js";

const NEW = 1100, OLD = 1000; // 0.11.0, 0.10.0
const tag = (mac, fw, polls, more = {}) => ({ mac, model: "2.9", fw, ota: true, rssi: -60, seen_s_ago: 10, signal: polls == null ? null : { polls }, ...more });
const read = (tags, uptime_s) => ({ ap: { uptime_s }, tags }); // no uptime: restarts told by the poll count alone

test("versions as the tag's fw", () => {
  assert.equal(verNum("0.11.0"), 1100);
  assert.equal(verNum("1.2.3"), 10203);
  assert.equal(verNum(null), null);
  assert.equal(verNum("0.11"), null);
});

test("counts the polls on a version from the first time it is seen on it", () => {
  let s = trackRollout({}, read([tag("A", OLD, 50)]), null, 10000);
  assert.equal(pollsOn(s.A, tag("A", OLD, 50)), 0);
  s = trackRollout(s, read([tag("A", NEW, 53)]), null, 10010);
  assert.equal(s.A.fw, NEW);
  s = trackRollout(s, read([tag("A", NEW, 60)]), null, 10100);
  assert.equal(pollsOn(s.A, tag("A", NEW, 60)), 7);
  // a different version than the one counted: nothing
  assert.equal(pollsOn(s.A, tag("A", OLD, 60)), null);
});

test("keeps the count over an AP restart", () => {
  let s = trackRollout({}, read([tag("A", NEW, 20)], 1000), null, 10000);
  s = trackRollout(s, read([tag("A", NEW, 26)], 1360), null, 10360);
  // restarted: the uptime and the count begin again
  s = trackRollout(s, read([tag("A", NEW, 3)], 200), null, 10600);
  assert.equal(pollsOn(s.A, tag("A", NEW, 3)), 9);
  // restarted, and the tag polled more times since than before: told by the uptime
  let t = trackRollout({}, read([tag("B", NEW, 2)], 100), null, 10000);
  t = trackRollout(t, read([tag("B", NEW, 30)], 1900), null, 20000);
  assert.equal(pollsOn(t.B, tag("B", NEW, 30)), 30);
  // the same boot, read a little apart: no restart
  let u = trackRollout({}, read([tag("C", NEW, 2)], 100), null, 10000);
  u = trackRollout(u, read([tag("C", NEW, 12)], 705), null, 10600);
  assert.equal(pollsOn(u.C, tag("C", NEW, 12)), 10);
});

test("a tag not heard since the AP started keeps what was counted", () => {
  let s = trackRollout({}, read([tag("A", NEW, 5)]), null, 10000);
  s = trackRollout(s, read([tag("A", NEW, 15)]), null, 10600);
  s = trackRollout(s, read([tag("A", NEW, null)], 30), null, 11000);
  assert.equal(s.A.last, 15);
  assert.equal(pollsOn(s.A, tag("A", NEW, null)), 10);
});

test("problems on the version stay with it, low battery does not count", () => {
  let s = trackRollout({}, read([tag("A", NEW, 5), tag("B", NEW, 5)]), { problems: [{ mac: "A", problems: ["draw_failed"] }, { mac: "B", problems: ["low_battery"] }] }, 10000);
  s = trackRollout(s, read([tag("A", NEW, 30), tag("B", NEW, 30)]), { problems: [] }, 11000);
  assert.deepEqual(s.A.bad, ["draw_failed"]);
  assert.deepEqual(s.B.bad, []);
  // new firmware: a clean slate
  s = trackRollout(s, read([tag("A", 1101, 31)]), null, 11100);
  assert.deepEqual(s.A.bad, []);
});

test("how far a version is tried", () => {
  const list = read([tag("A", NEW, 40), tag("B", OLD, 40), tag("C", OLD, 40)]);
  let s = trackRollout({}, read([tag("A", NEW, 35), tag("B", OLD, 30), tag("C", OLD, 30)]), null, 10000);
  s = trackRollout(s, list, null, 10300);
  let t = modelTrial("2.9", "0.11.0", list, s);
  assert.equal(t.tried, 1);
  assert.equal(t.polls, 5);
  assert.equal(t.proven, false);
  assert.match(t.text, /runs on 1 tag; 5 of 10 polls/);

  const later = read([tag("A", NEW, 45), tag("B", OLD, 45), tag("C", OLD, 45)]);
  s = trackRollout(s, later, null, 10600);
  t = modelTrial("2.9", NEW, later, s);
  assert.equal(t.proven, true);
  assert.match(t.text, /safe for the rest/);
  // a tag rejected the AP's image, or one had a problem: not proven
  assert.equal(modelTrial("2.9", NEW, later, s, 1).proven, false);
  const bad = trackRollout(s, later, { problems: [{ mac: "A", problems: ["silent"] }] }, 10700);
  t = modelTrial("2.9", NEW, later, bad);
  assert.equal(t.proven, false);
  assert.match(t.text, /A: silent/);
  // nobody on it
  assert.equal(modelTrial("1.54", NEW, later, s).text, "not tried on a tag yet");
});

test("the tag to try it on first", () => {
  const rows = [
    { tag: tag("A", OLD, 1, { rssi: -50 }), problems: ["draw_failed"], next: null },
    { tag: tag("B", OLD, 1, { rssi: -80 }), problems: [], next: null },
    { tag: tag("C", OLD, 1, { rssi: -55 }), problems: [], next: { late: true } },
    { tag: tag("D", OLD, 1, { rssi: -70 }), problems: [], next: null },
    { tag: tag("E", NEW, 1, { rssi: -40 }), problems: [], next: null },
    { tag: tag("F", OLD, 1, { rssi: -40, ota: false }), problems: [], next: null },
    { tag: tag("G", OLD, 1, { rssi: -40, model: "1.54" }), problems: [], next: null },
  ];
  assert.equal(firstTag(rows, "2.9", NEW).mac, "D");
  assert.equal(firstTag(rows, "2.9", NEW, ["A", "B", "C"]).mac, "B");
  assert.equal(firstTag(rows, "2.9", NEW, ["E", "F"]), null);
});

test("an AP that can give an image to chosen tags", () => {
  assert.equal(apCanChoose(null), true);
  assert.equal(apCanChoose({ images: [{ model: "2.9", version: null }] }), true);
  assert.equal(apCanChoose({ images: [{ model: "2.9", version: "0.11.0", to: "all" }] }), true);
  assert.equal(apCanChoose({ images: [{ model: "2.9", version: "0.11.0" }] }), false);
});

test("asks only for several tags of a version no tag has run long enough", () => {
  const notTried = { tried: 0, failed: 0, bad: [], proven: false, text: "not tried on a tag yet" };
  assert.equal(rolloutQuestion("2.9", NEW, 1, notTried), null);
  assert.equal(rolloutQuestion("2.9", NEW, 5, { ...notTried, proven: true }), null);
  const q = rolloutQuestion("2.9", NEW, 5, notTried);
  assert.match(q, /5 2\.9" tags would get firmware 0\.11\.0\. No 2\.9" tag runs 0\.11\.0 yet\./);
  assert.match(q, new RegExp(`polled ${ROLLOUT_POLLS} times`));
  assert.match(rolloutQuestion("2.9", "0.11.0", 5, { tried: 1, failed: 0, bad: [], proven: false, text: "runs on 1 tag" }), /No 2\.9" tag has polled 10 times on 0\.11\.0 yet/);
  assert.match(rolloutQuestion("2.9", NEW, 5, { tried: 1, failed: 1, bad: [], proven: false, text: "x" }), /have had problems/);
  // one more tag, but tags have had problems with it: asks too
  assert.match(rolloutQuestion("2.9", NEW, 1, { tried: 1, failed: 1, bad: [], proven: false, text: "x" }), /^1 2\.9" tag would get .*have had problems/);
  assert.equal(rolloutQuestion("2.9", NEW, 0, { tried: 1, failed: 1, bad: [], proven: false, text: "x" }), null);
});
