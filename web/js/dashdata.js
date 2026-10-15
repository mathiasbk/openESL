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

// The tag's fw_ver (major*10000 + minor*100 + patch): 100 -> "0.1.0"
export function fmtTagFw(v) {
  if (v == null) return "–";
  return `${Math.floor(v / 10000)}.${Math.floor(v / 100) % 100}.${v % 100}`;
}

export function fmtBattery(mv) {
  if (mv == null) return "–";
  if (mv === 0) return "unknown";
  return (mv / 1000).toFixed(2) + " V";
}

export const PROBLEM_TEXT = {
  silent: "silent",
  unseen: "not seen since the AP started",
  draw_failed: "drawing failed",
  low_battery: "low battery",
  layout_failed: "rejected the layout file",
  content_error: "content cannot be sent",
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

// What a tag is doing, in a few words: the problems the AP reports first, then whether it
// is still getting something (content, layout file, settings)
export function tagState(tag, problems = []) {
  if (problems.length) return { level: "bad", text: problems.map((p) => PROBLEM_TEXT[p] || p).join(", ") };
  if (tag.layout_pending) return { level: "busy", text: "downloading the layout file" };
  if (tag.pending) return { level: "busy", text: tag.content_id ? "new content waiting" : "no content yet" };
  if (tag.config_pending) return { level: "busy", text: "settings waiting" };
  return { level: "ok", text: "OK" };
}

// The rows of the tag table: { tag, problems, state }, tags with problems first, then by MAC
export function tagRows(list, health) {
  const byMac = new Map((health?.problems || []).map((p) => [p.mac, p.problems]));
  const rows = (list?.tags || []).map((tag) => {
    const problems = byMac.get(tag.mac) || [];
    return { tag, problems, state: tagState(tag, problems) };
  });
  const rank = { bad: 0, busy: 1, ok: 2 };
  return rows.sort((a, b) => rank[a.state.level] - rank[b.state.level] || a.tag.mac.localeCompare(b.tag.mac));
}

// One line about the health report
export function healthLine(h) {
  if (!h) return "";
  if (h.ok) return `All ${h.tags} tags are fine.`;
  const parts = [];
  for (const k of Object.keys(PROBLEM_TEXT)) if (h[k]) parts.push(`${h[k]} ${PROBLEM_TEXT[k]}`);
  return `${h.with_problems} of ${h.tags} tags need attention: ${parts.join(", ")}.`;
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
