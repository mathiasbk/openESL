// sendtext.js - the "Send text to a tag" card on the Dashboard: four lines with the built-in
// layout text, or today's date on the first two (as `esl_client.py date` in tools/esl-client).
// With more than one day, every day after today goes to the AP as scheduled content for local
// midnight ("at" in PUT /api/tags), so the tag changes date by itself.

export const MAX_DAYS = 31;      // one PUT /api/tags (at most 50 rows), well inside the schedule
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
export async function sendRows(client, rows) {
  if (rows.length === 1) {
    const { mac, ...content } = rows[0];
    return { replies: [await client.call("PUT", "/api/tags/" + encodeURIComponent(mac), content)], note: "" };
  }
  const note = await ensureTime(client);
  const r = await client.call("PUT", "/api/tags", { tags: rows });
  return { replies: Array.isArray(r) ? r : r?.tags || [], note };
}
