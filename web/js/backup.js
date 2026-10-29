// backup.js - the AP's backup file from the dashboard: the same file as `esl_client.py backup`
// and `restore` (tools/esl-client), so one made here goes back with esl-client and the other
// way round. In it: the AP's settings (poll interval, webhook, NTP server, time zone), the
// layout files, every tag's content (pictures too) and own settings, and the content scheduled
// for later, read through the API (docs/openapi.yaml). Not in it: the WiFi password and the API
// token, which the AP never gives out, and the tag firmware images, which come from the
// release (the backup notes which ones the AP offered). A restore can be run again: what is
// there already is left as it is, so a picture that had to wait for its tag (an AP that has
// not heard it yet) goes on at the next run.

export const BACKUP_FORMAT = "openesl-backup";
export const BACKUP_VERSION = 1;
const CONTENT_KEYS = ["layout", "fields", "price_highlight", "frame"];
const CONFIG_KEYS = ["poll_s", "webhook", "ntp", "tz"];
const BATCH = 25; // tags per PUT /api/tags: well inside the AP's 50 and its 24 KB body over USB
const MAC_RE = /^[0-9A-F]{16}$/;

const pick = (o, keys) => Object.fromEntries(keys.filter((k) => o?.[k] != null).map((k) => [k, o[k]]));
const b64decode = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

// The backup as an object (saved as JSON). progress({ done, total }) says how far.
export async function makeBackup(client, { progress = () => {}, now = () => new Date() } = {}) {
  const optional = (p) => client.call("GET", p).catch(() => null);
  const list = await client.call("GET", "/api/tags");
  const config = (await optional("/api/config")) || {};
  const wifi = await optional("/api/wifi");
  const layouts = await optional("/api/layouts");
  const schedule = await optional("/api/schedule");
  const tagfw = await optional("/api/tag-firmware");

  const files = (layouts?.layouts || []).filter((l) => l.version != null);
  const withContent = list.tags.filter((t) => t.content_id);
  const total = files.length + withContent.length;
  let done = 0;
  const step = () => progress({ done: ++done, total });

  const files_ = {};
  for (const l of files) {
    files_[l.model] = await client.call("GET", "/api/layouts/" + encodeURIComponent(l.model));
    step();
  }
  const tags = [];
  for (const t of list.tags) {
    const row = { mac: t.mac };
    if (t.content_id) {
      const d = await client.call("GET", "/api/tags/" + t.mac);
      if (d.layout === "image") {
        // a picture: its raw bytes, put back with PUT .../image; one the AP cannot give back is left out
        const img = await optional(`/api/tags/${t.mac}/image`);
        if (img?.data) row.image = img.data;
      } else Object.assign(row, pick(d, CONTENT_KEYS));
      step();
    }
    const cfg = pick(t.config, ["poll_s", "max_poll_s", "lost_ap_s"]);
    if (Object.keys(cfg).length) row.config = cfg;
    if (Object.keys(row).length > 1) tags.push(row); // a tag that has only polled comes back when it polls again
  }
  const scheduled = (schedule?.scheduled || []).map((e) => ({ mac: e.mac, at: e.at, ...pick(e, CONTENT_KEYS) }))
    .sort((a, b) => a.at - b.at || a.mac.localeCompare(b.mac));
  const ap = { fw: list.ap.fw, channel: list.ap.channel };
  if (list.ap.board) ap.board = list.ap.board;
  if (wifi?.hostname) ap.hostname = wifi.hostname;
  const tag_firmware = (tagfw?.images || []).filter((i) => i.version).map((i) => ({
    ...pick(i, ["model", "version", "crc", "to"]), waiting: (i.tags?.pending || 0) + (i.tags?.failed || 0),
  }));
  return {
    format: BACKUP_FORMAT,
    version: BACKUP_VERSION,
    taken: now().toISOString().replace(/\.\d+Z$/, "+00:00"),
    ap,
    // poll_s and webhook always, as esl-client writes them; ntp and tz from an AP that has them
    // the NTP server set in menuconfig is kept as null, so another AP keeps its own
    config: { poll_s: config.poll_s ?? 0, webhook: config.webhook ?? null,
      ...("ntp" in config ? { ntp: config.ntp === config.builtin_ntp ? null : config.ntp } : {}),
      ...("tz" in config ? { tz: config.tz ?? null } : {}) },
    layouts: files_,
    tags,
    scheduled,
    tag_firmware,
  };
}

// "openesl-backup-<host>-2026-10-26.json"
export function backupName(b) {
  const host = String(b.ap?.hostname || "ap").replace(/[^A-Za-z0-9-]/g, "");
  return `openesl-backup-${host}-${String(b.taken || "").slice(0, 10)}.json`;
}

// A file read back: the backup, or throws with what is wrong (the checks of esl-client's read_backup)
export function parseBackup(text) {
  let b;
  try {
    b = JSON.parse(text);
  } catch {
    throw new Error("the file is not JSON");
  }
  if (b?.format !== BACKUP_FORMAT) throw new Error("the file is not an OpenESL backup");
  if (b.version !== BACKUP_VERSION) throw new Error(`the backup is version ${b.version}, this page knows ${BACKUP_VERSION}`);
  const obj = (v) => v && typeof v === "object" && !Array.isArray(v);
  for (const k of ["config", "layouts"]) if (!obj(b[k])) throw new Error(`"${k}" is missing in the backup`);
  for (const k of ["tags", "scheduled"]) if (!Array.isArray(b[k])) throw new Error(`"${k}" is missing in the backup`);
  for (const t of [...b.tags, ...b.scheduled]) {
    if (!MAC_RE.test(String(t?.mac))) throw new Error("an entry in the backup has no valid MAC");
    if ("layout" in t && !Array.isArray(t.fields)) throw new Error(`${t.mac} has a layout but no fields`);
    if ("image" in t) {
      try {
        b64decode(t.image);
      } catch {
        throw new Error(`${t.mac} has a picture that is not base64`);
      }
    }
  }
  return b;
}

// What a restore would put back, counted: for the question before it starts
export function backupSummary(b) {
  return {
    layouts: Object.keys(b.layouts),
    tags: b.tags.filter((t) => "layout" in t).length,
    pictures: b.tags.filter((t) => "image" in t).length,
    configs: b.tags.filter((t) => t.config).length,
    scheduled: b.scheduled.length,
  };
}

// Puts the backup on the AP, in the order the AP needs (as esl-client's restore): settings,
// layout files, the tags' own settings, content, pictures, then the scheduled content whose time
// has not passed. parts picks what: { settings, layouts, tags, scheduled }, all by default (tags
// covers the tags' content, pictures and own settings). Tags the backup does not have are left
// as they are. Goes on past a failure; returns { done: [lines], failed: [lines], waiting: [MACs
// of pictures for tags the AP has not heard yet], notes: [lines] }.
export async function restoreBackup(client, b, { parts = {}, progress = () => {} } = {}) {
  const want = { settings: true, layouts: true, tags: true, scheduled: true, ...parts };
  const done = [], failed = [], waiting = [], notes = [];
  const note = (ok, line) => (ok ? done : failed).push(line);
  const list = await client.call("GET", "/api/tags");
  const models = new Map(list.tags.map((t) => [t.mac, t.model]));

  const steps = [];
  if (want.settings) steps.push(["settings"]);
  if (want.layouts) for (const m of Object.keys(b.layouts)) steps.push(["layout", m]);
  if (want.tags) {
    for (const t of b.tags.filter((t) => t.config)) steps.push(["config", t]);
    const content = b.tags.filter((t) => "layout" in t);
    for (let i = 0; i < content.length; i += BATCH) steps.push(["content", content.slice(i, i + BATCH)]);
    for (const t of b.tags.filter((t) => "image" in t)) steps.push(["picture", t]);
  }
  if (want.scheduled) {
    const now = list.ap.time || Math.round(Date.now() / 1000);
    const due = b.scheduled.filter((s) => s.at > now); // those past their time the AP would set now
    if (due.length < b.scheduled.length) notes.push(`${b.scheduled.length - due.length} scheduled entries skipped: their time has passed`);
    for (let i = 0; i < due.length; i += BATCH) steps.push(["scheduled", due.slice(i, i + BATCH)]);
  }

  let n = 0;
  for (const [kind, arg] of steps) {
    try {
      if (kind === "settings") await putSettings(client, b.config, note);
      else if (kind === "layout") {
        await client.call("PUT", "/api/layouts/" + encodeURIComponent(arg), b.layouts[arg]);
        note(true, `Layout file for the ${arg}"`);
      } else if (kind === "config") {
        await client.call("PUT", `/api/tags/${arg.mac}/config`, arg.config);
        note(true, `Own settings for ${arg.mac}`);
      } else if (kind === "content") {
        await putContent(client, arg.map((t) => ({ mac: t.mac, ...pick(t, CONTENT_KEYS) })), note, "Content");
      } else if (kind === "picture") {
        // a picture needs the tag's model: a tag that has not polled this AP yet cannot have it
        if (!models.get(arg.mac)) waiting.push(arg.mac);
        else {
          const r = await client.send("PUT", `/api/tags/${arg.mac}/image`, b64decode(arg.image));
          if (r.status === 404 || r.status === 409) waiting.push(arg.mac);
          else note(r.status === 200, `Picture for ${arg.mac}` + (r.status === 200 ? "" : ": " + answer(r)));
        }
      } else if (kind === "scheduled") {
        await putContent(client, arg.map((e) => ({ mac: e.mac, at: e.at, ...pick(e, CONTENT_KEYS) })), note, "Scheduled");
      }
    } catch (e) {
      note(false, `${kind === "layout" ? `Layout file for the ${arg}"` : kind[0].toUpperCase() + kind.slice(1)}: ${e.message}`);
    }
    progress({ done: ++n, total: steps.length });
  }
  const name = b.ap?.hostname;
  if (name && want.settings) {
    const now = await client.call("GET", "/api/wifi").then((w) => w.hostname, () => null);
    if (now && now !== name) notes.push(`The old AP was called ${name} (${name}.local), this one ${now}: set the name under Settings, General to keep the old one.`);
  }
  for (const f of b.tag_firmware || [])
    if (f?.waiting) notes.push(`The old AP offered ${f.model}" tag firmware ${f.version}, ${f.waiting} tags had not taken it yet: send it again under Firmware.`);
  return { done, failed, waiting, notes };
}

// The settings this AP has (an older one has no ntp or tz) that differ from what it has now,
// so an AP without a webhook is not asked to turn one off
async function putSettings(client, config, note) {
  const cur = await client.call("GET", "/api/config");
  const same = (k, v) => v === cur[k] || ((v === null || v === "") && (cur[k] == null || cur[k] === cur["builtin_" + k]));
  const body = Object.fromEntries(CONFIG_KEYS.filter((k) => k in config && k in cur && !same(k, config[k])).map((k) => [k, config[k]]));
  if (Object.keys(body).length) await client.call("PUT", "/api/config", body);
  note(true, "Settings: " + (Object.keys(body).join(", ") || "already as in the backup"));
}

// The error text in an answer from send() (the body is the JSON as bytes)
function answer(r) {
  try {
    return `${r.status}: ${JSON.parse(new TextDecoder().decode(r.body)).error}`;
  } catch {
    return String(r.status);
  }
}

// A batch at once; when the AP turns it down (one bad entry stops all of it), one at a time, so
// the others still go on and the bad one is named
async function putContent(client, tags, note, what) {
  try {
    await client.call("PUT", "/api/tags", { tags });
    note(true, `${what}: ${tags.length} tag${tags.length === 1 ? "" : "s"}`);
    return;
  } catch (e) {
    if (tags.length === 1) return note(false, `${what} for ${tags[0].mac}: ${e.message}`);
  }
  for (const { mac, ...c } of tags) {
    try {
      await client.call("PUT", "/api/tags/" + mac, c);
      note(true, `${what} for ${mac}`);
    } catch (e) {
      note(false, `${what} for ${mac}: ${e.message}`);
    }
  }
}
