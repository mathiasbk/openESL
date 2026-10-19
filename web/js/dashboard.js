// dashboard.js - the Dashboard tab: the AP's state, its WiFi and every tag, from GET /api/tags,
// /api/health and /api/wifi. Over the AP's USB port (js/apport.js, shared with the WiFi card)
// or over HTTP through the bridge in tools/esl-client (js/apclient.js). The AP also serves
// this dashboard itself at /dashboard (js/apdash.js): then it calls its own /api/ directly,
// with { sameOrigin: true }.
import { ApClient, apBase } from "./apclient.js";
import { describeWifi } from "./apserial.js";
import { hasSerial, serialAp, connectSerial, disconnectSerial, onSerialChange } from "./apport.js";
import { fmtTagFw, fmtBattery, lastCheckin, nextCheckin, tagRows, healthLine, apFacts } from "./dashdata.js";
import { infoRows, sendRows, textBytes, usesDate, maxDays, TEXT_BYTES, PLACEHOLDERS, PRESETS } from "./sendtext.js";
import { firmwareKind, sendTagFirmware, chooseTagFirmware, sendApFirmware, tagFirmwareRows, tagUpdate, waitApBack, apImageFor, apSendQuestion, cmpVersion } from "./fwupload.js";

const $ = (id) => document.getElementById(id);
const REFRESH_MS = 10000;
const REFRESH_UPDATING_MS = 5000; // while tags fetch firmware, so the bars move

// A bar for a download: fraction 0..1, or null for one that moves to and fro (nothing to count yet)
function progressBar({ fraction, text }) {
  const span = document.createElement("span");
  span.className = "fw-prog";
  const bar = document.createElement("progress");
  bar.max = 1;
  if (fraction != null) bar.value = fraction;
  bar.setAttribute("aria-label", text);
  const t = document.createElement("span");
  t.className = "muted";
  t.textContent = text;
  span.append(bar, t);
  return span;
}

const store = {
  get: (k) => { try { return localStorage.getItem("openesl." + k) || ""; } catch { return ""; } },
  set: (k, v) => { try { localStorage.setItem("openesl." + k, v); } catch { /* private window */ } },
};

export function initDashboard({ sameOrigin = false, firmwareBase = "" } = {}) {
  let client = null;      // anything with call(method, path)
  let timer = null;
  let loading = false;
  let last = null;        // { list, health, wifi, ota, tagfw }
  let sending = false;

  const msg = (text, cls = "") => {
    $("dash-msg").textContent = text;
    $("dash-msg").className = "hint" + (cls ? " " + cls : "");
  };

  const src = () => $("dash-src").value;
  const showSrc = () => {
    $("dash-url-label").hidden = $("dash-token-label").hidden = src() !== "http";
  };
  $("dash-src").value = store.get("dash.src") || (hasSerial() ? "serial" : "http");
  $("dash-url").value = store.get("dash.url");
  if (sameOrigin) {
    // On the AP's own page: its API is right here, only the token is asked for
    $("dash-src").value = "http";
    $("dash-src").closest("label").hidden = true;
    $("dash-token").placeholder = "the AP's API token";
    $("dash-token").value = store.get("dash.token");
  }
  showSrc();
  if (sameOrigin) $("dash-url-label").hidden = true;
  $("dash-src").addEventListener("change", () => {
    store.set("dash.src", src());
    showSrc();
  });
  $("dash-url").addEventListener("change", () => store.set("dash.url", $("dash-url").value.trim()));

  const visible = () => !$("tab-dashboard").hidden && !document.hidden;

  function setConnected(c) {
    client = c;
    $("dash-connect").textContent = c ? "Disconnect" : "Connect";
    $("dash-src").disabled = !!c;
    clearTimeout(timer);
    if (c) refresh();
    else {
      $("dash-body").hidden = true;
      last = null;
    }
  }

  // GET /api/health and /api/wifi are left out quietly when the AP has no such call
  const optional = (p) => client.call("GET", p).catch(() => null);

  async function refresh() {
    clearTimeout(timer);
    if (!client || loading) return;
    loading = true;
    try {
      const c = client;
      const list = await c.call("GET", "/api/tags");
      const health = await optional("/api/health");
      const wifi = await optional("/api/wifi");
      const ota = await optional("/api/ota");
      const tagfw = await optional("/api/tag-firmware");
      if (c !== client) return;
      last = { list, health, wifi, ota, tagfw };
      render();
      msg(`Updated ${new Date().toLocaleTimeString()}.`);
    } catch (e) {
      msg("No answer: " + e.message, "error");
    } finally {
      loading = false;
      const updating = last?.list?.tags?.some((t) => t.fw_update);
      if (client && $("dash-auto").checked) timer = setTimeout(tick, updating ? REFRESH_UPDATING_MS : REFRESH_MS);
    }
  }

  // The timer only reads while the tab is in view
  function tick() {
    if (visible()) refresh();
    else timer = setTimeout(tick, REFRESH_MS);
  }

  function render() {
    if (!last) return;
    const { list, health, wifi } = last;
    renderFirmware();
    $("dash-body").hidden = false;

    $("dash-ap").replaceChildren(...apFacts(list.ap).concat([["Tags", `${list.tags.length} of ${list.ap.max_tags}`]]).map(([k, v]) => {
      const d = document.createElement("div");
      const dt = document.createElement("dt");
      const dd = document.createElement("dd");
      dt.textContent = k;
      dd.textContent = v;
      d.append(dt, dd);
      return d;
    }));
    $("dash-wifi").textContent = wifi ? "WiFi: " + describeWifi(wifi) : "";

    $("dash-health").textContent = healthLine(health);
    $("dash-health").className = !health ? "" : health.ok ? "ok" : "bad";

    const find = $("dash-find").value.trim().toUpperCase();
    const onlyBad = $("dash-problems").checked;
    const rows = tagRows(list, health).filter((r) => (!find || r.tag.mac.includes(find)) && (!onlyBad || r.state.level === "bad"));
    $("dash-count").textContent = rows.length === list.tags.length ? `(${list.tags.length})` : `(${rows.length} of ${list.tags.length})`;
    $("dash-tags").replaceChildren(...rows.map(({ tag, state }) => {
      const tr = document.createElement("tr");
      tr.append(stCheckCell(tag.mac));
      const next = nextCheckin(tag);
      const cells = [
        [tag.mac, "mono"],
        [tag.model ? tag.model + '"' : "–"],
        [lastCheckin(tag)],
        [next ? next.text : "–", next?.late ? "state-bad" : ""],
        [tag.rssi == null ? "–" : `${tag.rssi} dBm`],
        [fmtBattery(tag.battery_mv)],
        [fmtTagFw(tag.fw), "fw"],
        [state.text, "state state-" + state.level],
      ];
      for (const [text, cls] of cells) {
        const td = document.createElement("td");
        td.textContent = text;
        if (cls) td.className = cls;
        tr.append(td);
      }
      // Firmware for it: how far it is, or a button that gives it the page's newer firmware
      const up = tagUpdate(tag, pageImgs, last.tagfw);
      if (up) {
        const td = tr.querySelector("td.fw");
        if (up.updating) {
          td.append(" \u2192 " + up.updating + " ", progressBar(up));
        } else if (up.update || up.choose) {
          const v = up.update || up.choose;
          const b = document.createElement("button");
          b.className = "small fw-up";
          b.textContent = "\u2191"; // arrow up
          b.title = `Update this tag to ${v}` + (up.update ? ": gives the firmware to the AP for this tag only" : "");
          b.setAttribute("aria-label", "Update this tag to " + v);
          b.disabled = sending;
          b.onclick = () => (up.update ? sendPageTagFirmware(tag.model, tag.mac) : chooseTag(tag.model, tag.mac, v));
          td.append(" ", b);
        } else {
          const s = document.createElement("span");
          s.className = "muted";
          s.textContent = " (" + up.text + ")";
          td.append(s);
        }
      }
      return tr;
    }));
    if (!list.tags.length) {
      const tr = document.createElement("tr");
      const td = document.createElement("td");
      td.colSpan = 9;
      td.className = "muted";
      td.textContent = "No tags yet. A tag shows up here after its first poll.";
      tr.append(td);
      $("dash-tags").append(tr);
    }
    renderSendTags();
  }

  // ---- Send info to the tags ticked in the tag table ----

  const stMsg = (text, cls = "") => {
    $("st-msg").textContent = text;
    $("st-msg").className = "hint" + (cls ? " " + cls : "");
  };
  const stLineInputs = [1, 2, 3, 4].map((i) => $("st-l" + i));
  const stWhat = () => $("st-what").value;
  // The lines: ready-made for the list's choices, the inputs for "Your own text"
  const stLines = () => (stWhat() === "text" ? stLineInputs.map((el) => el.value) : PRESETS[stWhat()]);
  const stSel = new Set(store.get("st.macs").split(",").filter(Boolean)); // kept across refreshes
  const stTags = () => (last?.list.tags || []).filter((t) => stSel.has(t.mac));
  const stRows = (tags = stTags()) => infoRows(tags, stLines(), {
    days: Number($("st-days").value) || 1, frame: $("st-frame").checked,
    ap: last?.list.ap, wifi: last?.wifi, total: last?.list.tags.length,
  });
  const stSave = () => {
    store.set("st.macs", [...stSel].join(","));
    store.set("st.what", stWhat());
    store.set("st.lines", JSON.stringify(stLineInputs.map((el) => el.value)));
  };
  if ([...$("st-what").options].some((o) => o.value === store.get("st.what"))) $("st-what").value = store.get("st.what");
  try {
    const saved = JSON.parse(store.get("st.lines") || "null");
    if (Array.isArray(saved)) saved.slice(0, 4).forEach((v, i) => (stLineInputs[i].value = String(v)));
  } catch { /* nothing saved */ }

  // The tick box in the first column of a tag's row
  function stCheckCell(mac) {
    const td = document.createElement("td");
    td.className = "sel";
    const c = document.createElement("input");
    c.type = "checkbox";
    c.checked = stSel.has(mac);
    c.setAttribute("aria-label", "Tick " + mac);
    c.onchange = () => {
      if (c.checked) stSel.add(mac);
      else stSel.delete(mac);
      stChanged();
    };
    td.append(c);
    return td;
  }

  // Buttons that put a placeholder where the cursor was in the last line used
  let stFocus = stLineInputs[0];
  for (const el of stLineInputs) el.addEventListener("focus", () => (stFocus = el));
  $("st-ph").replaceChildren(...PLACEHOLDERS.map(([name, what]) => {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "small";
    b.textContent = `{${name}}`;
    b.title = "Puts in " + what;
    b.onclick = () => {
      const el = stFocus;
      const at = el.selectionStart ?? el.value.length;
      el.setRangeText(`{${name}}`, at, el.selectionEnd ?? at, "end");
      el.focus();
      stChanged();
    };
    return b;
  }));

  // Called after the tag table is drawn
  function renderSendTags() {
    const rows = [...$("dash-tags").querySelectorAll("td.sel input")];
    $("st-all").checked = rows.length > 0 && rows.every((c) => c.checked);
    $("st-all").indeterminate = !$("st-all").checked && rows.some((c) => c.checked);
    stPreview();
  }

  function stChanged() {
    stSave();
    renderSendTags();
  }

  let stSeq = 0;
  async function stPreview() {
    const tags = stTags();
    const lines = stLines();
    $("st-own").hidden = stWhat() !== "text";
    $("st-days-label").hidden = !usesDate(lines);
    $("st-days").max = maxDays(tags.length);
    $("st-send").disabled = !client || !tags.length;
    $("st-send").textContent = !tags.length ? "Send" : tags.length > 1 ? `Send to ${tags.length} tags` : "Send to 1 tag";
    $("st-show").hidden = !tags.length;

    // What each tag gets today
    const rows = stRows(tags).filter((r) => !r.at);
    const most = rows.reduce((n, r) => Math.max(n, textBytes(r.fields)), 0);
    $("st-count").textContent = rows.length ? `${most} of ${TEXT_BYTES} bytes` + (rows.length > 1 ? " (the longest)" : "") : "";
    $("st-count").className = "count" + (most > TEXT_BYTES ? " error" : "");
    $("st-list").replaceChildren(...rows.map((r) => {
      const tr = document.createElement("tr");
      const td1 = document.createElement("td");
      const td2 = document.createElement("td");
      td1.className = "mono";
      td1.textContent = r.mac;
      td2.textContent = r.fields.join(" / ");
      if (textBytes(r.fields) > TEXT_BYTES) td2.className = "error";
      tr.append(td1, td2);
      return tr;
    }));

    // The first ticked tag, drawn by the tag's own code
    const row = rows[0];
    const model = tags[0]?.model || "2.9";
    const seq = ++stSeq;
    if (!row) return ($("st-preview").hidden = true);
    try {
      // On the AP's own page the renderer comes from the published page
      const R = await import(firmwareBase ? new URL("js/renderer.js", firmwareBase).href : "./renderer.js");
      const r = await R.loadRenderer(model, firmwareBase ? new URL("wasm/", firmwareBase) : undefined);
      if (seq !== stSeq) return;
      const out = r.render(R.LAYOUT_TEXT, row.frame ? R.FLAG_FRAME : 0, row.fields);
      if (!out.ok) {
        $("st-preview").hidden = true;
        return;
      }
      const { width: w, height: h } = R.MODELS[model];
      const c = $("st-screen");
      if (c.width !== w || c.height !== h) [c.width, c.height] = [w, h];
      const ctx = c.getContext("2d");
      const img = ctx.createImageData(w, h);
      for (let i = 0; i < w * h; i++) {
        const v = out.pixels[i];
        img.data.set(v === 2 ? [200, 30, 30, 255] : v ? [17, 17, 17, 255] : [255, 255, 255, 255], i * 4);
      }
      ctx.putImageData(img, 0, 0);
      c.title = "Preview: " + row.mac;
      $("st-preview").hidden = false;
    } catch {
      $("st-preview").hidden = true; // no renderer (a page built without it): only the list
    }
  }

  async function sendText(ev) {
    ev.preventDefault();
    const tags = stTags();
    if (!client || !tags.length) return;
    const rows = stRows(tags);
    const over = rows.filter((r) => textBytes(r.fields) > TEXT_BYTES);
    if (over.length) return stMsg(`Too much text for ${over[0].mac}: the lines share ${TEXT_BYTES} bytes.`, "error");
    $("st-send").disabled = true;
    try {
      const { replies, note } = await sendRows(client, rows);
      const now = replies.filter((r) => !r.scheduled);
      const later = replies.length - now.length;
      const waiting = now.filter((r) => r.pending !== false).length;
      const who = tags.length === 1 ? "The tag shows" : `${tags.length} tags show`;
      stMsg((waiting ? `Sent. ${who} it at the next poll.` : "Sent; the tags already show this.") +
        (later ? ` ${later} more ${later === 1 ? "day is" : "days are"} scheduled on the AP for midnight.` : "") +
        (note ? " " + note : ""), "ok");
    } catch (e) {
      stMsg("Not sent: " + e.message, "error");
    } finally {
      $("st-send").disabled = false;
      refresh();
    }
  }

  $("st-form").addEventListener("submit", sendText);
  $("st-form").addEventListener("input", stChanged);
  $("st-form").addEventListener("change", stChanged);
  // The header's tick box: every tag the table shows now (after Find and the problems filter)
  $("st-all").onchange = () => {
    const on = $("st-all").checked;
    for (const c of $("dash-tags").querySelectorAll("td.sel input")) {
      const mac = c.closest("tr").querySelector("td.mono").textContent;
      c.checked = on;
      if (on) stSel.add(mac);
      else stSel.delete(mac);
    }
    stChanged();
  };

  // ---- Firmware card ----

  const fwMsg = (text, cls = "") => {
    $("fw-msg").textContent = text;
    $("fw-msg").className = "hint" + (cls ? " " + cls : "");
  };

  function renderFirmware() {
    const { ota, tagfw } = last;
    const facts = !ota ? [] : [
      ["AP firmware", `${ota.version} (${ota.slot}${ota.confirmed === false ? ", not confirmed yet" : ""})`],
      ["Other slot", ota.other_slot ? `${ota.other_slot}: ${ota.other_version || "empty"}` : "none (old partition table)"],
    ];
    if (ota && typeof ota.display === "boolean") facts.push(["Status screen", ota.display ? "yes" : "no"]);
    $("dash-ota").replaceChildren(...facts.map(([k, v]) => {
      const d = document.createElement("div");
      const dt = document.createElement("dt");
      const dd = document.createElement("dd");
      dt.textContent = k;
      dd.textContent = v;
      d.append(dt, dd);
      return d;
    }));
    // A newer release on this page than the AP runs: a button that sends the image that fits it
    const img = ota && ota.other_slot ? apImageFor(ota, pageInfo) : null;
    if (img?.newer) {
      const dd = $("dash-ota").querySelector("dd");
      const b = document.createElement("button");
      b.className = "small fw-up";
      b.textContent = "\u2191 " + img.version; // arrow up
      b.title = `Update the AP to ${img.version}` + (img.file.includes("lcd") ? " (with the status screen)" : "");
      b.setAttribute("aria-label", "Update the AP to " + img.version);
      b.disabled = sending;
      b.onclick = () => sendPageApFirmware(img);
      dd.append(" ", b);
    }
    const rows = tagFirmwareRows(tagfw);
    $("dash-tagfw").replaceChildren(...rows.map((r) => {
      const tr = document.createElement("tr");
      for (const text of [r.model + '"', r.version ? `${r.version}, for ${r.to}` : "–"]) {
        const td = document.createElement("td");
        td.textContent = text;
        tr.append(td);
      }
      const tags = document.createElement("td");
      tags.textContent = r.text;
      if (r.progress) tags.append(" ", progressBar(r.progress));
      tr.append(tags);
      const td = document.createElement("td");
      const button = (text, title, onclick) => {
        const b = document.createElement("button");
        b.textContent = text;
        b.title = title;
        b.disabled = sending;
        b.onclick = onclick;
        td.append(b, " ");
      };
      // The page has newer firmware for the model than the AP: all its tags at once
      const page = pageImgs[r.model];
      if (page && (!r.version || cmpVersion(page.version, r.version) > 0))
        button("\u2191 " + page.version + " to all", `Give ${r.model}" firmware ${page.version} to the AP for every ${r.model}" tag`, () => sendPageTagFirmware(r.model));
      else if (r.toAll)
        button("To all", `Every ${r.model}" tag gets ${r.version}, not only the chosen ones`, () => chooseTag(r.model, "all", r.version));
      if (r.version) button("Remove", "Stop updating the tags of this model", () => removeTagFirmware(r.model));
      tr.append(td);
      return tr;
    }));
    if (!tagfw) {
      const tr = document.createElement("tr");
      const td = document.createElement("td");
      td.colSpan = 4;
      td.className = "muted";
      td.textContent = "This AP cannot update tags over the radio: it needs firmware with tag updates, flashed once over USB (Flash AP).";
      tr.append(td);
      $("dash-tagfw").append(tr);
    }
  }

  // The choices in "Send": the files published with this page, then a file of your own
  const FILES = [
    ["tag-29-ota", 'Tag firmware for the 2.9"'],
    ["tag-154-ota", 'Tag firmware for the 1.54"'],
    ["ap-openesl-ota", "AP firmware"],
    ["ap-openesl-lcd-ota", "AP firmware with the status screen"],
  ];
  // The tag images published with this page, read once: model -> { data, kind }
  const pageImgs = {};
  const pageData = {};
  let pageInfo = null;    // firmware/info.json
  async function loadPageTagImages(info) {
    for (const k of ["tag-29", "tag-154"]) {
      if (!info?.ota?.[k]) continue;
      try {
        const r = await fetch(`${firmwareBase}firmware/${k}-ota.bin`, { cache: "no-cache" });
        if (!r.ok) continue;
        const data = new Uint8Array(await r.arrayBuffer());
        const kind = firmwareKind(data);
        if (kind.kind !== "tag") continue;
        pageImgs[kind.model] = kind;
        pageData[kind.model] = data;
        // the list names the version in the file itself
        const o = $("fw-src").querySelector(`option[value="${k}-ota"]`);
        if (o) o.textContent = `Tag firmware for the ${kind.model}", ${kind.version}`;
      } catch {
        /* offline: no update buttons */
      }
    }
    if (last) render();
  }

  fetch(`${firmwareBase}firmware/info.json`, { cache: "no-cache" })
    .then((r) => (r.ok ? r.json() : null))
    .catch(() => null)
    .then((info) => {
      pageInfo = info;
      const opts = FILES.filter(([k]) => info?.ota?.[k.replace(/-ota$/, "")]).map(([k, label]) => [k, `${label}, ${info.version}`]);
      opts.push(["file", "A file from this computer …"]);
      $("fw-src").replaceChildren(...opts.map(([v, label]) => {
        const o = document.createElement("option");
        o.value = v;
        o.textContent = label;
        return o;
      }));
      loadPageTagImages(info);
      if (last) renderFirmware();
    });

  async function pageFile(v) {
    const r = await fetch(`${firmwareBase}firmware/${v}.bin`, { cache: "no-cache" });
    if (!r.ok) throw new Error(`could not load ${firmwareBase}firmware/${v}.bin (${r.status})`);
    return new Uint8Array(await r.arrayBuffer());
  }

  async function chosenFile() {
    const v = $("fw-src").value;
    if (v !== "file") return pageFile(v);
    const f = $("fw-file").files[0];
    if (!f) return null;
    return new Uint8Array(await f.arrayBuffer());
  }

  function setSending(on) {
    sending = on;
    $("fw-send").disabled = on;
    $("fw-src").disabled = on;
    $("fw-progress").hidden = !on;
    $("fw-progress").value = 0;
    if (last) renderFirmware();
  }

  async function send() {
    if (!client || sending) return;
    let data;
    try {
      data = await chosenFile();
    } catch (e) {
      return fwMsg("Could not read the firmware: " + e.message, "error");
    }
    if (!data) {
      $("fw-file").click();
      return;
    }
    const v = $("fw-src").value;
    return sendData(data, v === "ap-openesl-lcd-ota" ? true : v === "ap-openesl-ota" ? false : null);
  }

  // The arrow-up button next to the AP's firmware
  async function sendPageApFirmware(img) {
    if (!client || sending) return;
    let data;
    try {
      data = await pageFile(img.file);
    } catch (e) {
      return fwMsg("Could not read the firmware: " + e.message, "error");
    }
    $("fw-msg").scrollIntoView({ block: "nearest" });
    return sendData(data, img.file.includes("lcd"));
  }

  // The arrow-up buttons: in the tag table for one tag (mac), in the firmware table for all
  function sendPageTagFirmware(model, mac = null) {
    if (!client || sending || !pageData[model]) return;
    const v = pageImgs[model].version;
    const onAp = (last?.tagfw?.images || []).find((i) => i.model === model && i.version);
    const q = mac
      ? `Update tag ${mac} to ${model}" firmware ${v}? The AP gets the firmware for this tag only, and the tag updates itself over its next polls.` +
        (onAp ? ` It replaces ${onAp.version} on the AP, so tags still fetching that one stop.` : "")
      : `Give ${model}" firmware ${v} to the AP? Every ${model}" tag that takes firmware over the radio updates itself over its next polls.`;
    if (!confirm(q)) return;
    $("fw-msg").scrollIntoView({ block: "nearest" });
    return sendData(pageData[model], null, mac);
  }

  // The AP has the firmware already: it goes to one more tag, or (to "all") to every one
  async function chooseTag(model, to, version) {
    if (!client || sending) return;
    if (to === "all" && !confirm(`Give ${model}" firmware ${version} to every ${model}" tag, not only the chosen ones?`)) return;
    try {
      await chooseTagFirmware(client, model, to);
      fwMsg(to === "all" ? `Every ${model}" tag gets ${version} over its next polls.` : `Tag ${to} gets ${version} over its next polls.`, "ok");
    } catch (e) {
      fwMsg("Could not choose it: " + e.message, "error");
    }
    refresh();
  }

  // lcd: whether an AP image is the one with the status screen (null: a file of your own);
  // to: the one tag a tag image is for (null: all of its model)
  async function sendData(data, lcd = null, to = null) {
    const kind = firmwareKind(data);
    if (!kind.kind) return fwMsg("Not sent: " + kind.error + ".", "error");
    const what = kind.kind === "tag" ? `tag firmware ${kind.version} for the ${kind.model}"` : `AP firmware${kind.version ? " " + kind.version : ""}`;
    if (kind.kind === "ap" && !confirm(apSendQuestion(kind, last?.ota, lcd))) return;
    setSending(true);
    clearTimeout(timer);
    const progress = (sent, total) => {
      $("fw-progress").value = sent / total;
      fwMsg(`Sending ${what}: ${Math.floor((sent * 100) / total)} % ...`);
    };
    const c = client;
    try {
      if (kind.kind === "tag") {
        const r = await sendTagFirmware(c, data, progress, to);
        fwMsg(to
          ? `The AP now has ${r.model}" firmware ${r.version} for tag ${to}. It takes it over its next polls; the table shows how far it is.`
          : `The AP now offers ${r.model}" firmware ${r.version}. Each tag takes it over its next polls; the table shows how far they are.`, "ok");
      } else {
        const r = await sendApFirmware(c, data, progress);
        fwMsg(`Written to ${r.slot}: version ${r.version}. The AP is restarting ...`);
        const back = await waitApBack(c, r.slot, r.version, { alive: () => client === c });
        fwMsg(back.text, back.ok ? "ok" : "error");
      }
    } catch (e) {
      fwMsg(`Sending ${what} failed: ${e.message}`, "error");
    } finally {
      setSending(false);
      if (client === c) refresh();
    }
  }

  async function removeTagFirmware(model) {
    if (!client || sending || !confirm(`Stop updating the ${model}" tags? The AP removes its firmware for them.`)) return;
    try {
      await client.call("DELETE", "/api/tag-firmware/" + encodeURIComponent(model));
      fwMsg(`No more firmware updates for the ${model}" tags.`, "ok");
    } catch (e) {
      fwMsg("Could not remove it: " + e.message, "error");
    }
    refresh();
  }

  $("fw-send").addEventListener("click", send);
  $("fw-file").addEventListener("change", () => $("fw-file").files[0] && send());
  $("fw-src").addEventListener("change", () => fwMsg(""));

  $("dash-find").addEventListener("input", render);
  $("dash-problems").addEventListener("change", render);
  $("dash-auto").addEventListener("change", () => (client && $("dash-auto").checked ? refresh() : clearTimeout(timer)));
  document.addEventListener("visibilitychange", () => visible() && client && $("dash-auto").checked && refresh());
  window.addEventListener("hashchange", () => visible() && client && refresh());

  // The USB port opened or closed elsewhere (the WiFi card on the Set up tab)
  onSerialChange((ap) => {
    if (src() !== "serial") return;
    if (ap && !client) setConnected(ap);
    else if (!ap && client) {
      setConnected(null);
      msg("Disconnected.");
    }
  });

  $("dash-connect").addEventListener("click", async () => {
    if (client) {
      if (src() === "serial") await disconnectSerial();
      setConnected(null);
      msg("Disconnected.");
      return;
    }
    $("dash-connect").disabled = true;
    try {
      if (src() === "serial") {
        if (!hasSerial()) throw new Error("this browser has no Web Serial. Use Chrome or Edge on a computer, or the bridge.");
        msg("Waiting for the AP ...");
        setConnected(serialAp() || (await connectSerial()));
      } else {
        const token = $("dash-token").value.trim();
        if (sameOrigin) {
          if (!token) throw new Error("enter the AP's API token (set when it was flashed, or on its /setup page).");
          store.set("dash.token", token);  // kept in this browser, for this AP's address only
          setConnected(new ApClient("", token));
          return;
        }
        const { base, error } = apBase($("dash-url").value);
        if (error) throw new Error(error + " Or use USB.");
        setConnected(new ApClient(base, token));
      }
    } catch (e) {
      msg(e.name === "NotFoundError" ? "" : "Could not connect: " + e.message, "error");
    } finally {
      $("dash-connect").disabled = false;
    }
  });

  // The AP's own page with a remembered token: connect at once
  if (sameOrigin && $("dash-token").value) $("dash-connect").click();
}
