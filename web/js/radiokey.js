// radiokey.js - the AP's radio key (GET /api/radio-key, docs/protokoll.md "Security"), shown so
// it gets saved: every paired tag signs its frames with a key made from it, so an AP that is
// erased or replaced without it loses every paired tag until each is flashed again in the jig.
// The AP makes the key the first time it starts firmware with radio security (0.9.0), so the
// page reads it after the AP is up: the Flash AP tab over USB (js/apwizard.js), the dashboard
// whenever this browser has not been told the key is saved (after an update over the API, too),
// and Settings, Backup on request.
//
// The key file is a backup (js/backup.js) with only the key in it, so Restore on the dashboard
// and `esl-client restore` put it back. A copy can also be kept in this browser (localStorage of
// this page's address): anyone who can use the browser profile can read it, so it is a choice,
// and copies are only added, never replaced, so an old key is not lost when an AP gets a new one.
import { BACKUP_FORMAT, BACKUP_VERSION } from "./backup.js";

const KEPT = "openesl.radiokeys";        // [{ key, ap, saved }]
const SAVED = "openesl.radiokey.saved";  // fingerprints of the keys the user said are saved

const KEY_RE = /^[0-9A-F]{32}$/;

// localStorage, or nothing in a private window or with storage turned off
const local = {
  get: (k) => { try { return localStorage.getItem(k); } catch { return null; } },
  set: (k, v) => { try { localStorage.setItem(k, v); return true; } catch { return false; } },
};

const readList = (store, k) => {
  try {
    const v = JSON.parse(store.get(k) || "[]");
    return Array.isArray(v) ? v : [];
  } catch {
    return [];
  }
};

// A short fingerprint (FNV-1a, 32 bits) to remember that a key was saved without keeping the key
export function fingerprint(key) {
  let h = 0x811c9dc5;
  for (const c of String(key).toUpperCase()) h = Math.imul(h ^ c.charCodeAt(0), 0x01000193) >>> 0;
  return h.toString(16).padStart(8, "0");
}

// "0011 2233 4455 ..." for reading it off the screen
export const groupKey = (key) => String(key).toUpperCase().match(/.{1,4}/g).join(" ");

// What tells one AP from another in a list: { hostname, ip, setup_ssid, board, fw }, as known
export function apLabel(ap = {}) {
  const name = ap.hostname || ap.setup_ssid || "AP";
  return [name, ap.ip, ap.board].filter(Boolean).join(" · ");
}

// The key file: a backup that restores only the key
export function keyBackup(key, ap = {}, now = new Date()) {
  const info = Object.fromEntries(["hostname", "ip", "setup_ssid", "board", "fw"].filter((k) => ap[k] != null && ap[k] !== "").map((k) => [k, ap[k]]));
  return {
    format: BACKUP_FORMAT,
    version: BACKUP_VERSION,
    taken: now.toISOString().replace(/\.\d+Z$/, "+00:00"),
    note: "OpenESL radio key. Keep this file secret and away from the AP: without the key, paired tags must be flashed again. Restore it from the dashboard (Settings, Backup) or with esl-client restore.",
    ap: info,
    radio_key: String(key).toUpperCase(),
    config: {},
    layouts: {},
    tags: [],
    scheduled: [],
  };
}

// "openesl-radio-key-<host>-2026-10-30.json"
export function keyFileName(b) {
  const host = String(b.ap?.hostname || b.ap?.setup_ssid || "ap").replace(/[^A-Za-z0-9-]/g, "");
  return `openesl-radio-key-${host}-${String(b.taken || "").slice(0, 10)}.json`;
}

// The copies kept in this browser, newest first
export function keptKeys(store = local) {
  return readList(store, KEPT).filter((e) => KEY_RE.test(String(e?.key))).sort((a, b) => String(b.saved).localeCompare(String(a.saved)));
}

export const isKept = (key, store = local) => keptKeys(store).some((e) => e.key === String(key).toUpperCase());

// Adds a copy, or brings the AP's details of one kept already up to date. False when the browser
// does not keep it (private window).
export function keepKey(key, ap = {}, store = local, now = new Date()) {
  key = String(key).toUpperCase();
  if (!KEY_RE.test(key)) return false;
  const list = readList(store, KEPT).filter((e) => e?.key !== key);
  const old = readList(store, KEPT).find((e) => e?.key === key);
  list.push({ key, ap: { ...old?.ap, ...keyBackup(key, ap).ap }, saved: old?.saved || now.toISOString() });
  return store.set(KEPT, JSON.stringify(list));
}

export function dropKey(key, store = local) {
  key = String(key).toUpperCase();
  store.set(KEPT, JSON.stringify(readList(store, KEPT).filter((e) => e?.key !== key)));
}

export const isSaved = (key, store = local) => readList(store, SAVED).includes(fingerprint(key));

export function markSaved(key, store = local) {
  const f = fingerprint(key);
  const list = readList(store, SAVED).filter((x) => x !== f);
  list.push(f);
  store.set(SAVED, JSON.stringify(list.slice(-50)));
}

// The key of an AP, or null when its firmware has no radio security (before 0.9.0: 404)
export async function readKey(client) {
  try {
    const r = await client.call("GET", "/api/radio-key");
    return r?.key && KEY_RE.test(String(r.key).toUpperCase()) ? String(r.key).toUpperCase() : null;
  } catch (e) {
    const status = e.status ?? parseInt(e.message, 10);   // ApClient says it as "404: ..."
    if (status === 404 || status === 405) return null;
    throw e;
  }
}

function download(b) {
  const url = URL.createObjectURL(new Blob([JSON.stringify(b, null, 2) + "\n"], { type: "application/json" }));
  const a = Object.assign(document.createElement("a"), { href: url, download: keyFileName(b) });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

const el = (tag, props = {}, ...kids) => {
  const e = Object.assign(document.createElement(tag), props);
  e.append(...kids.filter((k) => k != null));
  return e;
};

// The panel that shows a key and asks for it to be saved. ap: { hostname, ip, setup_ssid, board,
// fw } for the file and the browser's copy; onSaved() after "I have saved the key"; keep: whether
// "Keep a copy in this browser" starts ticked (it is kept right away then).
export function keyPanel(key, { ap = {}, onSaved = null, keep = true, title = "Save the radio key", store = local } = {}) {
  key = String(key).toUpperCase();
  const msg = el("span", { className: "hint", role: "status" });
  const copy = el("button", { type: "button", textContent: "Copy" });
  copy.onclick = async () => {
    try {
      await navigator.clipboard.writeText(key);
      msg.textContent = "Copied.";
    } catch {
      prompt("Copy the radio key:", key);
    }
  };
  const file = el("button", { type: "button", textContent: "Download key file" });
  file.onclick = () => {
    download(keyBackup(key, ap));
    msg.textContent = "Saved to your downloads: move it somewhere safe, away from this AP.";
  };
  const box = el("input", { type: "checkbox", checked: keep || isKept(key, store) });
  const keepNote = el("span", { className: "muted" });
  const setKeep = () => {
    if (box.checked) {
      if (!keepKey(key, ap, store)) {
        box.checked = false;
        keepNote.textContent = " This browser does not keep it (private window?).";
        return;
      }
    } else dropKey(key, store);
    keepNote.textContent = box.checked ? " Kept: see Dashboard, Settings, Backup on this page." : "";
  };
  box.onchange = setKeep;
  if (box.checked) setKeep();
  const done = onSaved && el("button", { type: "button", className: "primary", textContent: "I have saved the key" });
  if (done) done.onclick = () => {
    markSaved(key, store);
    onSaved();
  };
  return el("div", { className: "rk" },
    el("h2", { textContent: "⚠ " + title }),
    el("p", { innerHTML: "Paired tags only talk to an AP that has this key. <b>If it is lost</b> (the AP is erased, breaks or is replaced, with no backup), <b>every paired tag must be flashed again in the jig.</b> Keep it somewhere safe that is not this AP: a password manager, or the key file on another computer." }),
    el("p", { className: "rk-key mono", textContent: groupKey(key) }),
    el("div", { className: "buttons tight" }, copy, file, msg),
    el("label", { className: "check rk-keep" }, box, " Keep a copy in this browser", keepNote),
    el("p", { className: "muted rk-small", textContent: "The browser's copy is a spare, not the backup: anyone who can use this browser profile can read it, and it is gone if the browser data is cleared. It is kept for this page's address only." }),
    done && el("div", { className: "buttons tight" }, done));
}

// Settings, Backup: "Show radio key" for the connected AP, and the copies kept in this browser.
// getClient() gives the dashboard's client, apInfo() the AP's details for the file.
export function initKeyCard({ getClient, apInfo }) {
  const $ = (id) => document.getElementById(id);
  const show = $("rk-show");
  if (!show) return { reset() {} };
  const out = $("rk-out");
  const msg = $("rk-msg");

  function listKept() {
    const list = keptKeys();
    $("rk-kept-box").hidden = !list.length;
    $("rk-kept").replaceChildren(...list.map((e) => {
      const view = el("button", { type: "button", textContent: "Show" });
      const file = el("button", { type: "button", textContent: "Download" });
      const forget = el("button", { type: "button", className: "ghost", textContent: "Remove" });
      const key = el("span", { className: "mono", textContent: "•••• •••• ••••" });
      view.onclick = () => {
        const on = view.textContent === "Show";
        key.textContent = on ? groupKey(e.key) : "•••• •••• ••••";
        view.textContent = on ? "Hide" : "Show";
      };
      file.onclick = () => download(keyBackup(e.key, e.ap, new Date(e.saved || Date.now())));
      forget.onclick = () => {
        if (!confirm(`Remove the copy of the radio key of ${apLabel(e.ap)} from this browser? Make sure it is saved somewhere else.`)) return;
        dropKey(e.key);
        listKept();
      };
      return el("li", {}, el("b", { textContent: apLabel(e.ap) }), " ",
        el("span", { className: "muted", textContent: String(e.saved || "").slice(0, 10) }), el("br"), key, " ", view, file, forget);
    }));
  }

  show.onclick = async () => {
    const c = getClient();
    if (!c) return;
    msg.textContent = "Reading ...";
    msg.className = "hint";
    try {
      const key = await readKey(c);
      msg.textContent = key ? "" : "This AP's firmware has no radio security (before 0.9.0): it has no radio key.";
      out.replaceChildren(...(key ? [keyPanel(key, { ap: apInfo(), keep: isKept(key), title: "The radio key of this AP" })] : []));
      out.querySelector(".rk-keep input")?.addEventListener("change", listKept);
    } catch (e) {
      msg.textContent = "Could not read it: " + e.message;
      msg.className = "hint error";
    }
  };
  listKept();
  return {
    reset() {
      out.replaceChildren();
      msg.textContent = "";
      listKept();
    },
    listKept,
  };
}
