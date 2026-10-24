// main.js - tabs, the tag backups, and the two flasher wizards (js/tagwizard.js, js/apwizard.js).
import { initEditor } from "./editor.js";
import { BackupStore, modelOfMac, MODELS as TAG_MODELS } from "./tagflash.js";
import { initTagWizard } from "./tagwizard.js";
import { makeZip } from "./zip.js";
import { initWifiSetup } from "./wifisetup.js";
import { initApWizard } from "./apwizard.js";
import { initDashboard } from "./dashboard.js";
import { MODELS } from "./models.js";

// The board picked on the Flash AP tab and the like, kept between visits
const store = {
  get: (k) => { try { return localStorage.getItem("openesl." + k) || ""; } catch { return ""; } },
  set: (k, v) => { try { localStorage.setItem("openesl." + k, v); } catch { /* private window */ } },
};

const $ = (id) => document.getElementById(id);

// ---------- tabs ----------

const TABS = ["home", "design", "tag", "ap", "dashboard", "setup", "api"];

function showTab(name) {
  if (!TABS.includes(name)) name = "home";
  document.querySelectorAll(".tabs button").forEach((b) => b.setAttribute("aria-selected", b.dataset.tab === name));
  document.querySelectorAll(".tab").forEach((t) => {
    // a modal dialog left open in a tab that is hidden (Back, a typed #name) would keep the
    // whole page inert behind it, unseen
    if (t.id !== "tab-" + name) t.querySelectorAll("dialog[open]").forEach((d) => d.close());
    t.hidden = t.id !== "tab-" + name;
  });
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
initWifiSetup("wifi");
initDashboard();

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
  // The AP and each tag model carry the version of the release they last changed in; named
  // only when they differ (tag_versions is keyed by build: "29" is the 2.9")
  const nameOf = (build) => Object.entries(MODELS).find(([, m]) => m.build === build)?.[0] ?? build;
  const tagVersions = Object.entries(info?.tag_versions || {}).filter(([, v]) => v);
  const tagText = new Set(tagVersions.map(([, v]) => v)).size > 1
    ? tagVersions.map(([b, v]) => `tag firmware ${v} (${nameOf(b)}")`).join(", ")
    : info?.tag_version ? `tag firmware ${info.tag_version}` : "";
  const allSame = new Set([info?.ap_version, ...tagVersions.map(([, v]) => v)]).size <= 1;
  const parts = info?.ap_version && tagText && !allSame ? ` AP firmware ${info.ap_version}, ${tagText}.` : "";
  $("home-fw").textContent = info ? "Firmware on this page: " + built + parts : "";
  $("ap-fw-info").textContent = info?.ap_boards?.length || info?.ap ? built + (info.ap_version ? ` AP firmware ${info.ap_version}.` : "") : "";
  initApWizard(info, store);
  $("tag-fw-info").textContent = info?.tag ? built + (tagText ? ` ${tagText[0].toUpperCase()}${tagText.slice(1)}.` : "") : "";
  initTagWizard(info, store, { onBackup: refreshBackups, exportBackups: exportZip });
});

// ---------- tag backups (the Flash tag tab's wizard is js/tagwizard.js) ----------

let backups = null;

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
    $("backup-count").textContent = all.length ? String(all.length) : "";
    if (!all.length) {
      body.innerHTML = '<tr><td colspan="5" class="muted">None yet: a tag is backed up when it is flashed.</td></tr>';
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

async function exportZip() {
  backups ||= await BackupStore.open();
  const all = await backups.list();
  const files = [{ name: "backups.csv", data: backupCsv(all) }];
  for (const b of all) {
    files.push({ name: b.mac + ".bin", data: b.flash }, { name: b.mac + ".info.bin", data: b.info });
  }
  saveFile(`openesl-backups-${today()}.zip`, makeZip(files), "application/zip");
}
$("backup-export-zip").addEventListener("click", exportZip);

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
