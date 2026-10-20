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
export function apFacts(ap) {
  if (!ap) return [];
  const facts = [
    ["Firmware", ap.fw],
    ["Running for", fmtAgo(ap.uptime_s)],
    ["Last start", RESET_TEXT[ap.reset] || ap.reset || "–"],
    ["Radio channel", String(ap.channel)],
    ["Poll interval", ap.poll_s ? `${ap.poll_s} s` : "tags' own"],
  ];
  if ("time" in ap) facts.push(["AP time", ap.time ? new Date(ap.time * 1000).toLocaleString() : "not set"]);
  if (ap.max_scheduled) facts.push(["Scheduled", `${ap.scheduled} of ${ap.max_scheduled}`]);
  return facts;
}

// What the AP's image of a tag depends on: a new key fetches it again (a tag whose model
// becomes known, new content, or a new layout file, which changes the image id)
export function screenKey(tag) {
  return `${tag.content_id}/${tag.model}/${tag.image_id ?? ""}`;
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
