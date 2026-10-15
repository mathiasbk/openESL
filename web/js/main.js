// main.js - tabs, and the two flasher pages around js/tagflash.js and esp-web-tools.
import { initEditor } from "./editor.js";
import * as zp from "./zbs.js";
import { flashTag, checkFirmware, BackupStore, MODELS as TAG_MODELS } from "./tagflash.js";

const $ = (id) => document.getElementById(id);

// ---------- tabs ----------

function showTab(name) {
  document.querySelectorAll(".tabs button").forEach((b) => b.setAttribute("aria-selected", b.dataset.tab === name));
  document.querySelectorAll(".tab").forEach((t) => (t.hidden = t.id !== "tab-" + name));
  if (location.hash !== "#" + name) history.replaceState(null, "", "#" + name);
}
document.querySelectorAll(".tabs button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));
showTab(["design", "tag", "ap"].includes(location.hash.slice(1)) ? location.hash.slice(1) : "design");

initEditor().catch((e) => {
  const s = $("status");
  s.textContent = "Could not start the renderer: " + e.message;
  s.className = "status error";
});

// ---------- firmware info (dist/firmware/info.json, written by the GitHub workflow) ----------

async function firmwareInfo() {
  try {
    const r = await fetch("firmware/info.json", { cache: "no-cache" });
    return r.ok ? await r.json() : null;
  } catch {
    return null;
  }
}

firmwareInfo().then((info) => {
  const built = info ? `Built from ${info.ref} (${info.commit.slice(0, 7)}), ${info.date}.` : "";
  $("ap-fw-info").textContent = info?.ap ? built : "No AP firmware is published with this page yet.";
  $("tag-fw-info").textContent = info?.tag ? built : "No tag firmware is published with this page; choose a file.";
  if (!info?.tag) $("tag-fw-src").value = "file";
});

// ---------- tag flasher ----------

let flasher = null;
let transport = null;
let backups = null;
let busy = false;
let lastMacSeen = false;

const log = (text, cls) => {
  const el = $("tag-log");
  const line = document.createElement("div");
  line.textContent = text;
  if (cls) line.className = cls;
  el.append(line);
  el.scrollTop = el.scrollHeight;
};

if (!("serial" in navigator)) {
  $("serial-missing").hidden = false;
  $("tag-connect").disabled = true;
}

$("tag-fw-src").addEventListener("change", (ev) => {
  if (ev.target.value === "file") $("tag-fw-file").click();
});

async function loadTagFirmware() {
  const model = $("tag-model").value;
  if ($("tag-fw-src").value === "file") {
    const f = $("tag-fw-file").files[0];
    if (!f) throw new Error("choose a firmware file (.bin) first");
    return checkFirmware(new Uint8Array(await f.arrayBuffer()));
  }
  const r = await fetch(TAG_MODELS[model].firmware, { cache: "no-cache" });
  if (!r.ok) throw new Error(`the ${TAG_MODELS[model].name} firmware is not published with this page (${r.status}); choose a file`);
  return checkFirmware(new Uint8Array(await r.arrayBuffer()));
}

$("tag-connect").addEventListener("click", async () => {
  try {
    if (transport) {
      await transport.close();
      transport = flasher = null;
      $("tag-connect").textContent = "Connect flasher";
      $("tag-flash").disabled = true;
      log("Disconnected");
      return;
    }
    const port = await navigator.serial.requestPort();
    transport = await zp.WebSerialTransport.open(port);
    flasher = new zp.ZbsFlasher(transport);
    log("Port open, asking the flasher for its version ...");
    const v = await zp.probe(flasher);
    log(`ZBS_Flasher version ${v}`, "good");
    $("tag-connect").textContent = "Disconnect";
    $("tag-flash").disabled = false;
  } catch (e) {
    log("Connect failed: " + e.message, "err");
    if (transport) await transport.close().catch(() => {});
    transport = flasher = null;
  }
});

async function waitForTag(stopped) {
  log("Waiting for the next tag in the jig ...");
  for (;;) {
    if (stopped()) return false;
    try {
      await flasher.begin();
      return true;
    } catch (e) {
      if (!(e instanceof zp.NoTagError)) throw e;
    }
    await new Promise((r) => setTimeout(r, 1000));
  }
}

$("tag-flash").addEventListener("click", async () => {
  if (busy) {
    busy = false;           // the loop stops after the current tag
    $("tag-flash").textContent = "Stopping after this tag ...";
    return;
  }
  busy = true;
  $("tag-flash").textContent = "Stop";
  const bar = $("tag-progress");
  try {
    backups ||= await BackupStore.open();
    const fw = await loadTagFirmware();
    const model = $("tag-model").value;
    let lastMac = null;
    lastMacSeen = false;
    do {
      if (lastMac) {
        log("Take the tag out of the jig ...");
        while (busy && (await tagStillThere())) await new Promise((r) => setTimeout(r, 1000));
        if (!busy || !(await waitForTag(() => !busy))) break;
      }
      bar.hidden = false;
      const res = await flashTag(flasher, model, fw, backups, {
        log: (t) => log(t, t.startsWith("FAILED") ? "err" : t.startsWith("OK") ? "good" : ""),
        progress: (label, d, t) => { bar.max = t; bar.value = d; },
      });
      bar.hidden = true;
      lastMac = res.mac;
      lastMacSeen = true;
      refreshBackups();
      if (!res.ok && !lastMac) lastMac = "?";
    } while (busy && $("tag-loop").checked);
  } catch (e) {
    log("Stopped: " + e.message, "err");
  } finally {
    // polling for the next tag leaves a tag in debug mode; let it run its firmware
    if (flasher && lastMacSeen) await flasher.reset().catch(() => {});
    busy = false;
    bar.hidden = true;
    $("tag-flash").textContent = "Flash tag in jig";
  }
});

// After a job the flasher has reset the tag; a tag still in the jig answers ZBS_BEGIN.
async function tagStillThere() {
  try {
    await flasher.begin();
    return true;
  } catch (e) {
    if (e instanceof zp.NoTagError) return false;
    throw e;
  }
}

async function refreshBackups() {
  const body = $("backup-list");
  try {
    backups ||= await BackupStore.open();
    const all = await backups.list();
    body.replaceChildren();
    if (!all.length) {
      body.innerHTML = '<tr><td colspan="3" class="muted">None yet.</td></tr>';
      return;
    }
    for (const b of all) {
      const tr = document.createElement("tr");
      const td = (t) => { const c = document.createElement("td"); c.textContent = t; tr.append(c); return c; };
      td(b.mac);
      td(new Date(b.time).toLocaleString());
      const c = td("");
      for (const [label, data, name] of [["flash", b.flash, b.mac + ".bin"], ["info page", b.info, b.mac + ".info.bin"]]) {
        const a = document.createElement("a");
        a.href = URL.createObjectURL(new Blob([data]));
        a.download = name;
        a.textContent = label;
        c.append(a, " ");
      }
      body.append(tr);
    }
  } catch (e) {
    body.innerHTML = "";
    const tr = document.createElement("tr");
    tr.innerHTML = '<td colspan="3" class="warn"></td>';
    tr.firstChild.textContent = "Backups unavailable in this browser: " + e.message + ". Flashing is blocked without them.";
    body.append(tr);
  }
}
refreshBackups();
