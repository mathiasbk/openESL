// sendtext.js - the "Send info to tags" card on the Dashboard: four lines with the built-in
// layout text to one or more tags. The lines may hold placeholders such as {ap_ip}, {mac} or
// {date}, filled in for each tag from GET /api/tags and /api/wifi. When a line has the date,
// every day after today goes to the AP as scheduled content for local midnight ("at" in
// PUT /api/tags), so the tag changes date by itself (as `esl_client.py date` in tools/esl-client).
import { fmtAgo, fmtTagFw } from "./dashdata.js";

export const MAX_DAYS = 31;      // one PUT /api/tags (at most 50 rows), well inside the schedule
export const MAX_ROWS = 50;      // rows in one PUT /api/tags
export const TEXT_BYTES = 96;    // the fields share 96 bytes: Latin-1 text plus one per field

const WEEKDAYS = ["søndag", "mandag", "tirsdag", "onsdag", "torsdag", "fredag", "lørdag"];
const MONTHS = ["januar", "februar", "mars", "april", "mai", "juni", "juli", "august", "september",
  "oktober", "november", "desember"];

// "Lørdag", "17. oktober 2026" for a Date (local time)
export function dateLines(d) {
  const w = WEEKDAYS[d.getDay()];
  return [w[0].toUpperCase() + w.slice(1), `${d.getDate()}. ${MONTHS[d.getMonth()]} ${d.getFullYear()}`];
}

// The fields as sent: empty lines at the end are left out
export function textFields(lines) {
  const f = lines.map((s) => String(s ?? ""));
  while (f.length && !f[f.length - 1].trim()) f.pop();
  return f.length ? f : [""];
}

// Bytes the AP counts: each character is one Latin-1 byte (others become "?"), plus one per field
export function textBytes(fields) {
  return fields.reduce((n, s) => n + [...s].length + 1, 0);
}

// Unix seconds of local midnight at the start of d
export function midnight(d) {
  return Math.floor(new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime() / 1000);
}

// The rows for PUT /api/tags: today without "at", then each next day at midnight.
// lines: the four inputs; with date, the first two are replaced by the day's date.
export function contentRows(mac, lines, { date = false, days = 1, frame = false, today = new Date() } = {}) {
  const n = date ? Math.max(1, Math.min(MAX_DAYS, days | 0)) : 1;
  const rows = [];
  for (let i = 0; i < n; i++) {
    const d = new Date(today.getFullYear(), today.getMonth(), today.getDate() + i);
    const l = date ? [...dateLines(d), ...lines.slice(2)] : lines;
    const r = { mac, layout: "text", fields: textFields(l) };
    if (frame) r.frame = true;
    if (i) r.at = midnight(d);
    rows.push(r);
  }
  return rows;
}

// Before scheduling: the AP must know the time (an AP without WiFi has no NTP). Sets it from
// this computer when it does not; returns a warning when the clocks are more than 2 min apart.
export async function ensureTime(client, now = Date.now()) {
  const t = await client.call("GET", "/api/time");
  const s = Math.floor(now / 1000);
  if (t?.time == null) {
    await client.call("PUT", "/api/time", { time: s });
    return "The AP did not know the time; it has been set from this computer.";
  }
  if (Math.abs(t.time - s) > 120)
    return `The AP's clock is ${t.time - s} s off this computer's; the date changes by the AP's clock.`;
  return "";
}

// Sends the rows: one tag now, or the whole set in one PUT /api/tags. Returns { replies, note }.
// The AP's clock is only checked when something is scheduled.
export async function sendRows(client, rows) {
  if (rows.length === 1) {
    const { mac, ...content } = rows[0];
    return { replies: [await client.call("PUT", "/api/tags/" + encodeURIComponent(mac), content)], note: "" };
  }
  const note = rows.some((r) => r.at) ? await ensureTime(client) : "";
  const r = await client.call("PUT", "/api/tags", { tags: rows });
  return { replies: Array.isArray(r) ? r : r?.tags || [], note };
}

// ---- Placeholders ----

// [name, what it gives]: {name} in a line is replaced for each tag
export const PLACEHOLDERS = [
  ["ap_ip", "the AP's IP address"],
  ["ap_name", "the AP's name on the network (NAME.local)"],
  ["wifi", "the AP's WiFi network"],
  ["ap_fw", "the AP's firmware"],
  ["channel", "the radio channel"],
  ["tags", "how many tags the AP has"],
  ["mac", "the tag's MAC"],
  ["model", "the tag's model"],
  ["battery", "the tag's battery"],
  ["rssi", "the tag's signal"],
  ["fw", "the tag's firmware"],
  ["seen", "how long since the tag last polled"],
  ["weekday", "the weekday (Norwegian)"],
  ["date", "the date (Norwegian)"],
  ["time", "the time it is sent"],
];

// Ready-made lines for the card's "Ready-made" list
export const PRESETS = {
  ap: ["OpenESL AP", "IP {ap_ip}", "WiFi {wifi}", "FW {ap_fw}"],
  tag: ["{mac}", '{model} FW {fw}', "Batteri {battery}", "Signal {rssi}"],
  date: ["{weekday}", "{date}", "", ""],
  clear: ["", "", "", ""],
};

const DATE_RE = /\{(weekday|date)\}/;

// A line with the weekday or date changes from day to day, so the days count
export const usesDate = (lines) => lines.some((l) => DATE_RE.test(l));

// How many days fit for n tags in one PUT /api/tags
export const maxDays = (n) => Math.max(1, Math.min(MAX_DAYS, Math.floor(MAX_ROWS / Math.max(1, n))));

const dash = (v) => (v == null || v === "" ? "-" : String(v));
const pad = (n) => String(n).padStart(2, "0");

// The values for one tag on one day. tag: a row of GET /api/tags; ap: its "ap"; wifi: GET /api/wifi
export function infoValues(tag, { ap = null, wifi = null, tags = null, day = new Date(), now = new Date() } = {}) {
  const [weekday, date] = dateLines(day);
  return {
    ap_ip: dash(wifi?.connected ? wifi.ip : null),
    ap_name: dash(wifi?.hostname),
    wifi: dash(wifi?.ssid),
    ap_fw: dash(ap?.fw),
    channel: dash(ap?.channel),
    tags: dash(tags),
    mac: dash(tag?.mac),
    model: tag?.model ? tag.model + '"' : "-",
    battery: tag?.battery_mv ? (tag.battery_mv / 1000).toFixed(2) + " V" : "-",
    rssi: tag?.rssi == null ? "-" : tag.rssi + " dBm",
    fw: tag?.fw == null ? "-" : fmtTagFw(tag.fw),
    seen: tag?.seen_s_ago == null ? "-" : fmtAgo(tag.seen_s_ago),
    weekday,
    date,
    time: `${pad(now.getHours())}:${pad(now.getMinutes())}`,
  };
}

// "IP {ap_ip}" -> "IP 192.168.1.40"; names it does not know are left as they are
export function fillLine(line, values) {
  return String(line ?? "").replace(/\{(\w+)\}/g, (m, k) => (k in values ? values[k] : m));
}

// The rows for PUT /api/tags for every tag in tags (rows of GET /api/tags): today without
// "at", then, when a line has the date, each next day at midnight. Tag by tag, day by day.
export function infoRows(tags, lines, { days = 1, frame = false, ap = null, wifi = null, total = null, today = new Date() } = {}) {
  const n = usesDate(lines) ? Math.max(1, Math.min(maxDays(tags.length), days | 0)) : 1;
  const rows = [];
  for (const tag of tags) {
    for (let i = 0; i < n; i++) {
      const day = new Date(today.getFullYear(), today.getMonth(), today.getDate() + i);
      const values = infoValues(tag, { ap, wifi, tags: total, day, now: today });
      const r = { mac: tag.mac, layout: "text", fields: textFields(lines.map((l) => fillLine(l, values))) };
      if (frame) r.frame = true;
      if (i) r.at = midnight(day);
      rows.push(r);
    }
  }
  return rows;
}
