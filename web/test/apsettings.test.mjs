// The checks and helpers of the dashboard's Settings card (js/apsettings.js), against what the
// AP takes (esl_tz_ok, esl_ntp_name_ok, esl_webhook_url_ok in ap/components/esl_core).
import { test } from "node:test";
import assert from "node:assert/strict";
import { TZ_PRESETS, tzForZone, checkTz, checkNtp, checkWebhook, clockSkew, currentSettings, settingsChanges, configBody,
  spanText, spanBody, readSpan, windowWarning } from "../js/apsettings.js";

test("every preset time zone is one the AP takes", () => {
  for (const [tz] of TZ_PRESETS) assert.equal(checkTz(tz), null, tz);
  assert.equal(TZ_PRESETS[0][0], "", "UTC first, as the AP's default");
});

test("the browser's zone picks its preset", () => {
  assert.equal(tzForZone("Europe/Oslo")[0], "CET-1CEST,M3.5.0,M10.5.0/3");
  assert.equal(tzForZone("Europe/Helsinki")[0], "EET-2EEST,M3.5.0/3,M10.5.0/4");
  assert.equal(tzForZone("Asia/Tokyo"), null);
});

test("time zones, NTP servers and webhooks are checked as the AP does", () => {
  assert.equal(checkTz("<+0530>-5:30"), null);
  assert.ok(checkTz("1CET"));
  assert.ok(checkTz("CET -1"));
  assert.ok(checkTz("Europe/Oslo"), "a tzdata name: the AP would take it as UTC");
  assert.ok(checkTz("CET"));
  assert.equal(checkTz("EST5EDT,M3.2.0,M11.1.0"), null);
  assert.equal(checkNtp("no.pool.ntp.org"), null);
  assert.equal(checkNtp("192.168.1.1"), null);
  assert.ok(checkNtp("-pool"));
  assert.ok(checkNtp("pool_ntp"));
  assert.ok(checkNtp("a".repeat(64)));
  assert.equal(checkWebhook("http://192.168.1.10:8123/api/webhook/openesl"), null);
  assert.ok(checkWebhook("https://ha.local/hook"));
  assert.ok(checkWebhook("http:///path"));
  assert.ok(checkWebhook("http://ha.local/a b"));
  assert.ok(checkWebhook("http://" + "a".repeat(121)));
});

test("the clock's difference from this computer", () => {
  assert.equal(clockSkew(1000, 1002), "");
  assert.equal(clockSkew(1000, 1125), "2 min 5 s behind this computer");
  assert.equal(clockSkew(1000 + 7260, 1000), "2 h 1 min ahead of this computer");
});

test("what the AP has, as the Settings page shows it", () => {
  const last = {
    list: { ap: { poll_s: 0 } },
    wifi: { hostname: "openesl-3f2a" },
    config: { poll_s: 0, webhook: null, tz: "CET-1CEST,M3.5.0,M10.5.0/3", ntp: "pool.ntp.org", builtin_ntp: "pool.ntp.org" },
  };
  assert.deepEqual(currentSettings(last), { hostname: "openesl-3f2a", tz: "CET-1CEST,M3.5.0,M10.5.0/3", poll_s: 0, webhook: "", ntp: "" });
  // older firmware (no clock settings) and an AP without WiFi: those keys are left out
  assert.deepEqual(currentSettings({ list: { ap: { poll_s: 300 } }, wifi: null, config: { poll_s: 300, webhook: "http://ha:8123/x" } }),
    { poll_s: 300, webhook: "http://ha:8123/x" });
  assert.equal(currentSettings({ ...last, config: { ...last.config, ntp: "ntp.uio.no" } }).ntp, "ntp.uio.no");
  // the date language from AP firmware 0.11.0; an older AP has none, so it is not offered
  assert.equal(currentSettings({ ...last, config: { ...last.config, lang: "nb" } }).lang, "nb");
  assert.equal(currentSettings({ ...last, config: { ...last.config, lang: null } }).lang, "", "not chosen");
  assert.ok(!("lang" in currentSettings(last)));
});

test("only what differs is saved, with built-in values as a reset", () => {
  const cur = { hostname: "openesl", tz: "", poll_s: 0, webhook: "", ntp: "" };
  assert.deepEqual(settingsChanges({ hostname: "openesl", tz: "", poll_s: 0 }, cur), {});
  assert.deepEqual(settingsChanges({ hostname: "butikk", tz: "CET", poll_s: 300 }, cur), { hostname: "butikk", tz: "CET", poll_s: 300 });
  // a key the AP does not have is not sent
  assert.deepEqual(settingsChanges({ tz: "CET", poll_s: 0 }, { poll_s: 0 }), {});
  assert.deepEqual(configBody({ tz: "", ntp: "", webhook: "" }), { tz: null, ntp: null, webhook: null });
  assert.deepEqual(configBody({ poll_s: 60, hostname: "x" }, { builtin_poll_s: 60 }), { poll_s: null });
  assert.deepEqual(configBody({ lang: "nb" }), { lang: "nb" });
  assert.deepEqual(configBody({ lang: "" }), { lang: null });
  assert.deepEqual(settingsChanges({ lang: "nb", tz: "" }, { lang: "en" }), { lang: "nb" });
  assert.deepEqual(configBody({ poll_s: 300, webhook: "http://ha/x" }, { builtin_poll_s: 60 }), { poll_s: 300, webhook: "http://ha/x" });
});

test("the transmit power is a setting only on an AP with a radio", () => {
  const last = { list: { ap: { poll_s: 0 } }, config: { poll_s: 0, webhook: null, tx_power: 8, builtin_tx_power: 10 } };
  assert.equal(currentSettings(last).tx_power, 8);
  assert.ok(!("tx_power" in currentSettings({ ...last, config: { poll_s: 0, webhook: null } })));
  assert.deepEqual(configBody({ tx_power: 10 }, last.config), { tx_power: null });
  assert.deepEqual(configBody({ tx_power: 13 }, last.config), { tx_power: 13 });
});

test("the channel: the one the AP moves to while a move waits, built-in as a reset", () => {
  const last = { list: { ap: { poll_s: 0 } }, config: { poll_s: 0, webhook: null, channel: 25, builtin_channel: 25, channel_next: null } };
  assert.equal(currentSettings(last).channel, 25);
  assert.equal(currentSettings({ ...last, config: { ...last.config, channel_next: 20, channel_waiting: 2, channel_due_s: 300 } }).channel, 20);
  assert.ok(!("channel" in currentSettings({ ...last, config: { poll_s: 0, webhook: null } })));
  assert.deepEqual(configBody({ channel: 25 }, last.config), { channel: null });
  assert.deepEqual(configBody({ channel: 20 }, last.config), { channel: 20 });
});

test("night mode and update window go to the AP as it takes them (#415)", () => {
  const config = { poll_s: 300, builtin_poll_s: 0, night: { from: "22:00", to: "06:00", poll_s: 1800 }, update_window: null };
  const cur = currentSettings({ config, list: { ap: { poll_s: 300 } } });
  assert.equal(cur.night, "22:00-06:00/1800");
  assert.equal(cur.update_window, "");
  assert.ok(!("night" in currentSettings({ config: { poll_s: 0 }, list: { ap: {} } })), "an AP before 0.11.0 has neither");
  const ch = settingsChanges({ night: "", update_window: "06:00-08:00" }, cur);
  assert.deepEqual(configBody(ch, config), { night: null, update_window: { from: "06:00", to: "08:00" } });
  assert.deepEqual(spanBody("23:30-05:00/600"), { from: "23:30", to: "05:00", poll_s: 600 });
  assert.equal(spanText(null), "");
  assert.equal(spanText({ from: "06:00", to: "08:00" }), "06:00-08:00");
  assert.equal(readSpan(false, "", "", "Night mode"), "");
  assert.equal(readSpan(true, "22:00", "06:00", "Night mode", 900), "22:00-06:00/900");
  assert.ok(readSpan(true, "08:00", "08:00", "Update window").error);
  assert.ok(readSpan(true, "", "08:00", "Update window").error);
});

test("windowWarning: a window shorter than the poll interval", () => {
  assert.equal(windowWarning("", 600), "");
  assert.equal(windowWarning("06:00-08:00", 600), "");
  assert.match(windowWarning("06:00-06:05", 600), /shorter than the poll interval \(10 min\)/);
  assert.equal(windowWarning("23:59-00:01", 0), ""); // over midnight, the tags' own 60 s
});
