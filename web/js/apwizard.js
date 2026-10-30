// apwizard.js - the Flash AP tab: a wizard from "which AP do you have" to the AP on the
// network. Step 1 picks the setup (cards from firmware/info.json, js/apsteps.js), step 2
// flashes each board in it (js/apflash.js, the C6 radio first), step 3 gives the AP its WiFi
// and API token over the same USB port (js/apserial.js through js/apport.js) and, for the radio
// over WiFi, the radio its network and the AP's address (js/radioconsole.js), step 4 shows
// where the AP is and its radio key, read over USB, to be saved (js/radiokey.js).
import { cards, radiosFor, plan, shortChip, isNativeUsb, twoPorts } from "./apsteps.js";
import { setupPicture, devkit } from "./appics.js";
import { loadParts, flashBoard, portAgain } from "./apflash.js";
import { hasSerial, serialAp, serialPort, connectSerial, connectSerialPort, disconnectSerial, onSerialChange } from "./apport.js";
import { checkWifi, checkToken, checkHostname, newToken } from "./apserial.js";
import { setUpRadio } from "./radioconsole.js";
import { readKey, keyPanel } from "./radiokey.js";

const STEPS = ["Choose AP", "Flash", "WiFi & token", "Done"];
const JOIN_WAIT_MS = 30000;
const WEAK_RSSI = -75;   // below this the radio over WiFi loses frames (docs/ap-netradio.md)
const HA_GUIDE = "https://github.com/mathiasbk/openesl/blob/master/docs/home-assistant.md";
const OTHER_TOOLS = "https://github.com/mathiasbk/openesl/blob/master/ap/README.md#build-and-flash";

const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export function initApWizard(info, store) {
  const $ = (id) => document.getElementById(id);
  const body = $("wz-body");
  const version = info?.ap_version || info?.version || "";
  const all = cards(info);

  const st = {
    step: 0,
    card: all.find((c) => c.board.id === store.get("ap.board") && c.ready) || null,
    radio: store.get("ap.radio"),
    parts: null,         // plan(): { flash, connect }
    flash: [],           // per part of step 2: { status, msg, frac, code, port }
    connect: [],         // per part of step 3
    wifi: null,          // { ssid, pass, ip, hostname, token, setup_ssid }
    onWifi: null,        // GET /api/wifi of an AP that is on a network already
    editWifi: false,     // and the user wants to change it
    apPort: null,        // the port the main board was flashed on
    radioPort: null,     // and the radio
    log: [],             // the radio's log lines
    key: null,           // step 4: the AP's radio key, "reading", "none" (no radio security) or "no-usb"
    keySaved: false,     // and the user said it is saved
  };

  const radios = () => (st.card ? radiosFor(st.card.board, info) : []);
  const radio = () => radios().find((r) => r.key === st.radio) || radios()[0] || null;
  const fresh = () => ({ status: "todo", msg: "", frac: 0, code: "" });

  function choose(c) {
    st.card = c;
    store.set("ap.board", c.board.id);
    if (!radios().some((r) => r.key === st.radio)) st.radio = radios()[0]?.key || "";
    render();
  }

  function start() {
    st.parts = plan(st.card.board, radio());
    st.flash = st.parts.flash.map(fresh);
    st.connect = st.parts.connect.map(fresh);
    st.key = null;
    st.keySaved = false;
  }

  const finished = (list) => list.every((p) => p.status === "done" || p.status === "skipped");
  const active = (list) => list.findIndex((p) => p.status !== "done" && p.status !== "skipped");

  // ---------- rendering ----------

  function stepper() {
    const label = (i) => {
      if (i >= st.step || i === 3) return STEPS[i];
      if (i === 0) return st.card.title;
      if (i === 1) return st.flash.every((p) => p.status === "skipped") ? "Skipped" : version ? `Flashed ${version}` : "Flashed";
      return st.wifi?.ssid || STEPS[2];
    };
    $("wz-steps").innerHTML = STEPS.map((s, i) => {
      const state = i < st.step || (i === 3 && st.step === 3) ? "done" : i === st.step ? "on" : "";
      return `<li class="${state}"${i === st.step ? ' aria-current="step"' : ""}><span class="wz-dot">${state === "done" ? "✓" : i === 3 ? "✓" : i + 1}</span><span class="wz-label">${esc(label(i))}</span></li>`;
    }).join("");
  }

  function render() {
    stepper();
    body.innerHTML = [choosePage, flashPage, connectPage, donePage][st.step]();
    const back = $("wz-back");
    const next = $("wz-next");
    back.hidden = st.step === 0 || st.step === 3;
    next.hidden = st.step === 3;
    next.textContent = st.step === 2 ? "Finish →" : "Next →";
    next.disabled = st.step === 0 ? !st.card || !hasSerial() : st.step === 1 ? !finished(st.flash) : st.step === 2 ? !finished(st.connect) : false;
    wire();
    if (st.step === 3) showKey();
  }

  function badge(status) {
    return status === "supported" ? `<span class="wz-badge ok">supported</span>` : `<span class="wz-badge">${esc(status)}</span>`;
  }

  function seg(name, options, value) {
    return `<div class="wz-seg" role="radiogroup">${options.map(([v, l]) =>
      `<button type="button" role="radio" data-seg="${name}" data-value="${esc(v)}" aria-checked="${v === value}">${esc(l)}</button>`).join("")}</div>`;
  }

  function choosePage() {
    const rs = radios();
    return `
      <h2>Which AP do you have?</h2>
      ${hasSerial() ? "" : `<p class="warn">Flashing needs Chrome or Edge on a computer (Web Serial).</p>`}
      <div class="wz-cards">${all.map((c) => `
        <button type="button" class="wz-card" data-card="${esc(c.board.id)}" aria-pressed="${st.card === c}" ${c.ready ? "" : "disabled"}>
          <span class="wz-pic">${setupPicture(c.pic, c.chip)}</span>
          <b>${esc(c.title)}</b>
          <span class="wz-chips">${c.chips.map((x) => `<span class="wz-chip ${esc(x.replace(/ .*/, "").toLowerCase())}">${esc(x)}</span>`).join("")}</span>
          ${c.ready ? badge(c.status) : `<span class="wz-badge">not on this page yet</span>`}
        </button>`).join("")}
      </div>
      ${all.length ? "" : `<p class="warn">No AP firmware is published with this page yet.</p>`}
      ${st.card ? `
      <div class="wz-opts">
        ${rs.length > 1 ? `<span class="wz-opt"><span class="muted">C6 radio</span>${seg("radio", rs.map((r) => [r.key, r.display ? "C6 with screen" : "Plain C6"]), radio()?.key)}</span>` : ""}
      </div>
      <p class="muted">${esc(st.card.board.label || "")}</p>` : ""}`;
  }

  function flashPart(p, s, i, on) {
    const chip = shortChip(p.chip);
    const head = `<span class="wz-chip ${chip.toLowerCase()}">${esc(chip)}</span> <b>${esc(p.title)}</b>`;
    if (s.status === "done") return `<div class="wz-part"><span class="wz-n done">✓</span><div class="wz-grow">${head} <span class="muted">· ${esc(s.msg)}</span>${s.note ? `<p class="warn">${s.note}</p>` : ""}</div></div>`;
    if (s.status === "skipped") return `<div class="wz-part"><span class="wz-n done">–</span><div class="wz-grow">${head} <span class="muted">· skipped</span></div></div>`;
    if (!on) return `<div class="wz-part dim"><span class="wz-n">${i + 1}</span><div class="wz-grow">${head}</div></div>`;
    const where = p.usbPort ? `Plug the ${esc(chip)} into its port marked <b>USB</b>.` : `Plug in the ${p.role === "radio" ? "C6 radio" : esc(chip)}.`;
    const busy = s.status === "busy";
    const pic = p.usbPort ? `<svg class="wz-board" viewBox="0 0 70 100" aria-hidden="true">${devkit(10, 6, chip, true)}</svg>` : "";
    const err = s.status === "error" ? `<p class="error">${esc(s.msg)}</p>${s.code === "different" ? `<p class="buttons tight"><button type="button" data-flash="${i}" data-erase="1">Erase and flash as new</button><span class="muted">Back up first: <code>esl-client backup ap.json</code></span></p>` : ""}` : "";
    return `<div class="wz-part on"><span class="wz-n on">${i + 1}</span><div class="wz-grow">
      ${head} <span class="muted">· ${where}</span>
      <div class="wz-row">${pic}<div class="wz-grow">
        ${busy ? `<div class="wz-prog"><span>${esc(s.msg)}</span><span class="muted">${Math.round(s.frac * 100)} %</span></div><progress max="1" value="${s.frac}"></progress>
          ` : `
          <p class="buttons tight"><button type="button" class="primary" data-flash="${i}" ${hasSerial() ? "" : "disabled"}>Connect and flash</button>
          <button type="button" class="ghost" data-skip="${i}">Skip</button></p>
          ${p.role === "radio" && radio()?.kind === "uart" ? `<p class="muted">The radio is erased first: it keeps nothing.</p>` : `
          <p class="wz-erase"><label><input type="checkbox" data-erase-opt="${i}" ${s.erase ? "checked" : ""}> Erase first (WiFi, token, tags and layouts are lost)</label></p>
          <p class="muted">${p.role === "radio" ? "A C6 that was an AP or a radio keeps its WiFi unless erased." : "An AP that runs OpenESL keeps its data unless erased first."}</p>`}`}
        ${err}
      </div></div></div></div>`;
  }

  function wirePart(s, i, on) {
    if (s.status === "done") return `<div class="wz-part"><span class="wz-n done">✓</span><div class="wz-grow"><b>Wired together</b></div></div>`;
    return `<div class="wz-part ${on ? "on" : "dim"}"><span class="wz-n ${on ? "on" : ""}">${i + 1}</span><div class="wz-grow">
      <b>Wire them together</b> <span class="muted">· TX to RX, RX to TX, GND. 5V only when the C6 has no USB cable.</span>
      ${on ? `<img class="wz-wiring" src="img/ap-s3-c6-wiring.svg" alt="Wiring: S3 GPIO17 to C6 GPIO17, S3 GPIO18 to C6 GPIO16, GND, and 5V only without USB on the C6">
      <p><label><input type="checkbox" data-wired="${i}"> Wired</label></p>` : ""}</div></div>`;
  }

  function flashPage() {
    const a = active(st.flash);
    return `<div class="wz-panel">${st.parts.flash.map((p, i) =>
      p.kind === "wire" ? wirePart(st.flash[i], i, i === a) : flashPart(p, st.flash[i], i, i === a)).join("")}</div>
      <p class="wz-hint">ⓘ Not found? Hold <b>BOOT</b> while you plug it in.</p>`;
  }

  function wifiPart(s, i, on) {
    const chip = shortChip(st.card.board.chip_family);
    const head = `<span class="wz-chip ${chip.toLowerCase()}">${esc(chip)}</span> <b>WiFi and API token</b>`;
    if (s.status === "done") return `<div class="wz-part"><span class="wz-n done">✓</span><div class="wz-grow">${head} <span class="muted">· ${esc(st.wifi.ssid)} · <span class="mono">${esc(st.wifi.ip)}</span></span></div>
      ${st.wifi.token ? `<button type="button" data-copy="${esc(st.wifi.token)}">Copy token</button>` : ""}</div>`;
    if (s.status === "skipped") return `<div class="wz-part"><span class="wz-n done">–</span><div class="wz-grow">${head} <span class="muted">· later, on its setup network</span></div></div>`;
    if (!on) return `<div class="wz-part dim"><span class="wz-n">${i + 1}</span><div class="wz-grow">${head}</div></div>`;
    const ap = serialAp();
    const port = serialPort() || st.apPort;
    const wrongPort = twoPorts(st.card.board) && port && !isNativeUsb(port.getInfo?.());
    const setup = st.wifi?.setup_ssid || "openesl-XXXX";
    const on_ = st.onWifi;
    return `<div class="wz-part on"><span class="wz-n on">${i + 1}</span><div class="wz-grow">${head}
      <div class="wz-split"><div>
      ${ap && on_ && !st.editWifi ? `<p class="ok">● On "${esc(on_.ssid)}" · <span class="mono">${esc(on_.ip)}</span></p>
        <p class="muted">This AP is on the network already and keeps its token.</p>
        <p class="buttons tight"><button type="button" class="primary" id="wz-keep-wifi">Continue</button><button type="button" id="wz-edit-wifi">Change WiFi or token</button></p>` : ap ? `<p class="ok">● Connected over USB</p>
        <form class="form wz-form" id="wz-wifi">
          <label>Network <input id="wz-ssid" list="wz-networks" autocomplete="off" maxlength="32" spellcheck="false" required></label>
          <datalist id="wz-networks"></datalist>
          <label>Password <input id="wz-pass" type="password" autocomplete="off" maxlength="63"${on_ ? ' placeholder="empty: keep the current one"' : ""}></label>
          <label>Name <input id="wz-host" autocomplete="off" maxlength="32" spellcheck="false" placeholder="openesl"></label>
          <label>API token <span class="wz-token"><input id="wz-token" class="mono" autocomplete="off" maxlength="128" spellcheck="false" placeholder="empty: keep the current one"><button type="button" id="wz-gen" title="New random token">⟳</button><button type="button" id="wz-copy">Copy</button></span></label>
          <p class="muted wz-indent">Keep the token: it cannot be read back.</p>
          <p class="buttons tight wz-indent"><button type="submit" class="primary" id="wz-save">Save and join</button><span id="wz-wifi-msg" class="hint" role="status">${esc(s.msg)}</span></p>
        </form>` : `
        ${wrongPort ? `<p class="warn">The ${esc(chip)} answers only on its port marked <b>USB</b>: move the cable there.</p>` : ""}
        <p class="buttons tight"><button type="button" class="primary" id="wz-connect" ${hasSerial() ? "" : "disabled"}>Connect the ${esc(chip)}</button><span class="hint ${s.status === "error" ? "error" : ""}" role="status">${esc(s.msg)}</span></p>`}
      </div>
      ${ap && on_ && !st.editWifi ? "" : `<aside class="wz-aside"><b>No answer?</b><p class="muted">Set WiFi from your phone on the AP's own network:</p><p class="mono" id="wz-setup-ssid">${esc(setup)}</p>
        <p class="muted">then open <span class="mono">192.168.4.1/setup</span></p><button type="button" class="ghost" data-skip-connect="${i}">Skip</button></aside>`}
      </div></div></div>`;
  }

  function joinPart(s, i, on) {
    const head = `<span class="wz-chip c6">C6</span> <b>Radio joins the AP</b>`;
    if (s.status === "done") return `<div class="wz-part"><span class="wz-n done">✓</span><div class="wz-grow">${head} <span class="muted">· ${esc(s.msg)}</span>${s.weak ? `<p class="warn">Weak WiFi at the radio (${s.weak} dBm): it will lose frames. Move it closer to the WiFi access point.</p>` : ""}</div></div>`;
    if (s.status === "skipped") return `<div class="wz-part"><span class="wz-n done">–</span><div class="wz-grow">${head} <span class="muted">· skipped</span></div></div>`;
    if (!on) return `<div class="wz-part dim"><span class="wz-n">${i + 1}</span><div class="wz-grow">${head}</div></div>`;
    const busy = s.status === "busy";
    return `<div class="wz-part on"><span class="wz-n on">${i + 1}</span><div class="wz-grow">${head} <span class="muted">· plug in the C6 radio</span>
      <div class="form wz-form">
        <label>Network <input id="wz-r-ssid" value="${esc(st.wifi?.ssid || "")}" maxlength="32" spellcheck="false"></label>
        <label>Password <input id="wz-r-pass" type="password" value="${esc(st.wifi?.pass || "")}" maxlength="63"></label>
        <label>AP address <input id="wz-r-host" class="mono" value="" placeholder="empty: find it on mDNS" maxlength="63" spellcheck="false"></label>
      </div>
      <p class="muted">Leave the address empty: the radio finds the AP on the network itself, also when its IP changes.${st.wifi?.ip ? ` If it does not, type the AP's address, <span class="mono">${esc(st.wifi.ip)}</span>.` : ""}</p>
      <p class="buttons tight"><button type="button" class="primary" id="wz-join" ${busy || !hasSerial() ? "disabled" : ""}>Connect the C6 and send</button>
        <button type="button" class="ghost" data-skip-connect="${i}">Skip</button><span class="hint ${s.status === "error" ? "error" : ""}" role="status">${esc(s.msg)}</span></p>
      ${st.log.length ? `<pre class="wz-log">${st.log.slice(-8).map(esc).join("\n")}</pre>` : ""}
      </div></div>`;
  }

  function connectPage() {
    const a = active(st.connect);
    return `<div class="wz-panel">${st.parts.connect.map((p, i) =>
      p.kind === "join" ? joinPart(st.connect[i], i, i === a) : wifiPart(st.connect[i], i, i === a)).join("")}</div>`;
  }

  function donePage() {
    const w = st.wifi;
    const name = w?.hostname ? `http://${w.hostname}.local/` : "";
    const where = w?.ip
      ? `<h2>Your AP is on the network</h2><p class="mono wz-addr">${esc(name)}${name ? " · " : ""}${esc(w.ip)}</p>`
      : `<h2>Your AP is flashed</h2><p>Give it WiFi and a token from your phone: join <span class="mono">${esc(w?.setup_ssid || "openesl-XXXX")}</span> and open <span class="mono">192.168.4.1/setup</span>.</p>`;
    return `<div class="wz-panel wz-done">
      <div class="wz-big ok">✓</div>
      ${where}
      <div id="wz-key" class="wz-key"></div>
      <p class="muted">${esc(st.card.title)}${radio() && st.parts.flash.some((p) => p.role === "radio") ? ` · ${esc(radio().display ? "C6 radio with screen" : "C6 radio")}` : ""}${version ? ` · ${esc(version)}` : ""}</p>
      <p class="buttons wz-center">
        ${w?.ip ? `<a class="button primary" href="http://${esc(w.ip)}/dashboard" target="_blank" rel="noopener">Open dashboard</a>` : `<a class="button primary" href="#setup">Set up</a>`}
        <a class="button" href="#tag">Flash a tag</a>
        <a class="button ghost" href="${HA_GUIDE}">Home Assistant guide</a>
      </p>
      <p><button type="button" class="ghost" id="wz-again">Flash another AP</button></p>
    </div>`;
  }

  // The radio key on the done page. The AP makes it the first time it starts (0.9.0), so it is
  // read now, over the USB port of step 3; an AP this page cannot reach shows it on its dashboard.
  function showKey() {
    const box = $("wz-key");
    if (st.key === null) {
      st.key = "reading";
      readWizardKey();
    }
    if (st.keySaved) {
      box.innerHTML = `<p class="ok">✓ You saved the radio key.</p>`;
    } else if (st.key === "no-usb") {
      box.innerHTML = `<p class="warn"><b>Save the AP's radio key.</b> This page could not read it over USB. The AP's dashboard shows it until you say it is saved; it is also under Settings, Backup. Without it, a new or erased AP cannot reach paired tags, and they must be flashed again.</p>`;
    } else if (st.key && st.key !== "reading" && st.key !== "none") {
      box.replaceChildren(keyPanel(st.key, {
        ap: { hostname: st.wifi?.hostname, ip: st.wifi?.ip, setup_ssid: st.wifi?.setup_ssid, board: st.card?.title, fw: version },
        title: "Save the radio key now",
        onSaved: () => {
          st.keySaved = true;
          render();
        },
      }));
    }
  }

  async function readWizardKey() {
    const ap = serialAp();
    try {
      st.key = ap ? (await readKey(ap)) || "none" : "no-usb";
    } catch {
      st.key = "no-usb";
    }
    if (st.step === 3) render();
  }

  // ---------- actions ----------

  function wire() {
    body.querySelectorAll("[data-card]").forEach((b) => b.addEventListener("click", () => choose(all.find((c) => c.board.id === b.dataset.card))));
    body.querySelectorAll("[data-seg]").forEach((b) => b.addEventListener("click", () => {
      st.radio = b.dataset.value;
      store.set("ap.radio", st.radio);
      render();
    }));
    body.querySelectorAll("[data-flash]").forEach((b) => b.addEventListener("click", () => flash(Number(b.dataset.flash), !!b.dataset.erase)));
    body.querySelectorAll("[data-erase-opt]").forEach((b) => b.addEventListener("change", () => {
      st.flash[Number(b.dataset.eraseOpt)].erase = b.checked;
    }));
    body.querySelectorAll("[data-skip]").forEach((b) => b.addEventListener("click", () => {
      st.flash[Number(b.dataset.skip)].status = "skipped";
      render();
    }));
    body.querySelectorAll("[data-wired]").forEach((b) => b.addEventListener("change", () => {
      st.flash[Number(b.dataset.wired)].status = "done";
      render();
    }));
    body.querySelectorAll("[data-skip-connect]").forEach((b) => b.addEventListener("click", () => {
      st.connect[Number(b.dataset.skipConnect)].status = "skipped";
      render();
    }));
    body.querySelectorAll("[data-copy]").forEach((b) => b.addEventListener("click", () => copy(b.dataset.copy, b)));
    $("wz-connect")?.addEventListener("click", connectAp);
    $("wz-keep-wifi")?.addEventListener("click", () => {
      st.connect[active(st.connect)].status = "done";
      render();
    });
    $("wz-edit-wifi")?.addEventListener("click", () => {
      st.editWifi = true;
      render();
    });
    if ($("wz-wifi")) fillWifi();
    $("wz-join")?.addEventListener("click", join);
    $("wz-again")?.addEventListener("click", async () => {
      // the AP just set up may still be open over USB: the next one must not answer as it
      await disconnectSerial().catch(() => {});
      Object.assign(st, { step: 0, wifi: null, onWifi: null, editWifi: false, apPort: null, radioPort: null, key: null, keySaved: false });
      render();
    });
  }

  async function copy(text, btn) {
    try {
      await navigator.clipboard.writeText(text);
      btn.textContent = "Copied";
    } catch {
      prompt("Copy the API token:", text);
    }
  }

  async function flash(i, erase) {
    const p = st.parts.flash[i];
    const s = st.flash[i];
    let port;
    try {
      port = await navigator.serial.requestPort();
    } catch {
      return;   // no port picked
    }
    // a port open on the dashboard, the Set up tab or step 3 of an AP before this one
    if (serialPort() === port) await disconnectSerial().catch(() => {});
    // the radio on a cable is always erased (its partition table is not the AP's); the radio over
    // WiFi has NVS where the AP has it, so a C6 that was an AP keeps its WiFi unless asked to erase
    if (erase) s.erase = true;
    const wipe = !!s.erase || (p.role === "radio" && radio()?.kind === "uart");
    Object.assign(s, { status: "busy", msg: "Loading firmware ...", frac: 0, code: "", note: "" });
    render();
    const phase = { connect: "Connecting ...", keep: "Keeping WiFi, token, tags and layouts ...", erase: "Erasing ...", write: "Writing ...", reset: "Starting ..." };
    try {
      const parts = await loadParts(p.manifest, info?.version);
      const r = await flashBoard(port, {
        chip: p.chip, parts, erase: wipe,
        on: (state, frac) => {
          s.msg = phase[state];
          s.frac = frac;
          render();
        },
      });
      const what = p.role === "radio" ? "flashed" : `${r.kept ? "updated, kept its data" : "flashed new"}${version ? " · " + version : ""}`;
      Object.assign(s, { status: "done", msg: what });
      if (p.role === "radio") st.radioPort = port;
      else {
        st.apPort = port;
        // flashed over the UART bridge (COM): the next step needs the port marked USB
        if (p.usbPort && !isNativeUsb(port.getInfo?.()))
          s.note = `Flashed over the port marked COM. For the next step, move the cable to the ${esc(shortChip(p.chip))}'s port marked <b>USB</b>.`;
      }
    } catch (e) {
      Object.assign(s, { status: "error", msg: "Not flashed: " + (e.message || e), code: e.code || "" });
    }
    render();
  }

  // Step 3: the AP on the port it was just flashed on, if it is still there, else the user picks it
  async function connectAp() {
    const s = st.connect[active(st.connect)];
    s.msg = "Waiting for the AP ...";
    s.status = "todo";
    render();
    try {
      // a port the AP's serial API does not answer on (COM on an S3 or C5) is not tried again
      const usable = st.apPort && !(twoPorts(st.card.board) && !isNativeUsb(st.apPort.getInfo?.()));
      const again = usable && (await portAgain(st.apPort, 3000));
      if (again) await connectSerialPort(again);
      else await connectSerial();
      s.msg = "";
    } catch (e) {
      if (e.name !== "NotFoundError") Object.assign(s, { status: "error", msg: "No answer: " + e.message });
      else s.msg = "";
    }
    render();
  }

  async function fillWifi() {
    const ap = serialAp();
    if (!ap) return;
    const form = $("wz-wifi");
    const msg = (t, cls = "") => {
      $("wz-wifi-msg").textContent = t;
      $("wz-wifi-msg").className = "hint" + (cls ? " " + cls : "");
    };
    $("wz-gen").addEventListener("click", () => ($("wz-token").value = newToken()));
    $("wz-copy").addEventListener("click", (ev) => $("wz-token").value && copy($("wz-token").value, ev.target));
    let current = "";
    try {
      const w = await ap.wifi();
      if (!form.isConnected) return;   // rendered again meanwhile (the port changed)
      st.wifi = { ...st.wifi, setup_ssid: w.setup_ssid, ...(w.connected ? { ssid: w.ssid, ip: w.ip, hostname: w.hostname } : {}) };
      if (w.connected && !st.editWifi && !st.onWifi) {
        st.onWifi = w;
        return render();
      }
      if (w.setup_ssid) $("wz-setup-ssid").textContent = w.setup_ssid;
      $("wz-networks").replaceChildren(...(w.networks || []).map((n) => Object.assign(document.createElement("option"), { value: n.ssid, label: `${n.ssid} (${n.rssi} dBm)` })));
      if (w.ssid) $("wz-ssid").value = w.ssid;
      current = w.hostname || "";
      if (current) $("wz-host").value = current;
      if (w.token_set === false) $("wz-token").value = newToken();
    } catch (e) {
      msg("The AP did not answer: " + e.message, "error");
    }
    form.addEventListener("submit", async (ev) => {
      ev.preventDefault();
      const ssid = $("wz-ssid").value;
      const pass = $("wz-pass").value;
      const token = $("wz-token").value.trim();
      const typed = $("wz-host").value.trim();
      const hostname = typed && typed !== current ? typed : "";
      // on its network already and no new password: only the token and the name change, as an
      // empty password would move the AP to an open network of that name
      const same = st.onWifi && ssid === st.onWifi.ssid && !pass;
      const err = (same ? null : checkWifi(ssid, pass)) || (token ? checkToken(token) : null) || (hostname ? checkHostname(hostname) : null);
      if (err) return msg(err, "error");
      if (same && !token && !hostname) {   // nothing changed
        st.connect[active(st.connect)].status = "done";
        return render();
      }
      $("wz-save").disabled = true;
      msg(same ? "Saving ..." : `Joining "${ssid}" ...`);
      try {
        let w = await ap.setWifi(same ? undefined : ssid, pass, token, hostname);
        const end = Date.now() + JOIN_WAIT_MS;
        while (!w.connected && Date.now() < end) {
          await sleep(2000);
          w = await ap.wifi();
        }
        if (!w.connected) {
          $("wz-save").disabled = false;
          return msg(`Not on "${ssid}" yet. Check the name and password.`, "error");
        }
        st.wifi = { ssid: w.ssid, pass, ip: w.ip, hostname: w.hostname || hostname || current, token, setup_ssid: w.setup_ssid };
        st.connect[active(st.connect)].status = "done";
        render();
      } catch (e) {
        $("wz-save").disabled = false;
        msg("Failed: " + e.message, "error");
      }
    });
  }

  async function join() {
    const s = st.connect[active(st.connect)];
    const cfg = { ssid: $("wz-r-ssid").value, pass: $("wz-r-pass").value, host: $("wz-r-host").value.trim() };
    const err = checkWifi(cfg.ssid, cfg.pass);
    if (err) {
      Object.assign(s, { status: "error", msg: err });
      return render();
    }
    // the radio's port from step 2 if it is plugged in again, else the user picks it
    let port = st.radioPort && (await portAgain(st.radioPort, 1500));
    if (!port) {
      try {
        port = await navigator.serial.requestPort();
      } catch {
        return;
      }
    }
    st.wifi = { ...st.wifi, ssid: cfg.ssid, pass: cfg.pass, ip: st.wifi?.ip || cfg.host };
    st.log = [];
    Object.assign(s, { status: "busy", msg: "Sending ... then the radio restarts and looks for the AP" });
    render();
    try {
      const r = await setUpRadio(port, cfg, {
        onLine: (line) => {
          st.log.push(line);
          const pre = body.querySelector(".wz-log");
          if (pre) pre.textContent = st.log.slice(-8).join("\n");
          else render();
        },
      });
      st.radioPort = r.port;
      if (r.state === "joined") {
        Object.assign(s, { status: "done", msg: `connected to the AP${r.rssi != null ? `, WiFi ${r.rssi} dBm` : ""}`, weak: r.rssi != null && r.rssi < WEAK_RSSI ? r.rssi : 0 });
      } else {
        const why = {
          "no-ap": "The radio is on the network but did not find the AP. Check the AP address.",
          "no-wifi": "The radio has no network: check the name and password.",
        "bad-pass": "The network refused the password. Check it and send again.",
          lost: "The radio did not come back on USB after its restart. Plug it in again and retry.",
          "link-down": "The radio did not reach the AP. Check the AP address.",
        }[r.state] || "The radio did not say it joined the AP. Check the name, password and AP address.";
        Object.assign(s, { status: "error", msg: why });
      }
    } catch (e) {
      Object.assign(s, { status: "error", msg: "Failed: " + e.message });
    }
    render();
  }

  $("wz-next").addEventListener("click", () => {
    if (st.step === 0) start();
    st.step++;
    render();
    window.scrollTo(0, 0);
  });
  $("wz-back").addEventListener("click", () => {
    st.step--;
    render();
  });
  // the AP's port opened or closed, here or on the dashboard or the Set up tab
  onSerialChange(() => st.step === 2 && render());
  $("wz-other").href = OTHER_TOOLS;
  render();
}
