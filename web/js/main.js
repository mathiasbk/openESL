// main.js - tabs, and the two flasher pages around js/tagflash.js and esp-web-tools.
import { initEditor } from "./editor.js";
import * as zp from "./zbs.js";
import { flashTag, checkFirmware, BackupStore, modelOfMac, MODELS as TAG_MODELS } from "./tagflash.js";
import { loadRenderer, LAYOUT_PRICE, FLAG_PRICE_HIGHLIGHT } from "./renderer.js";
import { makeZip } from "./zip.js";

const $ = (id) => document.getElementById(id);

// ---------- tabs ----------

const TABS = ["home", "design", "tag", "ap", "setup", "api"];

function showTab(name) {
  if (!TABS.includes(name)) name = "home";
  document.querySelectorAll(".tabs button").forEach((b) => b.setAttribute("aria-selected", b.dataset.tab === name));
  document.querySelectorAll(".tab").forEach((t) => (t.hidden = t.id !== "tab-" + name));
  // the API reference is large: load it the first time its tab is shown
  const frame = name === "api" && document.querySelector(".api-frame:not([src])");
  if (frame) frame.src = frame.dataset.src;
  if (location.hash !== "#" + name) history.replaceState(null, "", "#" + name);
}
document.querySelectorAll(".tabs button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));
window.addEventListener("hashchange", () => {
  showTab(location.hash.slice(1));
  window.scrollTo(0, 0);
});
showTab(location.hash.slice(1));

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
  const built = !info ? "" : info.release
    ? `Release ${info.version}.`
    : `Development build ${info.version} from ${info.ref} (${info.commit.slice(0, 7)}), ${info.date}.`;
  $("home-fw").textContent = info ? "Firmware on this page: " + built : "";
  $("ap-fw-info").textContent = info?.ap ? built : "No AP firmware is published with this page yet.";
  // The AP with the status screen is optional: a checkbox picks its manifest
  $("ap-lcd-choice").hidden = $("ap-lcd-note").hidden = !info?.ap_lcd;
  $("ap-lcd").onchange = () => {
    $("ap-install").setAttribute("manifest", $("ap-lcd").checked ? "firmware/ap-lcd-manifest.json" : "firmware/ap-manifest.json");
  };
  $("tag-fw-info").textContent = info?.tag ? built : "No tag firmware is published with this page; choose a file.";
  if (!info?.tag) $("tag-fw-src").value = "file";
});

// ---------- tag flasher ----------

let flasher = null;
let transport = null;
let backups = null;
let busy = false;
let lastMacSeen = false;
let tagModel = "2.9";

// ---------- model picker: a preview of each tag, drawn by the tag's own code ----------

const PREVIEW = ["Lettmelk 1 L", "24,90", "kr 24,90/l"];

async function drawModelPreviews() {
  for (const btn of document.querySelectorAll("#tag-models .model")) {
    try {
      const r = await loadRenderer(btn.dataset.model);
      const out = r.render(LAYOUT_PRICE, FLAG_PRICE_HIGHLIGHT, PREVIEW);
      if (!out.ok) continue;
      const c = btn.querySelector("canvas");
      const ctx = c.getContext("2d");
      const img = ctx.createImageData(r.width, r.height);
      for (let i = 0; i < r.width * r.height; i++) {
        const v = out.pixels[i] ? 17 : 255;
        img.data.set([v, v, v, 255], i * 4);
      }
      ctx.putImageData(img, 0, 0);
    } catch {
      /* the picker still works without the picture */
    }
  }
}

function selectModel(model) {
  if (busy) return;
  tagModel = model;
  document.querySelectorAll("#tag-models .model").forEach((b) => b.setAttribute("aria-checked", b.dataset.model === model));
}

document.querySelectorAll("#tag-models .model").forEach((b) => b.addEventListener("click", () => selectModel(b.dataset.model)));
drawModelPreviews();

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
  const model = tagModel;
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
    const model = tagModel;
    document.getElementById("tag-models").classList.add("locked");
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
    document.getElementById("tag-models").classList.remove("locked");
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

function backupModel(b) {
  if (b.model) return b.model;
  const mac = Uint8Array.from(b.mac.match(/../g), (x) => parseInt(x, 16));
  return modelOfMac(mac) || "";
}

async function refreshBackups() {
  const body = $("backup-list");
  try {
    backups ||= await BackupStore.open();
    const all = await backups.list();
    body.replaceChildren();
    $("backup-export-zip").disabled = $("backup-export-csv").disabled = !all.length;
    if (!all.length) {
      body.innerHTML = '<tr><td colspan="5" class="muted">None yet.</td></tr>';
      return;
    }
    for (const b of all) {
      const tr = document.createElement("tr");
      const td = (t) => { const c = document.createElement("td"); c.textContent = t; tr.append(c); return c; };
      td(b.mac).className = "mono";
      const m = backupModel(b);
      td(m ? TAG_MODELS[m].name : "?");
      td(new Date(b.time).toLocaleString());
      const inp = document.createElement("input");
      inp.value = b.barcode || "";
      inp.placeholder = "optional";
      inp.setAttribute("aria-label", "Barcode of " + b.mac);
      inp.addEventListener("change", async () => {
        try {
          await backups.setBarcode(b.mac, inp.value.trim());
          inp.classList.remove("bad");
        } catch {
          inp.classList.add("bad");
        }
      });
      // a scanner ends with Enter: save and move on to the next row
      inp.addEventListener("keydown", (ev) => {
        if (ev.key !== "Enter") return;
        inp.dispatchEvent(new Event("change"));
        const next = tr.nextElementSibling?.querySelector("input");
        if (next) next.focus();
      });
      td("").append(inp);
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
    tr.innerHTML = '<td colspan="5" class="warn"></td>';
    tr.firstChild.textContent = "Backups unavailable in this browser: " + e.message + ". Flashing is blocked without them.";
    body.append(tr);
  }
}

const csvCell = (v) => (/[",\n;]/.test(v) ? '"' + v.replace(/"/g, '""') + '"' : v);

function backupCsv(all) {
  const rows = [["mac", "model", "barcode", "stored", "flash_file", "info_file"]];
  for (const b of all) rows.push([b.mac, backupModel(b), b.barcode || "", b.time, b.mac + ".bin", b.mac + ".info.bin"]);
  return rows.map((r) => r.map((v) => csvCell(String(v))).join(",")).join("\r\n") + "\r\n";
}

function saveFile(name, data, type) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([data], { type }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

const today = () => new Date().toISOString().slice(0, 10);

$("backup-export-csv").addEventListener("click", async () => {
  saveFile(`openesl-backups-${today()}.csv`, backupCsv(await backups.list()), "text/csv");
});

$("backup-export-zip").addEventListener("click", async () => {
  const all = await backups.list();
  const files = [{ name: "backups.csv", data: backupCsv(all) }];
  for (const b of all) {
    files.push({ name: b.mac + ".bin", data: b.flash }, { name: b.mac + ".info.bin", data: b.info });
  }
  saveFile(`openesl-backups-${today()}.zip`, makeZip(files), "application/zip");
});

// Ask the browser to keep the backups even when it runs low on space.
(async () => {
  try {
    const kept = (await navigator.storage?.persisted?.()) || (await navigator.storage?.persist?.());
    $("backup-persist").textContent = kept ? "Kept by the browser until you clear the site's data." : "";
  } catch {
    /* not supported */
  }
})();

refreshBackups();
