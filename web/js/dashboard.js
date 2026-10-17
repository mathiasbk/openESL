// dashboard.js - the Dashboard tab: the AP's state, its WiFi and every tag, from GET /api/tags,
// /api/health and /api/wifi. Over the AP's USB port (js/apport.js, shared with the WiFi card)
// or over HTTP through the bridge in tools/esl-client (js/apclient.js).
import { ApClient } from "./apclient.js";
import { describeWifi } from "./apserial.js";
import { hasSerial, serialAp, connectSerial, disconnectSerial, onSerialChange } from "./apport.js";
import { fmtAgo, fmtTagFw, fmtBattery, tagRows, healthLine, apFacts } from "./dashdata.js";
import { firmwareKind, sendTagFirmware, sendApFirmware, tagFirmwareRows, tagUpdate, waitApBack } from "./fwupload.js";

const $ = (id) => document.getElementById(id);
const REFRESH_MS = 10000;

const store = {
  get: (k) => { try { return localStorage.getItem("openesl." + k) || ""; } catch { return ""; } },
  set: (k, v) => { try { localStorage.setItem("openesl." + k, v); } catch { /* private window */ } },
};

export function initDashboard() {
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
  showSrc();
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
      if (client && $("dash-auto").checked) timer = setTimeout(tick, REFRESH_MS);
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
      const cells = [
        [tag.mac, "mono"],
        [tag.model ? tag.model + '"' : "–"],
        [fmtAgo(tag.seen_s_ago)],
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
      // Newer firmware on this page for its model: a button that gives it to the AP
      const up = tagUpdate(tag, pageImgs, last.tagfw);
      if (up) {
        const td = tr.querySelector("td.fw");
        if (up.update) {
          const b = document.createElement("button");
          b.className = "small fw-up";
          b.textContent = "\u2191"; // arrow up
          b.title = `Update to ${up.update}: gives the firmware to the AP; every ${tag.model}" tag takes it at its next polls`;
          b.setAttribute("aria-label", "Update to " + up.update);
          b.disabled = sending;
          b.onclick = () => sendPageTagFirmware(tag.model);
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
      td.colSpan = 7;
      td.className = "muted";
      td.textContent = "No tags yet. A tag shows up here after its first poll.";
      tr.append(td);
      $("dash-tags").append(tr);
    }
  }

  // ---- Firmware card ----

  const fwMsg = (text, cls = "") => {
    $("fw-msg").textContent = text;
    $("fw-msg").className = "hint" + (cls ? " " + cls : "");
  };

  function renderFirmware() {
    const { ota, tagfw } = last;
    $("dash-ota").replaceChildren(...(!ota ? [] : [
      ["AP firmware", `${ota.version} (${ota.slot})`],
      ["Other slot", ota.other_slot ? `${ota.other_slot}: ${ota.other_version || "empty"}` : "none (old partition table)"],
    ]).map(([k, v]) => {
      const d = document.createElement("div");
      const dt = document.createElement("dt");
      const dd = document.createElement("dd");
      dt.textContent = k;
      dd.textContent = v;
      d.append(dt, dd);
      return d;
    }));
    const rows = tagFirmwareRows(tagfw);
    $("dash-tagfw").replaceChildren(...rows.map((r) => {
      const tr = document.createElement("tr");
      for (const text of [r.model + '"', r.version || "–", r.text]) {
        const td = document.createElement("td");
        td.textContent = text;
        tr.append(td);
      }
      const td = document.createElement("td");
      if (r.version) {
        const b = document.createElement("button");
        b.textContent = "Remove";
        b.title = "Stop updating the tags of this model";
        b.disabled = sending;
        b.onclick = () => removeTagFirmware(r.model);
        td.append(b);
      }
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
  async function loadPageTagImages(info) {
    for (const k of ["tag-29", "tag-154"]) {
      if (!info?.ota?.[k]) continue;
      try {
        const r = await fetch(`firmware/${k}-ota.bin`, { cache: "no-cache" });
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

  fetch("firmware/info.json", { cache: "no-cache" })
    .then((r) => (r.ok ? r.json() : null))
    .catch(() => null)
    .then((info) => {
      const opts = FILES.filter(([k]) => info?.ota?.[k.replace(/-ota$/, "")]).map(([k, label]) => [k, `${label}, ${info.version}`]);
      opts.push(["file", "A file from this computer …"]);
      $("fw-src").replaceChildren(...opts.map(([v, label]) => {
        const o = document.createElement("option");
        o.value = v;
        o.textContent = label;
        return o;
      }));
      loadPageTagImages(info);
    });

  async function chosenFile() {
    const v = $("fw-src").value;
    if (v !== "file") {
      const r = await fetch(`firmware/${v}.bin`, { cache: "no-cache" });
      if (!r.ok) throw new Error(`could not load firmware/${v}.bin (${r.status})`);
      return new Uint8Array(await r.arrayBuffer());
    }
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
    return sendData(data);
  }

  // The arrow-up button in the tag table
  function sendPageTagFirmware(model) {
    if (!client || sending || !pageData[model]) return;
    if (!confirm(`Give ${model}" firmware ${pageImgs[model].version} to the AP? Every ${model}" tag that takes firmware over the radio updates itself over its next polls.`)) return;
    $("fw-msg").scrollIntoView({ block: "nearest" });
    return sendData(pageData[model]);
  }

  async function sendData(data) {
    const kind = firmwareKind(data);
    if (!kind.kind) return fwMsg("Not sent: " + kind.error + ".", "error");
    const what = kind.kind === "tag" ? `tag firmware ${kind.version} for the ${kind.model}"` : "AP firmware";
    if (kind.kind === "ap" && !confirm("Send new firmware to the AP? It restarts into it when the upload is done.")) return;
    setSending(true);
    clearTimeout(timer);
    const progress = (sent, total) => {
      $("fw-progress").value = sent / total;
      fwMsg(`Sending ${what}: ${Math.floor((sent * 100) / total)} % ...`);
    };
    const c = client;
    try {
      if (kind.kind === "tag") {
        const r = await sendTagFirmware(c, data, progress);
        fwMsg(`The AP now offers ${r.model}" firmware ${r.version}. Each tag takes it over its next polls; the table shows how far they are.`, "ok");
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
        const url = $("dash-url").value.trim() || "http://localhost:8080";
        setConnected(new ApClient(url, $("dash-token").value.trim()));
      }
    } catch (e) {
      msg(e.name === "NotFoundError" ? "" : "Could not connect: " + e.message, "error");
    } finally {
      $("dash-connect").disabled = false;
    }
  });
}
