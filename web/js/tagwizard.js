// tagwizard.js - the Flash tag tab: a wizard in the style of the Flash AP tab (js/apwizard.js,
// css/apwizard.css). Step 1 picks the tag model and the firmware, step 2 connects the D1 mini
// with the ZBS_Flasher (js/zbs.js), step 3 flashes the tag in the jig with the same job as
// tools/flash-tui (flashTag() in js/tagflash.js: MAC, backup, write, verify), one tag or one after
// the other, and step 4 sums up, with the backups to export.
import * as zp from "./zbs.js";
import { flashTag, checkFirmware, BackupStore, macFromEntry, MODELS as TAG_MODELS } from "./tagflash.js";
import { MODELS } from "./models.js";
import { flasherPicture } from "./appics.js";

const STEPS = ["Choose tag", "Connect flasher", "Flash", "Done"];
const STAGES = [["mac", "MAC"], ["backup", "Backup"], ["write", "Write"], ["verify", "Verify"]];
const FLASHER = "https://github.com/atc1441/ZBS_Flasher";

const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const hasSerial = () => "serial" in navigator;

// info: firmware/info.json (or null); store: kept between visits; onBackup(): a backup was stored
// or changed; exportBackups(): saves the ZIP of all backups
export function initTagWizard(info, store, { onBackup = () => {}, exportBackups = () => {} } = {}) {
  const $ = (id) => document.getElementById(id);
  const body = $("tw-body");
  const names = Object.keys(MODELS);

  const st = {
    step: 0,
    model: names.includes(store.get("tag.model")) ? store.get("tag.model") : names[0],
    fwSrc: info?.tag ? "release" : "file",
    fwFile: null,        // the file picked for "From a file"
    transport: null,
    flasher: null,
    version: "",         // the ZBS_Flasher's
    connectMsg: "",
    busy: false,
    stopping: false,
    loop: store.get("tag.loop") === "1",
    job: null,           // the tag being flashed: { stage, label, frac, waiting }
    askMac: false,       // the last tag has no MAC left: ask for the barcode
    error: "",           // why the last run stopped, other than a tag's own result
    results: [],         // one per tag flashed in this visit, newest first
    log: [],             // [text, class]
  };
  let backups = null;

  // the version of the firmware for a model (tag_versions is keyed by build: "29" is the 2.9")
  const fwVersion = (name) => info?.tag_versions?.[MODELS[name].build] || info?.tag_version || "";
  const fwText = () => (st.fwSrc === "file" ? st.fwFile?.name || "a file" : `OpenESL ${fwVersion(st.model)}`.trim());

  // ---------- rendering ----------

  function stepper() {
    const ok = st.results.filter((r) => r.ok).length;
    const label = (i) => {
      if (i >= st.step || i === 3) return STEPS[i];
      if (i === 0) return `${st.model}" tag`;
      if (i === 1) return "ZBS_Flasher";
      return `${ok} flashed`;
    };
    $("tw-steps").innerHTML = STEPS.map((s, i) => {
      const state = i < st.step || (i === 3 && st.step === 3) ? "done" : i === st.step ? "on" : "";
      return `<li class="${state}"${i === st.step ? ' aria-current="step"' : ""}><span class="wz-dot">${state === "done" || i === 3 ? "✓" : i + 1}</span><span class="wz-label">${esc(label(i))}</span></li>`;
    }).join("");
  }

  function render() {
    stepper();
    body.innerHTML = [choosePage, connectPage, flashPage, donePage][st.step]();
    const back = $("tw-back");
    const next = $("tw-next");
    back.hidden = st.step === 0 || st.step === 3;
    back.disabled = st.busy;
    next.hidden = st.step === 3;
    next.textContent = st.step === 2 ? "Done →" : "Next →";
    next.disabled = st.busy || (st.step === 0 ? !ready() : st.step === 1 ? !st.flasher : st.step === 2 ? !st.results.length : false);
    wire();
  }

  const ready = () => hasSerial() && (st.fwSrc === "release" ? !!info?.tag : !!st.fwFile);

  function seg(name, options, value) {
    return `<div class="wz-seg" role="radiogroup">${options.map(([v, l, off]) =>
      `<button type="button" role="radio" data-seg="${name}" data-value="${esc(v)}" aria-checked="${v === value}"${off ? " disabled" : ""}>${esc(l)}</button>`).join("")}</div>`;
  }

  function choosePage() {
    return `
      <h2>Which tag do you have?</h2>
      ${hasSerial() ? "" : `<p class="warn">Flashing needs Chrome or Edge on a computer (Web Serial).</p>`}
      <div class="wz-cards">${names.map((n) => {
        const m = MODELS[n];
        const pic = m.photo
          ? `<img src="${esc(m.photo.file)}" width="${m.photo.width}" height="${m.photo.height}" alt="Photo of the ${esc(n)}&quot; tag">`
          : `<canvas width="${m.width}" height="${m.height}"></canvas>`;
        const v = fwVersion(n);
        return `
        <button type="button" class="wz-card" data-model="${esc(n)}" aria-pressed="${n === st.model}" ${st.busy ? "disabled" : ""}>
          <span class="wz-pic wz-photo">${pic}</span>
          <b>${esc(n)}" ${esc(m.label.replace(/^\S+ [\d.]+" ?/, ""))}</b>
          <span class="wz-chips"><span class="wz-chip">${esc(m.product)}</span><span class="wz-chip">${m.width} × ${m.height}</span></span>
          ${info?.tag ? `<span class="wz-badge ok">OpenESL${v ? " " + esc(v) : ""}</span>` : ""}
        </button>`;
      }).join("")}
      </div>
      <div class="wz-opts">
        <span class="wz-opt"><span class="muted">Firmware</span>${seg("fw", [["release", info?.tag ? `This page's${fwVersion(st.model) ? " " + fwVersion(st.model) : ""}` : "This page's", !info?.tag], ["file", st.fwFile ? st.fwFile.name : "From a file …"]], st.fwSrc)}</span>
        <input id="tw-file" type="file" accept=".bin" hidden>
      </div>
      ${info?.tag ? "" : `<p class="muted">No tag firmware is published with this page: pick a <code>.bin</code> file.</p>`}`;
  }

  function connectPage() {
    const on = !!st.flasher;
    return `<div class="wz-panel"><div class="wz-part on"><span class="wz-n ${on ? "done" : "on"}">${on ? "✓" : "1"}</span><div class="wz-grow">
      <b>Tag flasher</b> <span class="muted">· a D1 mini with atc1441's <a href="${FLASHER}" target="_blank" rel="noopener">ZBS_Flasher</a>, wired to the jig, on USB</span>
      <div class="wz-row"><span class="wz-flasher">${flasherPicture()}</span><div class="wz-grow">
        ${on ? `<p class="ok">● ZBS_Flasher ${esc(st.version)}</p>
          <p class="buttons tight"><button type="button" class="ghost" id="tw-disconnect">Disconnect</button></p>`
        : `<p class="buttons tight"><button type="button" class="primary" id="tw-connect" ${hasSerial() ? "" : "disabled"}>Connect flasher</button>
          <span class="hint ${st.connectMsg.startsWith("No") ? "error" : ""}" role="status">${esc(st.connectMsg)}</span></p>
          <p class="muted">Pick the D1 mini's port (CH340 or CP210x).</p>`}
      </div></div></div></div></div>
      <p class="wz-hint">ⓘ No answer? The D1 mini must run the ZBS_Flasher, and no other program (such as <code>tools/flash-tui</code>) may have its port.</p>`;
  }

  function stages() {
    const at = STAGES.findIndex(([k]) => k === st.job?.stage);
    return `<ol class="tw-stages">${STAGES.map(([, l], i) =>
      `<li class="${i < at ? "done" : i === at ? "on" : ""}"><span class="wz-n ${i < at ? "done" : i === at ? "on" : ""}">${i < at ? "✓" : i + 1}</span>${esc(l)}</li>`).join("")}</ol>`;
  }

  function jobPart() {
    const head = `<b>Tag in the jig</b> <span class="muted">· ${esc(st.model)}" · ${esc(fwText())}</span>`;
    let inner;
    if (st.busy && st.job?.waiting) {
      inner = `<p class="tw-wait"><span class="tw-spin" aria-hidden="true"></span>${esc(st.job.waiting)}</p>
        <p class="buttons tight"><button type="button" id="tw-stop">${st.stopping ? "Stopping ..." : "Stop"}</button></p>`;
    } else if (st.busy) {
      inner = `${stages()}
        <div class="wz-prog"><span id="tw-plabel">${esc(st.job?.label || "")}</span><span class="muted" id="tw-pct">${st.job?.frac != null ? Math.round(st.job.frac * 100) + " %" : ""}</span></div>
        <progress id="tw-prog" max="1" ${st.job?.frac != null ? `value="${st.job.frac}"` : ""}></progress>
        <p class="buttons tight"><button type="button" id="tw-stop" ${st.loop ? "" : "hidden"}>${st.stopping ? "Stopping after this tag ..." : "Stop after this tag"}</button></p>`;
    } else {
      inner = `<p class="buttons tight"><button type="button" class="primary" id="tw-flash">${st.results.length ? "Flash next tag" : "Flash tag"}</button>
        <label class="tw-loop"><input type="checkbox" id="tw-loop" ${st.loop ? "checked" : ""}> Keep going: flash each next tag I put in</label></p>
        ${st.error ? `<p class="error">${esc(st.error)}</p>` : ""}
        <p class="muted">The tag's own firmware is backed up in this browser before anything is erased.</p>`;
    }
    const ask = st.askMac && !st.busy ? `
      <div class="tw-mac">
        <p class="warn">This tag has no MAC left: not in its info page and not at 0xFC06, where the stock firmware kept it (an earlier build wrote over it). Type the 12 digits under the barcode on its label, or its whole MAC (16 digits). It is written to the info page, so this is needed only once.</p>
        <p class="buttons tight"><input id="tw-mac" class="mono" placeholder="020793500102 or 000002079350341C" autocomplete="off" spellcheck="false">
        <button type="button" class="primary" id="tw-mac-go">Flash with this MAC</button><span class="hint error" id="tw-mac-msg" role="status"></span></p>
      </div>` : "";
    return `<div class="wz-part on"><span class="wz-n on">${st.results.length + 1}</span><div class="wz-grow">${head}${inner}${ask}</div></div>`;
  }

  function resultRows() {
    return st.results.map((r) => `<tr class="${r.ok ? "" : "bad"}">
      <td>${r.ok ? '<span class="ok">✓</span>' : '<span class="error">✗</span>'}</td>
      <td class="mono">${esc(r.mac || "?")}</td><td class="mono">${esc(r.barcode || "")}</td>
      <td>${esc(r.ok ? `${r.message}, ${Math.round(r.seconds)} s` : `${r.step}: ${r.message}`)}</td>
      <td class="muted">${esc({ saved: "backed up", exists: "had a backup", blank: "empty, nothing to back up", failed: "backup failed" }[r.backup] || "")}</td></tr>`).join("");
  }

  function flashPage() {
    return `<div class="wz-panel">${jobPart()}</div>
      ${st.results.length ? `<div class="table-wrap"><table class="tw-results"><thead><tr><th></th><th>MAC</th><th>Barcode</th><th>Result</th><th>Backup</th></tr></thead><tbody>${resultRows()}</tbody></table></div>` : ""}
      <details class="tw-logbox"${st.results.some((r) => !r.ok) ? " open" : ""}><summary>Log</summary><pre class="wz-log" id="tw-log" aria-live="polite">${st.log.map(([t, c]) => `<span class="${c}">${esc(t)}</span>`).join("\n")}</pre></details>`;
  }

  function donePage() {
    const ok = st.results.filter((r) => r.ok).length;
    const bad = st.results.length - ok;
    const saved = st.results.filter((r) => r.backup === "saved").length;
    return `<div class="wz-panel wz-done">
      <div class="wz-big ${ok ? "ok" : "error"}">${ok ? "✓" : "✗"}</div>
      <h2>${ok} ${ok === 1 ? "tag" : "tags"} flashed${bad ? `, ${bad} not` : ""}</h2>
      <p class="muted">${esc(st.model)}" · ${esc(fwText())}${saved ? ` · ${saved} new ${saved === 1 ? "backup" : "backups"} in this browser` : ""}</p>
      ${saved ? `<p>Export the backups and keep them somewhere safe: they are the only copy of the tags' own firmware.</p>` : ""}
      <p class="buttons wz-center">
        ${saved ? `<button type="button" class="primary" id="tw-export">Export backups (ZIP)</button>` : ""}
        <a class="button${saved ? "" : " primary"}" href="#dashboard">Dashboard</a>
        <a class="button" href="#ap">Flash an AP</a>
      </p>
      <p><button type="button" class="ghost" id="tw-more">Flash more tags</button></p>
    </div>`;
  }

  // ---------- actions ----------

  function wire() {
    body.querySelectorAll("[data-model]").forEach((b) => b.addEventListener("click", () => {
      st.model = b.dataset.model;
      store.set("tag.model", st.model);
      render();
    }));
    body.querySelectorAll("[data-seg=fw]").forEach((b) => b.addEventListener("click", () => {
      if (b.dataset.value === "file") $("tw-file").click();
      else {
        st.fwSrc = "release";
        render();
      }
    }));
    $("tw-file")?.addEventListener("change", (ev) => {
      const f = ev.target.files[0];
      if (!f) return;
      st.fwFile = f;
      st.fwSrc = "file";
      render();
    });
    $("tw-connect")?.addEventListener("click", connect);
    $("tw-disconnect")?.addEventListener("click", disconnect);
    $("tw-flash")?.addEventListener("click", () => flashTags());
    $("tw-stop")?.addEventListener("click", () => {
      st.stopping = true;
      render();
    });
    $("tw-loop")?.addEventListener("change", (ev) => {
      st.loop = ev.target.checked;
      store.set("tag.loop", st.loop ? "1" : "");
    });
    $("tw-mac-go")?.addEventListener("click", () => {
      const text = $("tw-mac").value.trim();
      try {
        macFromEntry(text, st.model);
      } catch (e) {
        $("tw-mac-msg").textContent = e.message;
        return;
      }
      flashTags(text);
    });
    $("tw-mac")?.addEventListener("keydown", (ev) => ev.key === "Enter" && $("tw-mac-go").click());
    $("tw-export")?.addEventListener("click", exportBackups);
    $("tw-more")?.addEventListener("click", () => {
      st.step = st.flasher ? 2 : 1;
      render();
    });
  }

  const log = (text, cls = "") => {
    st.log.push([text, cls]);
    const pre = $("tw-log");
    if (!pre) return;
    const line = document.createElement("span");
    line.className = cls;
    line.textContent = text;
    if (pre.childNodes.length) pre.append("\n");
    pre.append(line);
    pre.scrollTop = pre.scrollHeight;
  };

  async function connect() {
    let port;
    try {
      port = await navigator.serial.requestPort();
    } catch {
      return;   // no port picked
    }
    st.connectMsg = "Asking the flasher for its version ...";
    render();
    try {
      st.transport = await zp.WebSerialTransport.open(port);
      st.flasher = new zp.ZbsFlasher(st.transport);
      st.version = await zp.probe(st.flasher);
      st.connectMsg = "";
      log(`ZBS_Flasher version ${st.version}`, "good");
    } catch (e) {
      if (st.transport) await st.transport.close().catch(() => {});
      st.transport = st.flasher = null;
      st.connectMsg = "No answer: " + e.message;
    }
    render();
  }

  async function disconnect() {
    if (st.transport) await st.transport.close().catch(() => {});
    st.transport = st.flasher = null;
    st.version = "";
    log("Disconnected");
    render();
  }

  async function loadFirmware() {
    if (st.fwSrc === "file") return checkFirmware(new Uint8Array(await st.fwFile.arrayBuffer()));
    const r = await fetch(TAG_MODELS[st.model].firmware, { cache: "no-cache" });
    if (!r.ok) throw new Error(`the ${TAG_MODELS[st.model].name} firmware is not published with this page (${r.status}); choose a file`);
    return checkFirmware(new Uint8Array(await r.arrayBuffer()));
  }

  // A tag in the jig answers ZBS_BEGIN
  async function tagThere() {
    try {
      await st.flasher.begin();
      return true;
    } catch (e) {
      if (e instanceof zp.NoTagError) return false;
      throw e;
    }
  }

  // The progress bar and its label without drawing the page again
  function progress(label, done, total) {
    st.job = { ...st.job, label: stageLabel[st.job?.stage] || label, frac: total ? done / total : 0 };
    const bar = $("tw-prog");
    if (!bar) return render();
    bar.value = st.job.frac;
    $("tw-plabel").textContent = st.job.label;
    $("tw-pct").textContent = Math.round(st.job.frac * 100) + " %";
  }

  const stageLabel = { mac: "Reading the MAC ...", backup: "Backing up the tag's own firmware ...", write: "Writing ...", verify: "Verifying ..." };

  async function flashTags(manualMac = "") {
    if (st.busy || !st.flasher) return;
    Object.assign(st, { busy: true, stopping: false, askMac: false, error: "", job: { stage: "mac", label: "Starting ...", frac: null } });
    render();
    let touched = false;
    try {
      backups ||= await BackupStore.open();
      const fw = await loadFirmware();
      let first = true;
      do {
        if (!first) {
          st.job = { waiting: "Take the tag out of the jig ..." };
          render();
          while (!st.stopping && (await tagThere())) await sleep(1000);
          st.job = { waiting: "Put the next tag in the jig ..." };
          render();
          while (!st.stopping && !(await tagThere())) await sleep(1000);
          if (st.stopping) break;
          st.job = { stage: "mac", label: "Starting ...", frac: null };
          render();
        }
        first = false;
        const res = await flashTag(st.flasher, st.model, fw, backups, {
          log: (t) => log(t, t.startsWith("FAILED") ? "err" : t.startsWith("OK") ? "good" : ""),
          progress,
          stage: (s) => {
            st.job = { stage: s, label: stageLabel[s], frac: null };
            render();
          },
          manualMac,
        });
        touched = true;
        // the same tag again with its MAC typed in: its row without one goes
        if (manualMac && st.results[0]?.step === "mac" && !st.results[0].mac) st.results.shift();
        manualMac = "";       // typed for this one tag only
        st.results.unshift(res);
        onBackup();
        if (res.step === "mac" && !res.mac) {
          st.askMac = true;   // the tag stays in the jig for the typed MAC
          break;
        }
        render();
      } while (st.loop && !st.stopping);
    } catch (e) {
      log("Stopped: " + e.message, "err");
      st.error = "Stopped: " + e.message;
    } finally {
      // waiting for the next tag leaves a tag in debug mode; let it run its firmware
      if (st.flasher && touched) await st.flasher.reset().catch(() => {});
      Object.assign(st, { busy: false, stopping: false, job: null });
      render();
      $("tw-mac")?.focus();
    }
  }

  $("tw-next").addEventListener("click", () => {
    st.step++;
    render();
    window.scrollTo(0, 0);
  });
  $("tw-back").addEventListener("click", () => {
    st.step--;
    render();
  });
  // the D1 mini pulled out
  navigator.serial?.addEventListener("disconnect", (ev) => {
    if (!st.transport || ev.target !== st.transport.port) return;
    st.transport = st.flasher = null;
    log("The flasher was unplugged", "err");
    render();
  });
  render();
}
