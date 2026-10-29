// dashdata.js - what the dashboard shows, worked out from GET /api/tags and GET /api/health
// (docs/openapi.yaml). No DOM here, so test/dashdata.test.mjs can check it.

// 75 -> "1 min", null -> "never"
export function fmtAgo(s) {
  if (s == null) return "never";
  if (s < 60) return `${s} s`;
  if (s < 3600) return `${Math.floor(s / 60)} min`;
  if (s < 86400) return `${Math.floor(s / 3600)} h ${Math.floor((s % 3600) / 60)} min`;
  return `${Math.floor(s / 86400)} d ${Math.floor((s % 86400) / 3600)} h`;
}

// What a tag polls with when the AP sends it no interval (poll_s null): tag/src/config.h
// CFG_POLL_S, ESL_TAG_BUILTIN_POLL_S on the AP
export const TAG_BUILTIN_POLL_S = 60;

// The clock time offset_s seconds from now (negative = in the past), with the date when it
// is not today: "13:41:05", "16.10. 09:12:00"
export function fmtClock(offset_s, now = Date.now()) {
  const d = new Date(now + offset_s * 1000);
  const time = d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  if (d.toDateString() === new Date(now).toDateString()) return time;
  return d.toLocaleDateString([], { day: "2-digit", month: "2-digit" }) + " " + time;
}

// The tag's last poll: { text: "2 min ago", at: "13:41:05" }, or "never" with no time
export function lastCheckin(tag, now = Date.now()) {
  if (tag.seen_s_ago == null) return { text: "never", at: "" };
  return { text: `${fmtAgo(tag.seen_s_ago)} ago`, at: fmtClock(-tag.seen_s_ago, now) };
}

// When the tag should poll next: its last poll plus the interval the AP gives it (poll_s,
// or the tag's own when null). The AP may tell it to come sooner when scheduled content is
// due, so this is the latest it is expected. { left_s, late, text, at }, null when never seen
export const CHECKIN_GRACE_S = 15; // radio retries and clock drift: not late yet
export function nextCheckin(tag, now = Date.now()) {
  if (tag.seen_s_ago == null) return null;
  const left = (tag.poll_s || TAG_BUILTIN_POLL_S) - tag.seen_s_ago;
  const at = fmtClock(left, now);
  if (left >= 0) return { left_s: left, late: false, text: `in ${fmtAgo(left)}`, at };
  if (-left <= CHECKIN_GRACE_S) return { left_s: left, late: false, text: "any moment now", at };
  return { left_s: left, late: true, text: `${fmtAgo(-left)} late`, at };
}

// The tag's fw_ver (major*10000 + minor*100 + patch): 100 -> "0.1.0"
export function fmtTagFw(v) {
  if (v == null) return "–";
  return `${Math.floor(v / 10000)}.${Math.floor(v / 100) % 100}.${v % 100}`;
}

export function fmtBattery(mv) {
  if (mv == null) return "–";
  if (mv === 0) return "–"; // the 1.54" cannot measure it
  return (mv / 1000).toFixed(2) + " V";
}

// The signal history in GET /api/tags (signal: the last POLLs, #261): { rssi, min, avg,
// lost, retries, n, weak, text, title }, null for a tag not heard since the AP started.
// Weak: a POLL lost or answered only at its retry in the history, or an average below
// WEAK_RSSI. The radios still hear each other down to about -95 dBm, so a tag that is
// weak now is the one to move or give a nearer AP before it drops out.
export const WEAK_RSSI = -85;
export function signalSummary(tag) {
  const s = tag.signal;
  if (!s || !s.rssi?.length) return null;
  const n = s.rssi.length;
  const min = Math.min(...s.rssi);
  const avg = Math.round(s.rssi.reduce((a, b) => a + b, 0) / n);
  const lost = (s.lost || []).reduce((a, b) => a + b, 0);
  const retries = (s.retry || []).reduce((a, b) => a + b, 0);
  // one retry now and then is normal; a lost round, a weak average or retries on a quarter
  // of the polls are not
  const weak = lost > 0 || avg < WEAK_RSSI || retries * 4 >= n;
  const rssi = s.rssi[n - 1];
  const polls = n === 1 ? "The last poll" : `The last ${n} polls`;
  const parts = [n === 1 ? `${polls}: ${rssi} dBm.` : `${polls}: ${min} to ${Math.max(...s.rssi)} dBm, ${avg} on average.`];
  if (lost) parts.push(lost === 1 ? "1 poll got no answer." : `${lost} polls got no answer.`);
  if (retries) parts.push(retries === 1 ? "1 answer needed the tag's second try." : `${retries} answers needed the tag's second try.`);
  if (s.tag_lqi != null) parts.push(`The tag hears the AP at link quality ${s.tag_lqi} of 127.`);
  if (s.polls) parts.push(`Since the AP started: ${s.polls} polls, ${s.lost_total} lost, ${s.retries} retried.`);
  if (weak) parts.push("Weak: move the tag or the AP closer, or the tag may drop out.");
  return { rssi, min, avg, lost, retries, n, weak, text: `${rssi} dBm` + (weak ? " · weak" : ""), title: parts.join(" ") };
}

export const PROBLEM_TEXT = {
  silent: "silent",
  unseen: "not seen since the AP started",
  draw_failed: "drawing failed",
  low_battery: "low battery",
  content_error: "content cannot be shown",
};

export const RESET_TEXT = {
  poweron: "power on",
  ext: "reset pin",
  sw: "restart",
  usb: "USB reset",
  panic: "crash",
  int_wdt: "watchdog (interrupt)",
  task_wdt: "watchdog (stuck task)",
  wdt: "watchdog",
  deepsleep: "deep sleep",
  brownout: "brownout (low voltage)",
  jtag: "JTAG",
  pwr_glitch: "power glitch",
  cpu_lockup: "CPU lockup",
  unknown: "unknown",
};

// What a tag is doing, in a few words: the problems the AP reports first, then a poll that
// is overdue (next: nextCheckin), then whether it is still getting something (firmware,
// content, settings)
export function tagState(tag, problems = [], next = null) {
  if (problems.length) return { level: "bad", text: problems.map((p) => PROBLEM_TEXT[p] || p).join(", ") };
  if (next?.late) return { level: "busy", text: "late" };
  if (tag.fw_update) return { level: "busy", text: "updating firmware" };
  if (tag.pending) return { level: "busy", text: tag.content_id ? "new content waiting" : "no content yet" };
  if (tag.config_pending) return { level: "busy", text: "settings waiting" };
  return { level: "ok", text: "OK" };
}

// The rows of the tag table: { tag, problems, next, state }, tags with problems first, then by MAC
export function tagRows(list, health, now = Date.now()) {
  const byMac = new Map((health?.problems || []).map((p) => [p.mac, p.problems]));
  const rows = (list?.tags || []).map((tag) => {
    const problems = byMac.get(tag.mac) || [];
    const next = nextCheckin(tag, now);
    return { tag, problems, next, state: tagState(tag, problems, next) };
  });
  const rank = { bad: 0, busy: 1, ok: 2 };
  return rows.sort((a, b) => rank[a.state.level] - rank[b.state.level] || a.tag.mac.localeCompare(b.tag.mac));
}

// How many rows each filter of the tag table shows: { all, bad, busy, models: { "2.9": n } }
export function rowCounts(rows) {
  const c = { all: rows.length, bad: 0, busy: 0, models: {} };
  for (const r of rows) {
    if (r.state.level !== "ok") c[r.state.level]++;
    if (r.tag.model) c.models[r.tag.model] = (c.models[r.tag.model] || 0) + 1;
  }
  return c;
}

// The rows a filter keeps: "all", "bad", "busy" or "model:2.9", and a part of the MAC
export function filterRows(rows, filter = "all", find = "") {
  const f = find.trim().toUpperCase();
  return rows.filter((r) =>
    (!f || r.tag.mac.includes(f)) &&
    (filter === "all" || r.state.level === filter || filter === "model:" + r.tag.model));
}

// One line about the health report
export function healthLine(h) {
  if (!h) return "";
  if (!h.tags) return "";
  if (h.ok) return h.tags === 1 ? "The tag is fine." : `All ${h.tags} tags are fine.`;
  const parts = [];
  for (const k of Object.keys(PROBLEM_TEXT)) if (h[k]) parts.push(`${h[k]} ${PROBLEM_TEXT[k]}`);
  const n = h.with_problems;
  const who = h.tags === 1 ? "The tag needs" : `${n} of ${h.tags} tags need${n === 1 ? "s" : ""}`;
  return `${who} attention: ${parts.join(", ")}.`;
}

// The AP's own figures as [label, value] pairs
export function apFacts(ap, ota) {
  if (!ap) return [];
  const facts = [
    ["Board", apBoard(ap, ota) || "–"],
    ["Firmware", ap.fw],
    ["Running for", fmtAgo(ap.uptime_s)],
    ["Last start", RESET_TEXT[ap.reset] || ap.reset || "–"],
    ["Radio channel", String(ap.channel)],
    ["Poll interval", ap.poll_s ? fmtInterval(ap.poll_s) : `tag default (${fmtInterval(TAG_BUILTIN_POLL_S)})`],
  ];
  if ("time" in ap) facts.push(["AP time", ap.time ? new Date(ap.time * 1000).toLocaleString() : "not set"]);
  if (ap.max_scheduled) facts.push(["Scheduled", `${ap.scheduled} of ${ap.max_scheduled}`]);
  if (ap.radio) facts.push(["Radio (C6)", radioText(ap.radio)]);
  return facts;
}

// The network radio's line in the AP facts: "192.168.3.249, last seen 3 s ago", or "not connected"
export function radioText(r) {
  if (!r.connected) return "not connected";
  return `${r.ip}, last seen ${fmtAgo(r.seen_s)} ago`;
}

// Which AP board this is: its name from GET /api/tags (board_name, in firmware after 0.6.0), or for an
// older AP the chip from GET /api/ota ("esp32s3" -> "ESP32-S3"), or ""
export function apBoard(ap, ota) {
  if (ap?.board_name) return ap.board_name;
  if (ap?.board) return ap.board;
  return ota?.chip ? ota.chip.toUpperCase().replace(/^ESP32-?/, "ESP32-") : "";
}

// The line that says which AP the dashboard talks to: board, name on the network, firmware
export function apIdentity(ap, ota, wifi) {
  return [apBoard(ap, ota), wifi?.hostname ? wifi.hostname + ".local" : "", ap?.fw].filter(Boolean).join(" · ");
}

// 30 -> "30 s", 120 -> "2 min", 90 -> "90 s", 3600 -> "1 h"
export function fmtInterval(s) {
  if (s % 3600 === 0) return `${s / 3600} h`;
  if (s % 60 === 0) return `${s / 60} min`;
  return `${s} s`;
}

// The poll interval for all tags (PUT /api/config poll_s): 0 lets each tag use its default
// (TAG_BUILTIN_POLL_S), otherwise POLL_MIN_S..POLL_MAX_S as the AP takes it
// (ESL_CFG_POLL_MIN_S/MAX_S in shared/proto.h)
export const POLL_MIN_S = 10;
export const POLL_MAX_S = 3600;
// 60 s is left out: it is the tag default (0), which the AP sends as 60 s anyway
export const POLL_PRESETS = [30, 120, 300, 600, 1800, 3600];

// The "Other" field: "300" -> { poll_s: 300 }, anything else (empty too) -> { error }
export function parsePoll(text) {
  const t = String(text).trim();
  const v = Number(t);
  if (t === "" || !Number.isInteger(v) || v < POLL_MIN_S || v > POLL_MAX_S)
    return { error: `The interval must be a whole number of seconds from ${POLL_MIN_S} to ${POLL_MAX_S}.` };
  return { poll_s: v };
}

// Years on one CR2450 (a 1.54" tag, one display update a day) for a poll interval, from
// the table in docs/strombudsjett.md (tools/power-budget); between two rows the nearer one
const BATTERY_YEARS = [[30, 7.5], [60, 9.3], [120, 10.7], [300, 11.7], [600, 12.1]];
export function batteryYears(poll_s) {
  const s = poll_s || TAG_BUILTIN_POLL_S;
  let best = BATTERY_YEARS[0];
  for (const row of BATTERY_YEARS) if (Math.abs(Math.log(row[0] / s)) < Math.abs(Math.log(best[0] / s))) best = row;
  return s < 30 ? null : best[1];
}

// What an interval means for the tags: how long a new price may take, the battery, and the
// tags that keep their own interval (config.poll_s in GET /api/tags) and so ignore it
export function pollNote(poll_s, tags = []) {
  const s = poll_s || TAG_BUILTIN_POLL_S;
  const years = batteryYears(s);
  const parts = [`A new price shows within ${fmtInterval(s)}.`];
  if (years >= 10) parts.push(`Over 10 years on one CR2450, so the cells' shelf life sets the limit, not the polls.`);
  else if (years) parts.push(`About ${years} years on one CR2450 (a 1.54" tag, one update a day; a 2.9" tag lasts longer).`);
  else parts.push(`Polls this often drain the battery noticeably faster; 60 s or more is kinder.`);
  const own = tags.filter((t) => t.config?.poll_s).length;
  if (own) parts.push(own === 1 ? "One tag has an interval of its own set and keeps it." : `${own} tags have an interval of their own set and keep it.`);
  return parts.join(" ");
}

// The radio's transmit power (PUT /api/config "tx_power"): the choices for the form, as
// [dbm, label], highest first. The EU allows about 13 dBm EIRP for 802.15.4 (EN 300 328:
// 10 dBm/MHz on a signal about 2 MHz wide), so an antenna of G dBi takes at most about
// 13 - G; the labels say which antenna each fits. The AP's value now and its
// built-in one are always there.
export const TX_EU_MAX = 13; // dBm EIRP for 802.15.4 in the EU, with an antenna of up to 0 dBi
export const TX_MIN = -24, TX_MAX = 20; // what the C6 takes (PUT /api/config)
export const TX_PRESETS = [[20, "most, above the EU limit"], [13, "EU max with a chip or PCB antenna"], [10, "EU max with a 3 dBi antenna"], [8, "EU max with a 5 dBi antenna"], [5, "EU max with an 8 dBi antenna"], [0, ""]];
export function txOptions(dbm, builtin) {
  const all = new Map(TX_PRESETS);
  for (const v of [dbm, builtin]) if (Number.isInteger(v) && !all.has(v)) all.set(v, "");
  return [...all].sort((a, b) => b[0] - a[0]).map(([v, fits]) => {
    const notes = [fits, v === builtin ? "built-in" : ""].filter(Boolean);
    return [v, `${v} dBm` + (notes.length ? ` (${notes.join(", ")})` : "")];
  });
}
// A transmit power typed in: { dbm } or { error }
export function parseTx(text) {
  const t = String(text).trim();
  const v = Number(t);
  if (t === "" || !Number.isInteger(v) || v < TX_MIN || v > TX_MAX)
    return { error: `The transmit power must be a whole number of dBm from ${TX_MIN} to ${TX_MAX}.` };
  return { dbm: v };
}
// A warning for a power above the EU limit with a chip or PCB antenna, else ""
export const txWarning = (dbm) => dbm > TX_EU_MAX
  ? `${dbm} dBm is above the EU limit (about ${TX_EU_MAX} dBm with a chip or PCB antenna); fine outside the EU where more is allowed.` : "";
export const TX_NOTE = "For this radio the EU allows about 13 dBm including the antenna's gain (EN 300 328: 10 dBm/MHz): with an antenna of G dBi, the limit is about 13 − G. "
  + "More power reaches tags further away, but their answers do not get stronger; for those, a better antenna on the AP helps.";

// The radio channel (PUT /api/config "channel"): 802.15.4 channels 11-26, as [channel, label].
// 15, 20, 25 and 26 lie between or above WiFi channels 1, 6 and 11, the ones most networks
// use; the built-in one is marked.
export const CHANNEL_MIN = 11, CHANNEL_MAX = 26;
export const CHANNEL_CLEAR = [15, 20, 25, 26];
export function channelOptions(builtin) {
  const out = [];
  for (let c = CHANNEL_MIN; c <= CHANNEL_MAX; c++) {
    const notes = [CHANNEL_CLEAR.includes(c) ? "clear of WiFi 1, 6, 11" : "", c === builtin ? "built-in" : ""].filter(Boolean);
    out.push([c, `Channel ${c}` + (notes.length ? ` (${notes.join(", ")})` : "")]);
  }
  return out;
}
// A move the AP is waiting with (GET /api/config channel_next), as a sentence, or "" for none
export function channelMove(config) {
  const c = config || {};
  if (!Number.isInteger(c.channel_next)) return "";
  const n = c.channel_waiting || 0;
  if (!n) return `Moving from channel ${c.channel} to ${c.channel_next}: every tag has it, so the AP moves now.`;
  const wait = n === 1 ? "1 tag has" : `${n} tags have`;
  const due = Number.isInteger(c.channel_due_s) ? `, at the latest in ${fmtInterval(Math.max(60, Math.ceil(c.channel_due_s / 60) * 60))}` : "";
  return `Moving from channel ${c.channel} to ${c.channel_next}: ${wait} not confirmed it yet. The AP moves once they all have it${due}.`;
}
// What keeps the AP from moving off its built-in channel (GET /api/config): a radio on WiFi with
// firmware from before 0.9.0 (it retunes but does not tell the tags), or tags heard lately whose
// firmware cannot follow a move; "" when nothing does
export function channelBlock(config) {
  const c = config || {};
  if (c.channel_radio_old) return "The radio on WiFi runs firmware from before 0.9.0, which does not tell the tags of a new channel: update the radio before you change the channel.";
  const n = c.channel_old_tags || 0, u = c.channel_unheard_tags || 0, s = c.channel_stopped || 0;
  const said = [];
  if (s && !Number.isInteger(c.channel_next)) said.push(`The last channel change was called off: ${s === 1 ? "a tag" : `${s} tags`} with firmware from before 0.9.0 showed up, which would have been left behind.`);
  if (n) said.push(`${n === 1 ? "1 tag runs" : `${n} tags run`} firmware from before 0.9.0 and cannot follow a channel change: update ${n === 1 ? "it" : "them"} first (Firmware), or ${n === 1 ? "it is" : "they are"} left on the old channel.`);
  else if (u) said.push(`${u === 1 ? "1 tag has" : `${u} tags have`} not polled since the AP started, so the AP does not know yet whether ${u === 1 ? "it" : "they"} can follow a channel change: wait until ${u === 1 ? "it has" : "they have"}.`);
  return said.join(" ");
}

export const CHANNEL_NOTE = "The AP tells every tag at its next poll and moves once they have confirmed it, so no tag is left behind. "
  + "A tag that misses it tries the new channel as soon as it stops hearing the AP.";

// What the AP's image of a tag depends on: a new key fetches it again (a tag whose model
// becomes known, new content, a new image id, or a new layout file for its model; the last
// matters for a tag the AP has not drawn for, e.g. one without SPI flash). layouts is
// GET /api/layouts, or null
export function screenKey(tag, layouts) {
  const file = layouts?.layouts?.find((x) => x.model === tag.model)?.version ?? "";
  return `${tag.content_id}/${tag.model}/${tag.image_id ?? ""}/${file}`;
}

// Whether a failed GET /api/tags/{mac}/image stands until the key changes: a 4xx does
// (unknown tag or model, no content, content that cannot be drawn, an AP without
// images); anything else (429, 503, no answer, an AP restarting) is tried again
export function screenErrorFinal(err) {
  const status = err?.status ?? Number(/^(\d{3}):/.exec(String(err?.message))?.[1]);
  return status >= 400 && status < 500 && status !== 429;
}

// GET /api/tags/{mac}/image (docs/openapi.yaml): the screen as width x height pixels,
// 0 white, 1 black, 2 red; null when the data is short
export function decodeTagImage({ width, height, red, data }) {
  const bytes = Uint8Array.from(atob(data), (c) => c.charCodeAt(0));
  const col = Math.ceil(height / 8);
  const plane = width * col;
  if (!width || !height || bytes.length < plane * (red ? 2 : 1)) return null;
  const pixels = new Uint8Array(width * height);
  for (let x = 0; x < width; x++) {
    for (let y = 0; y < height; y++) {
      const i = x * col + (y >> 3);
      const m = 0x80 >> (y & 7);
      pixels[y * width + x] = red && bytes[plane + i] & m ? 2 : bytes[i] & m ? 1 : 0;
    }
  }
  return { width, height, pixels };
}
