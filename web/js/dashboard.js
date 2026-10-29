// dashboard.js - the Dashboard tab: the AP's state, its WiFi and every tag, from GET /api/tags,
// /api/health and /api/wifi. Over the AP's USB port (js/apport.js, shared with the WiFi card)
// or over HTTP through the bridge in tools/esl-client (js/apclient.js). The AP also serves
// this dashboard itself at /dashboard (js/apdash.js): then it calls its own /api/ directly,
// with { sameOrigin: true }.
import { ApClient, apBase } from "./apclient.js";
import { describeWifi } from "./apserial.js";
import { hasSerial, serialAp, connectSerial, disconnectSerial, onSerialChange } from "./apport.js";
import { fmtAgo, fmtTagFw, fmtBattery, lastCheckin, tagRows, rowCounts, filterRows, healthLine, apFacts, apIdentity, decodeTagImage, screenKey, screenErrorFinal } from "./dashdata.js";
import { infoRows, sendRows, textBytes, usesDate, maxDays, TEXT_BYTES, PLACEHOLDERS, PRESETS } from "./sendtext.js";
import { FITS, turnedSize, placement, dragTo, tone, quantize, packRaw, picturePlan, sendPictures } from "./sendpic.js";
import { initSources } from "./apsources.js";
import { MODELS as TAG_MODELS } from "./models.js";
import { initSettings } from "./apsettings.js";
import { initLayouts } from "./aplayouts.js";
import { firmwareKind, sendTagFirmware, chooseTagFirmware, sendApFirmware, tagFirmwareRows, tagUpdate, tagFirmwarePlan, waitApBack, apImageFor, apSendQuestion, cmpVersion, radioImageFor, radioSendQuestion, sendRadioFirmware, waitRadioBack } from "./fwupload.js";

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
  const sources = initSources(); // the data sources card (apsources.js)
  let timer = null;
  let loading = false;
  let again = false;      // refresh() was called while one was under way: read again after it
  let last = null;        // { list, health, wifi, ota, tagfw, layouts, config }
  let connectedTo = "";   // "Connected to <where>", before the AP's board
  let lastAt = 0;         // when last was read
  let stale = false;      // the last refresh got no answer: what is shown is from lastAt
  let failed = false;     // the last refresh got no answer, and nothing was read before it
  let sending = false;
  const screens = new Map(); // what the AP draws for each tag (screenCell)
  const screenQueue = [];
  let screenBusy = false;
  const layoutCard = initLayouts({ client: () => client, refresh: () => refresh(), firmwareBase, sameOrigin });

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

  // The Settings page (js/apsettings.js): a new token is used from now on, and kept on the AP's own page
  const settings = initSettings({
    getClient: () => client,
    refresh: () => refresh(),
    overUsb: () => src() === "serial",
    onToken: (token) => {
      if (client instanceof ApClient) client.token = token;
      $("dash-token").value = token;
      if (sameOrigin) store.set("dash.token", token);
    },
  });

  // Connected, the setup text and fields give way to one line: where, and how fresh
  function setConnected(c, where = "") {
    client = c;
    sources.connected(c);
    settings.reset();
    screens.clear();
    screenQueue.length = 0;
    $("dash-connect").textContent = c ? "Disconnect" : "Connect";
    $("dash-src").disabled = !!c;
    $("dash-conn").classList.toggle("connected", !!c);
    $("dash-live").hidden = !c;
    connectedTo = c ? "Connected to " + where : "";
    $("dash-where").textContent = connectedTo;
    clearTimeout(timer);
    layoutCard.reset();
    if (c) refresh();
    else {
      $("dash-body").hidden = true;
      last = null;
      lastAt = 0;
      failed = false;
      setStale(false);
    }
    showLive();
  }

  function showLive() {
    const u = $("dash-updated");
    const noAnswer = stale || failed;
    $("dash-dot").className = "dot" + (noAnswer ? " bad" : lastAt ? "" : " wait");
    u.className = noAnswer ? "error" : "";
    u.textContent = stale ? `no answer since ${new Date(lastAt).toLocaleTimeString()}`
      : failed ? "no answer" : lastAt ? `updated ${fmtAgo(Math.max(0, Math.round((Date.now() - lastAt) / 1000)))} ago` : "connecting …";
    u.title = lastAt ? new Date(lastAt).toLocaleString() : "";
  }
  setInterval(() => client && visible() && showLive(), 1000);

  // The AP stopped answering: the figures stay, greyed, with when they are from, so an
  // "All tags are fine" from before does not look current
  function setStale(on) {
    stale = on;
    $("dash-body").classList.toggle("stale", on);
    showLive();
    if (last) render();
  }

  // GET /api/health and /api/wifi are left out quietly when the AP has no such call
  const optional = (p) => client.call("GET", p).catch(() => null);

  async function refresh() {
    clearTimeout(timer);
    if (!client) return;
    if (loading) {
      again = true;
      return;
    }
    loading = true;
    try {
      const c = client;
      const list = await c.call("GET", "/api/tags");
      const health = await optional("/api/health");
      const wifi = await optional("/api/wifi");
      const ota = await optional("/api/ota");
      const tagfw = await optional("/api/tag-firmware");
      const layouts = await optional("/api/layouts");
      const config = await optional("/api/config");
      if (c !== client) return;
      last = { list, health, wifi, ota, tagfw, layouts, config };
      lastAt = Date.now();
      stale = failed = false;
      $("dash-body").classList.remove("stale");
      render();
      showLive();
      msg("");
      layoutCard.render(last);
    } catch (e) {
      msg(e.message.charAt(0).toUpperCase() + e.message.slice(1), "error");
      if (last) setStale(true); // the line above says since when
      else {
        failed = true;
        showLive();
      }
    } finally {
      loading = false;
      const updating = last?.list?.tags?.some((t) => t.fw_update);
      if (again) {
        // what was read may be from before a change the page just made, e.g. a forgotten tag
        again = false;
        refresh();
      } else if (client && $("dash-auto").checked) timer = setTimeout(tick, updating ? REFRESH_UPDATING_MS : REFRESH_MS);
    }
  }

  // The timer only reads while the tab is in view
  function tick() {
    if (visible()) refresh();
    else timer = setTimeout(tick, REFRESH_MS);
  }

  // The filter of the tag table: "all", "bad", "busy" or "model:<model>", kept in this browser
  let filter = store.get("dash.filter") || "all";
  const setFilter = (f) => {
    filter = f;
    store.set("dash.filter", f);
    render();
  };

  const cell = (text, cls = "", title = "") => {
    const td = document.createElement("td");
    td.setAttribute("role", "cell");
    td.textContent = text;
    if (cls) td.className = cls;
    if (title) td.title = title;
    return td;
  };

  // A button that sets the filter, pressed while it is the one in use
  function filterButton(f, cls, ...content) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = cls + (filter === f ? " on" : "");
    b.setAttribute("aria-pressed", String(filter === f));
    b.dataset.f = f;
    b.append(...content);
    b.onclick = () => setFilter(filter === f && f !== "all" ? "all" : f);
    return b;
  }

  // What has the keyboard focus in the parts render() draws anew, to give it back after:
  // a tile, a filter, or a row's tick box or update button
  function focusKey() {
    const a = document.activeElement;
    if (!a || !$("dash-body").contains(a)) return null;
    if (a.dataset.f) return `[data-f="${a.dataset.f}"]` + (a.classList.contains("tile") ? ".tile" : ".chip");
    const tr = a.closest("#dash-tags tr[data-mac]");
    if (tr) return `tr[data-mac="${tr.dataset.mac}"] ` + (a.matches("td.sel input") ? "td.sel input" : "button.fw-up");
    return null;
  }

  function render() {
    if (!last) return;
    const focus = focusKey();
    const { list, health, wifi } = last;
    renderFirmware();
    $("dash-body").hidden = false;

    const ap = list.ap;
    const who = apIdentity(ap, last.ota, wifi);
    $("dash-where").textContent = connectedTo + (who ? " · " + who : "");
    $("dash-ap-sum").textContent = [ap.fw, "up " + fmtAgo(ap.uptime_s), "channel " + ap.channel]
      .concat(wifi?.connected && wifi.ssid ? [`WiFi "${wifi.ssid}"`] : []).join(" · ");
    $("dash-ap").replaceChildren(...apFacts(ap, last.ota).concat([["Tags", `${list.tags.length} of ${ap.max_tags}` + (ap.table_full ? ", full: no room for a new tag" : "")]]).map(([k, v]) => {
      const d = document.createElement("div");
      const dt = document.createElement("dt");
      const dd = document.createElement("dd");
      dt.textContent = k;
      dd.textContent = v;
      d.append(dt, dd);
      return d;
    }));
    $("dash-wifi").textContent = wifi ? "WiFi: " + describeWifi(wifi) : "";
    settings.render(last);

    const health_ = stale
      ? [`The figures below are from ${new Date(lastAt).toLocaleTimeString()}, the AP's last answer.`, "bad"]
      : [healthLine(health), !health ? "" : health.ok ? "ok" : "bad"];
    // only a change is read out (aria-live)
    if ($("dash-health").textContent !== health_[0]) $("dash-health").textContent = health_[0];
    $("dash-health").className = health_[1];

    const all = tagRows(list, health);
    const counts = rowCounts(all);
    const models = Object.keys(counts.models);
    if (!["all", "bad", "busy"].includes(filter) && !models.some((m) => filter === "model:" + m)) filter = "all";

    // The figures at the top: each picks what the table shows
    const tile = (f, n, label, cls) => {
      const big = document.createElement("span");
      big.className = "n";
      big.textContent = n;
      const small = document.createElement("span");
      small.className = "l";
      small.textContent = label;
      if (f) return filterButton(f, "tile " + cls, big, small);
      const d = document.createElement("div");
      d.className = "tile " + cls;
      d.append(big, small);
      return d;
    };
    const tiles = [
      tile("all", list.tags.length, ap.table_full ? "tags, full: no room for more" : `tags, of ${ap.max_tags}`,
           ap.table_full ? "bad" : ""),
      tile("bad", counts.bad, counts.bad === 1 ? "needs attention" : "need attention", counts.bad ? "bad" : "good"),
      tile("busy", counts.busy, "busy or late", counts.busy ? "busy" : ""),
    ];
    if (ap.max_scheduled) tiles.push(tile(null, ap.scheduled, "scheduled changes", ""));
    $("dash-tiles").replaceChildren(...tiles);

    const chip = (f, label, n) => filterButton(f, "chip", `${label} `, Object.assign(document.createElement("span"), { className: "n", textContent: n }));
    $("dash-filter").replaceChildren(
      chip("all", "All", counts.all), chip("bad", "Need attention", counts.bad), chip("busy", "Busy or late", counts.busy),
      ...(models.length > 1 ? models.map((m) => chip("model:" + m, m + '"', counts.models[m])) : []));

    const rows = filterRows(all, filter, $("dash-find").value);
    $("dash-count").textContent = rows.length === list.tags.length ? `(${list.tags.length})` : `(${rows.length} of ${list.tags.length})`;
    $("dash-tags").replaceChildren(...rows.map(({ tag, state, next }) => {
      const tr = document.createElement("tr");
      tr.setAttribute("role", "row");
      tr.dataset.mac = tag.mac;
      tr.className = "lvl-" + state.level;
      tr.append(stCheckCell(tag.mac));
      const st = cell("", "state");
      const dot = document.createElement("span");
      dot.className = "st state-" + state.level;
      dot.textContent = state.text;
      st.append(dot);
      // the MAC: the last six digits tell the tags apart
      const mac = cell("", "mono mac", tag.mac);
      const pre = document.createElement("span");
      pre.className = "pre";
      pre.textContent = tag.mac.slice(0, -6);
      const b = document.createElement("b");
      b.textContent = tag.mac.slice(-6);
      mac.append(pre, b);
      const seen = lastCheckin(tag);
      const lateCls = next?.late ? (state.level === "bad" ? "state-bad" : "state-busy") : "";
      const battery = fmtBattery(tag.battery_mv);
      const fw = cell(fmtTagFw(tag.fw), "opt fw");
      tr.append(st, mac,
        cell(tag.model ? tag.model + '"' : "–", "opt"),
        cell(seen.text, "opt", seen.at),
        cell(next ? next.text : "–", "opt " + lateCls, next ? "Expected by " + next.at : ""),
        cell(tag.rssi == null ? "–" : `${tag.rssi} dBm`, "opt num"),
        cell(battery, "opt num", tag.battery_mv === 0 ? "This model cannot measure its battery" : ""),
        fw);
      // Firmware for it: how far it is, or a button that gives it the page's newer firmware
      const up = tagUpdate(tag, pageImgs, last.tagfw);
      let fwText = fmtTagFw(tag.fw);
      if (up) {
        if (up.updating) {
          fw.append(" \u2192 " + up.updating + " ", progressBar(up));
          fwText += ` \u2192 ${up.updating} (${up.text})`;
        } else if (up.update || up.choose) {
          const v = up.update || up.choose;
          const b = document.createElement("button");
          b.className = "small fw-up";
          b.textContent = "\u2191"; // arrow up
          b.title = `Update this tag to ${v}` + (up.update ? ": gives the firmware to the AP for this tag only" : "");
          b.setAttribute("aria-label", "Update this tag to " + v);
          b.disabled = sending;
          b.onclick = () => (up.update ? sendPageTagFirmware(tag.model, tag.mac) : chooseTag(tag.model, tag.mac, v));
          fw.append(" ", b);
        } else {
          const s = document.createElement("span");
          s.className = "muted";
          s.textContent = " (" + up.text + ")";
          fw.append(s);
        }
      }
      // On a phone the columns after the MAC become one line under it
      const meta = [tag.model ? tag.model + '"' : null, tag.seen_s_ago == null ? "never seen" : "seen " + seen.text,
        next && !next.late ? "next " + next.text : null, tag.rssi == null ? null : `${tag.rssi} dBm`,
        battery === "–" ? null : battery, tag.fw == null ? null : fwText].filter(Boolean).join(" · ");
      tr.append(screenCell(tag), cell(meta, "meta"));
      return tr;
    }));
    if (!rows.length) {
      const tr = document.createElement("tr");
      tr.setAttribute("role", "row");
      const td = cell(list.tags.length ? "No tags match. " : "No tags yet. A tag shows up here after its first poll.", "muted empty");
      td.colSpan = 10;
      if (list.tags.length) {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "small";
        b.textContent = "Show all";
        b.onclick = () => {
          $("dash-find").value = "";
          setFilter("all");
          $("dash-find").focus();
        };
        td.append(b);
      }
      tr.append(td);
      $("dash-tags").append(tr);
    }
    renderSendTags();
    if (focus) document.querySelector(focus)?.focus({ preventScroll: true });
  }

  // ---- The screen the AP draws for each tag (GET /api/tags/{mac}/image) ----

  // screens: mac -> { want, key, canvas, retry_ms }: the screenKey() last fetched, and
  // the key of the canvas, null when the AP draws nothing for it (no content, content
  // held back, or an AP from before images). Fetched one at a time when screenKey()
  // changes, so a table of many tags costs the AP one drawing per change, and painted
  // once: render() moves the same canvas into the new row.
  // retry_ms: a failure that may pass (screenErrorFinal) is tried again after that time.
  const SCREEN_RETRY_MS = 15000;

  function paintScreen(img, canvas = document.createElement("canvas")) {
    if (canvas.width !== img.width || canvas.height !== img.height) [canvas.width, canvas.height] = [img.width, img.height];
    const ctx = canvas.getContext("2d");
    const data = ctx.createImageData(img.width, img.height);
    const px = new Uint32Array(data.data.buffer);
    const le = new Uint8Array(new Uint32Array([1]).buffer)[0] === 1;
    const rgba = (r, g, b) => (le ? (0xff << 24) | (b << 16) | (g << 8) | r : (r << 24) | (g << 16) | (b << 8) | 0xff) >>> 0;
    const colours = [rgba(255, 255, 255), rgba(17, 17, 17), rgba(200, 30, 30)];
    for (let i = 0; i < img.pixels.length; i++) px[i] = colours[img.pixels[i]];
    ctx.putImageData(data, 0, 0);
    canvas.setAttribute("role", "img");
    return canvas;
  }

  function screenCell(tag) {
    const td = cell("", "opt screen");
    if (!tag.content_id) {
      td.textContent = "–";
      return td;
    }
    const key = screenKey(tag, last?.layouts);
    const s = screens.get(tag.mac);
    if ((!s || s.want !== key || (s.retry_ms && Date.now() >= s.retry_ms)) && !screenQueue.some((q) => q.mac === tag.mac)) {
      screenQueue.push({ mac: tag.mac, key });
      fetchScreens();
    }
    if (!s?.canvas) {
      if (s && !s.retry_ms) td.textContent = "–";
      return td;
    }
    const c = s.canvas;
    const waiting = tag.pending || s.key !== key;
    c.className = "screen" + (waiting ? " waiting" : "");
    c.title = waiting ? "What the AP draws for it; the tag does not show it yet" : "What the tag shows, as the AP draws it";
    c.setAttribute("aria-label", c.title);
    td.append(c);
    return td;
  }

  async function fetchScreens() {
    if (screenBusy) return;
    screenBusy = true;
    let changed = false;
    try {
      while (screenQueue.length && client) {
        const { mac, key } = screenQueue.shift();
        const c = client;
        const old = screens.get(mac);
        let s;
        try {
          const img = decodeTagImage(await c.call("GET", `/api/tags/${encodeURIComponent(mac)}/image`));
          s = { want: key, key, canvas: img ? paintScreen(img) : null, retry_ms: 0 };
        } catch (e) {
          // a final answer stands until the key changes; otherwise keep showing what we had
          s = screenErrorFinal(e) ? { want: key, key, canvas: null, retry_ms: 0 }
            : { want: key, key: old?.key, canvas: old?.canvas ?? null, retry_ms: Date.now() + SCREEN_RETRY_MS };
        }
        if (c !== client) break;
        screens.set(mac, s);
        changed = true;
      }
    } finally {
      screenBusy = false;
      if (changed) render();
    }
  }

  // ---- Send info to the tags ticked in the tag table ----

  const stMsg = (text, cls = "") => {
    $("st-msg").textContent = text;
    $("st-msg").className = "hint" + (cls ? " " + cls : "");
  };
  const stLineInputs = [1, 2, 3, 4].map((i) => $("st-l" + i));
  const stWhat = () => $("st-what").value;
  // The lines: ready-made for the list's choices, the inputs for "Your own text"
  const stLines = () => (stWhat() === "text" ? stLineInputs.map((el) => el.value) : PRESETS[stWhat()] || PRESETS.clear);
  const stFw = () => stWhat() === "fw";
  const stPic = () => stWhat() === "pic";
  // the picture for "A picture" (js/sendpic.js), turned and placed as the previews show it
  let pic = null;             // { bitmap, name }
  let picX = 0.5, picY = 0.5, picTurn = 0;
  const picViews = new Map(); // model -> { fig, canvas, cap, place }: the previews, kept while dragging
  const picSettings = () => ({
    fit: $("st-fit").value, dither: $("st-dither").checked,
    brightness: Number($("st-bright").value), contrast: Number($("st-contrast").value),
  });
  const stWeather = () => stWhat() === "weather"; // apsources.js: a dialog chooses the place
  let stBusy = false; // text going to the ticked tags
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
    store.set("st.pic", JSON.stringify(picSettings()));
  };
  if ([...$("st-what").options].some((o) => !o.disabled && o.value === store.get("st.what"))) $("st-what").value = store.get("st.what");
  try {
    const saved = JSON.parse(store.get("st.lines") || "null");
    if (Array.isArray(saved)) saved.slice(0, 4).forEach((v, i) => (stLineInputs[i].value = String(v)));
  } catch { /* nothing saved */ }

  // The tick box in the first column of a tag's row
  function stCheckCell(mac) {
    const td = document.createElement("td");
    td.setAttribute("role", "cell");
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
    // Ticked tags bring up the bar and the form; none, only a hint
    const n = stTags().length;
    $("st-bar").hidden = $("st-form").hidden = !n;
    $("st-hint").hidden = !!n;
    $("st-forget").disabled = !client || sending || stBusy;
    const rows = [...$("dash-tags").querySelectorAll("td.sel input")];
    // the text goes to every ticked tag, also those Find or a filter hides now
    const hidden = stHidden(n, rows);
    $("st-sel-count").replaceChildren(n === 1 ? "1 tag ticked" : `${n} tags ticked`,
      ...(hidden > 0 ? [Object.assign(document.createElement("span"), { className: "hidden-n", textContent: ` (${hidden} not shown)` })] : []));
    $("st-all").checked = rows.length > 0 && rows.every((c) => c.checked);
    $("st-all").indeterminate = !$("st-all").checked && rows.some((c) => c.checked);
    stPreview();
  }

  // How many of the n ticked tags Find or a filter hides now
  function stHidden(n = stTags().length, rows = [...$("dash-tags").querySelectorAll("td.sel input")]) {
    return n - rows.filter((c) => c.checked).length;
  }

  function stChanged() {
    $("st-forget-msg").hidden = true; // said about the tags ticked before
    stSave();
    renderSendTags();
  }

  let stSeq = 0;
  async function stPreview() {
    const tags = stTags();
    const lines = stLines();
    $("st-own").hidden = stWhat() !== "text";
    $("st-days-label").hidden = stFw() || stPic() || stWeather() || !usesDate(lines);
    $("st-frame-label").hidden = stFw() || stPic() || stWeather();
    $("st-days").max = maxDays(tags.length);
    $("st-send").disabled = !client || !tags.length || sending || stBusy;
    $("st-send").textContent = !tags.length ? "Send" : tags.length > 1 ? `Send to ${tags.length} tags` : "Send to 1 tag";
    $("st-show").hidden = !tags.length;
    const listRow = (mac, text, cls = "") => {
      const tr = document.createElement("tr");
      const td1 = document.createElement("td");
      const td2 = document.createElement("td");
      td1.className = "mono";
      td1.textContent = mac;
      td2.textContent = text;
      if (cls) td2.className = cls;
      tr.append(td1, td2);
      return tr;
    };

    // The weather: the place each tag shows now; the dialog chooses the new one
    if (stWeather()) {
      ++stSeq;
      $("st-preview").hidden = true;
      const where = sources.placeOf();
      $("st-count").textContent = "";
      $("st-list").replaceChildren(...tags.map((t) => listRow(t.mac, where.has(t.mac) ? "The weather for " + where.get(t.mac) : "No weather yet", where.has(t.mac) ? "" : "muted")));
      if (tags.length) $("st-send").textContent = "Choose the place …";
      return;
    }

    // Firmware: what each tag gets, or why it gets nothing
    if (stFw()) {
      ++stSeq;
      $("st-preview").hidden = true;
      const { rows } = tagFirmwarePlan(tags, pageImgs, last?.tagfw);
      const n = rows.filter((r) => r.action).length;
      $("st-count").textContent = `${n} of ${tags.length} ${tags.length === 1 ? "tag" : "tags"} to update`;
      $("st-count").className = "count";
      $("st-list").replaceChildren(...rows.map((r) => listRow(r.mac, r.text, r.action ? "" : "muted")));
      if (tags.length) $("st-send").textContent = n === 1 ? "Update 1 tag" : `Update ${n} tags`;
      if (!n) $("st-send").disabled = true;
      return;
    }

    // A picture: which ticked tags can take one; the dialog chooses it (picOpen)
    if (stPic()) {
      ++stSeq;
      $("st-preview").hidden = true;
      const plan = picturePlan(tags, TAG_MODELS);
      const n = plan.filter((p) => p.ok).length;
      $("st-count").textContent = `${n} of ${tags.length} ${tags.length === 1 ? "tag takes" : "tags take"} a picture`;
      $("st-count").className = "count";
      $("st-list").replaceChildren(...plan.map((p) => listRow(p.mac, p.ok ? p.text.replace("the picture", "a picture") : p.text, p.ok ? "" : "muted")));
      if (tags.length) $("st-send").textContent = "Choose the picture …";
      if (!n) $("st-send").disabled = true;
      if ($("pic-dlg").open) picDialog(plan);
      return;
    }

    // What each tag gets today
    const rows = stRows(tags).filter((r) => !r.at);
    const most = rows.reduce((n, r) => Math.max(n, textBytes(r.fields)), 0);
    $("st-count").textContent = rows.length ? `${most} of ${TEXT_BYTES} bytes` + (rows.length > 1 ? " (the longest)" : "") : "";
    $("st-count").className = "count" + (most > TEXT_BYTES ? " error" : "");
    const where = sources.placeOf();
    $("st-list").replaceChildren(...rows.map((r) => listRow(r.mac, r.fields.join(" / ") +
      (where.has(r.mac) ? ` (instead of the weather for ${where.get(r.mac)})` : ""), textBytes(r.fields) > TEXT_BYTES ? "error" : "")));

    // The first ticked tag, drawn by the AP's own code
    const row = rows[0];
    const model = tags[0]?.model || Object.keys(TAG_MODELS)[0];
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

  // The page's firmware to the ticked tags: the image to the AP for the first tag of a model
  // that needs it, then the AP gives it to the others too
  async function sendTagFw(tags) {
    const { rows, upload } = tagFirmwarePlan(tags, pageImgs, last?.tagfw);
    const todo = rows.filter((r) => r.action);
    if (!todo.length) return stMsg("None of the ticked tags has anything to update.", "error");
    const replaced = upload.map((m) => (last?.tagfw?.images || []).find((i) => i.model === m && i.version)).filter(Boolean);
    if (!confirm(`Update ${todo.length === 1 ? "1 tag" : todo.length + " tags"} to the firmware on this page? ` +
      "Each fetches it over its next polls, keeps showing its content, and restarts into it." +
      replaced.map((i) => ` The AP's ${i.model}" firmware ${i.version} is replaced, so tags still fetching it stop.`).join(""))) return;
    const c = client;
    sending = true;
    clearTimeout(timer);
    renderFirmware();
    stPreview();
    try {
      for (const model of upload) {
        const first = todo.find((r) => r.model === model && r.action === "upload");
        const v = pageImgs[model].version;
        await sendTagFirmware(c, pageData[model], (sent, total) =>
          stMsg(`Sending ${model}" firmware ${v} to the AP: ${Math.floor((sent * 100) / total)} % ...`), first.mac);
        first.done = true;
      }
      for (const r of todo) {
        if (r.done) continue;
        await chooseTagFirmware(c, r.model, r.mac);
        r.done = true;
      }
      stMsg(`${todo.length === 1 ? "The tag gets" : todo.length + " tags get"} the new firmware over the next polls; the table shows how far ${todo.length === 1 ? "it is" : "they are"}.`, "ok");
    } catch (e) {
      const n = todo.filter((r) => r.done).length;
      stMsg((n ? `Only ${n} of ${todo.length} tags get it: ` : "Not sent: ") + e.message, "error");
    } finally {
      sending = false;
      if (client === c) refresh();
    }
  }

  async function sendText(ev) {
    ev.preventDefault();
    const tags = stTags();
    if (!client || !tags.length || sending || stBusy) return;
    if (stFw()) return sendTagFw(tags);
    if (stPic()) return picOpen();
    if (stWeather()) {
      return sources.open(tags, (text, err) => {
        stMsg(err ? "Not done: " + err.message : text, err ? "error" : "ok");
        refresh();
      });
    }
    const rows = stRows(tags);
    const over = rows.filter((r) => textBytes(r.fields) > TEXT_BYTES);
    if (over.length) return stMsg(`Too much text for ${over[0].mac}: the lines share ${TEXT_BYTES} bytes.`, "error");
    stBusy = true;
    $("st-send").disabled = $("st-forget").disabled = true;
    try {
      // the AP takes a tag that shows the weather out of its place, so the forecast leaves this be
      const where = sources.placeOf();
      const left = [...new Set(tags.filter((t) => where.has(t.mac)).map((t) => where.get(t.mac)))];
      const { replies, note } = await sendRows(client, rows);
      if (left.length) sources.reload();
      const now = replies.filter((r) => !r.scheduled);
      const later = replies.length - now.length;
      const waiting = now.filter((r) => r.pending !== false).length;
      const who = tags.length === 1 ? "The tag shows" : `${tags.length} tags show`;
      stMsg((waiting ? `Sent. ${who} it at the next poll.` : "Sent; the tags already show this.") +
        (later ? ` ${later} more ${later === 1 ? "day is" : "days are"} scheduled on the AP for midnight.` : "") +
        (note ? " " + note : "") +
        (left.length ? ` No more weather for ${left.length === 1 && tags.length === 1 ? "it" : "them"} (${left.join(", ")}).` : ""), "ok");
    } catch (e) {
      stMsg("Not sent: " + e.message, "error");
    } finally {
      stBusy = false;
      $("st-send").disabled = false;
      $("st-forget").disabled = !client;
      refresh();
    }
  }

  // ---- A picture to the ticked tags (js/sendpic.js) ----

  try {
    const p = JSON.parse(store.get("st.pic") || "null");
    if (p && FITS.includes(p.fit)) $("st-fit").value = p.fit;
    if (p && typeof p.dither === "boolean") $("st-dither").checked = p.dither;
    if (p && Number.isFinite(p.brightness)) $("st-bright").value = p.brightness;
    if (p && Number.isFinite(p.contrast)) $("st-contrast").value = p.contrast;
  } catch { /* nothing saved */ }

  // The picture on a model's screen: { w, h, red, place, pixels } (pixel values as sendpic.js)
  function picScreen(model) {
    const { width: w, height: h, colors } = TAG_MODELS[model];
    const c = document.createElement("canvas");
    [c.width, c.height] = [w, h];
    const ctx = c.getContext("2d", { willReadFrequently: true });
    const b = pic.bitmap; // drawn on a transparent canvas: tone() makes the rest white
    const [iw, ih] = turnedSize(b.width, b.height, picTurn);
    const set = picSettings();
    const place = placement(iw, ih, w, h, { fit: set.fit, zoom: Number($("st-zoom").value), x: picX, y: picY });
    ctx.imageSmoothingQuality = "high";
    ctx.translate(place.dx + place.dw / 2, place.dy + place.dh / 2);
    ctx.rotate((picTurn * Math.PI) / 180);
    const [rw, rh] = picTurn % 180 ? [place.dh, place.dw] : [place.dw, place.dh];
    ctx.drawImage(b, -rw / 2, -rh / 2, rw, rh);
    const data = tone(ctx.getImageData(0, 0, w, h).data, set);
    const red = colors.includes("red");
    return { w, h, red, place, pixels: quantize(data, w, h, { red, dither: set.dither }) };
  }

  // The previews of the models in plan (picturePlan), drawn again in the same canvases
  function picDraw(plan = picturePlan(stTags(), TAG_MODELS)) {
    const models = pic ? [...new Set(plan.filter((p) => p.ok).map((p) => p.model))] : [];
    $("st-zoom").disabled = $("st-fit").value === "stretch";
    for (const m of [...picViews.keys()]) if (!models.includes(m)) picViews.delete(m);
    for (const m of models) {
      let v = picViews.get(m);
      if (!v) {
        v = { fig: document.createElement("figure"), canvas: document.createElement("canvas"), cap: document.createElement("figcaption"), place: null };
        v.canvas.tabIndex = 0;
        v.canvas.setAttribute("role", "img");
        picDragging(v, m);
        v.fig.append(v.canvas, v.cap);
        picViews.set(m, v);
      }
      const s = picScreen(m);
      v.place = s.place;
      paintScreen({ width: s.w, height: s.h, pixels: s.pixels }, v.canvas);
      v.canvas.style.maxWidth = 2 * s.w + "px"; // at most two screen pixels per pixel
      v.canvas.setAttribute("aria-label", `The picture on a ${m}" tag`);
      const fill = $("st-fit").value !== "stretch";
      v.cap.textContent = `${m}" (${s.w} x ${s.h})` + (fill ? ": drag the picture, or use the arrow keys, to move it" : "");
    }
    const box = $("st-pics");
    const figs = models.map((m) => picViews.get(m).fig);
    if (figs.length !== box.children.length || figs.some((f, i) => box.children[i] !== f)) box.replaceChildren(...figs);
    box.hidden = !figs.length;
  }

  // Dragging a preview, or its arrow keys, moves the picture on every model's screen
  function picDragging(v, model) {
    const { width: w, height: h } = TAG_MODELS[model];
    let start = null;
    v.canvas.addEventListener("pointerdown", (e) => {
      if (!pic || $("st-fit").value === "stretch") return;
      const r = v.canvas.getBoundingClientRect();
      start = { cx: e.clientX, cy: e.clientY, x: picX, y: picY, k: w / r.width, place: v.place };
      v.canvas.setPointerCapture(e.pointerId);
      v.canvas.classList.add("drag");
    });
    v.canvas.addEventListener("pointermove", (e) => {
      if (!start) return;
      picX = dragTo(start.x, (e.clientX - start.cx) * start.k, w, start.place.dw);
      picY = dragTo(start.y, (e.clientY - start.cy) * start.k, h, start.place.dh);
      picDraw();
    });
    const end = () => {
      start = null;
      v.canvas.classList.remove("drag");
    };
    v.canvas.addEventListener("pointerup", end);
    v.canvas.addEventListener("pointercancel", end);
    v.canvas.addEventListener("keydown", (e) => {
      const d = { ArrowLeft: [-4, 0], ArrowRight: [4, 0], ArrowUp: [0, -4], ArrowDown: [0, 4] }[e.key];
      if (!d || !pic || !v.place || $("st-fit").value === "stretch") return;
      e.preventDefault();
      picX = dragTo(picX, d[0], w, v.place.dw);
      picY = dragTo(picY, d[1], h, v.place.dh);
      picDraw();
    });
  }

  async function picLoad(file) {
    if (!file) return;
    if (file.type && !file.type.startsWith("image/"))
      return picMsg(`${file.name} is not a picture: choose a PNG or JPG.`, "error");
    try {
      const bitmap = await createImageBitmap(file, { imageOrientation: "from-image" });
      pic?.bitmap.close?.();
      pic = { bitmap, name: file.name || "the pasted picture" };
      picX = picY = 0.5;
      picTurn = 0;
      $("st-zoom").value = 1;
      $("st-drop-text").textContent = `${pic.name} (${bitmap.width} x ${bitmap.height}). Drop or paste another to change it.`;
      picMsg("");
    } catch {
      picMsg(`${file.name || "That"} could not be read as a picture: choose a PNG or JPG.`, "error");
    }
    picDialog();
  }

  $("st-file").addEventListener("change", (e) => {
    e.stopPropagation(); // the picture is read first; picLoad() redraws
    picLoad($("st-file").files[0]);
    $("st-file").value = "";
  });
  const drop = $("st-drop");
  drop.addEventListener("dragover", (e) => {
    e.preventDefault();
    drop.classList.add("over");
  });
  drop.addEventListener("dragleave", () => drop.classList.remove("over"));
  drop.addEventListener("drop", (e) => {
    e.preventDefault();
    drop.classList.remove("over");
    picLoad(e.dataTransfer?.files?.[0]);
  });
  // A picture pasted anywhere while the dialog is open
  document.addEventListener("paste", (e) => {
    if (!$("pic-dlg").open) return;
    const f = [...(e.clipboardData?.files || [])].find((x) => x.type.startsWith("image/"));
    if (!f) return;
    e.preventDefault();
    picLoad(f);
  });
  $("st-turn").onclick = () => {
    picTurn = (picTurn + 90) % 360;
    picDialog();
  };
  $("st-pic-reset").onclick = () => {
    picX = picY = 0.5;
    picTurn = 0;
    $("st-zoom").value = 1;
    $("st-bright").value = 0;
    $("st-contrast").value = 0;
    picChanged();
  };

  // The dialog: the picture, how it sits on each model's screen, and the button that sends it
  const picMsg = (text, cls = "") => {
    $("pic-msg").textContent = text;
    $("pic-msg").className = "hint" + (cls ? " " + cls : "");
  };
  function picDialog(plan = picturePlan(stTags(), TAG_MODELS)) {
    const n = plan.filter((p) => p.ok).length;
    $("pic-n").textContent = n === 1 ? plan.find((p) => p.ok).mac : `${n} tags`;
    picDraw(plan);
    $("pic-send").textContent = n === 1 ? "Send to 1 tag" : `Send to ${n} tags`;
    $("pic-send").disabled = !client || !pic || !n || sending || stBusy;
  }
  function picChanged() {
    stSave();
    picDialog();
  }
  function picOpen() {
    picMsg("");
    stMsg("");
    $("pic-dlg").showModal();
    picDialog();
    if (!pic) $("st-file").focus();
  }
  $("pic-dlg").addEventListener("input", picChanged);
  $("pic-dlg").addEventListener("change", picChanged);
  $("pic-cancel").onclick = () => $("pic-dlg").close();
  $("pic-send").onclick = () => sendPic(stTags());

  // The picture to every ticked tag that can take it, as its model's raw screen image
  async function sendPic(tags) {
    if (!client || !pic || sending || stBusy) return;
    const plan = picturePlan(tags, TAG_MODELS);
    const ok = plan.filter((p) => p.ok);
    if (!ok.length) return picMsg("None of the ticked tags can take it.", "error");
    const images = {};
    for (const m of new Set(ok.map((p) => p.model))) {
      const s = picScreen(m);
      images[m] = packRaw(s.pixels, s.w, s.h, s.red);
    }
    stBusy = true;
    $("pic-send").disabled = $("st-send").disabled = $("st-forget").disabled = true;
    picMsg("Sending the picture ...");
    try {
      const replies = await sendPictures(client, plan, images, (d, n) => n > 1 && picMsg(`Sending the picture: ${d} of ${n} tags ...`));
      const waiting = replies.filter((r) => r.pending !== false).length;
      const who = ok.length === 1 ? "The tag shows" : `${ok.length} tags show`;
      const left = plan.length - ok.length;
      $("pic-dlg").close();
      stMsg((waiting ? `Picture sent. ${who} it at the next poll.` : "Sent; the tags already show this picture.") +
        (left ? ` ${left} ticked ${left === 1 ? "tag gets" : "tags get"} nothing (see the list).` : ""), "ok");
    } catch (e) {
      picMsg((e.sent ? `Only ${e.sent} of ${ok.length} tags got it: ` : "Not sent: ") + e.message, "error");
    } finally {
      stBusy = false;
      $("st-send").disabled = false;
      $("st-forget").disabled = !client;
      if ($("pic-dlg").open) picDialog();
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
      const mac = c.closest("tr").dataset.mac;
      c.checked = on;
      if (on) stSel.add(mac);
      else stSel.delete(mac);
    }
    stChanged();
  };
  $("st-goto").onclick = () => {
    $("st-form").scrollIntoView({ block: "start", behavior: matchMedia("(prefers-reduced-motion: reduce)").matches ? "auto" : "smooth" });
    $("st-what").focus({ preventScroll: true });
  };
  $("st-clear").onclick = () => {
    stSel.clear();
    stChanged();
    render();
  };
  const forgetMsg = (text, cls = "") => {
    $("st-forget-msg").textContent = text;
    $("st-forget-msg").className = "hint" + (cls ? " " + cls : "");
    $("st-forget-msg").hidden = !text;
  };
  // The ticked tags off the AP, e.g. a broken one: content, settings and scheduled content go.
  // A tag that still polls comes back, without them.
  $("st-forget").onclick = async () => {
    const tags = stTags();
    if (!client || !tags.length || sending || stBusy) return;
    const what = tags.length === 1 ? tags[0].mac : `${tags.length} tags`;
    // a tag Find or a filter hides is forgotten too, so the question says so
    const hidden = stHidden(tags.length);
    const unseen = !hidden ? "" : tags.length === 1 ? " (not shown in the table now)"
      : hidden === tags.length ? " (none of them shown in the table now)" : ` (${hidden} of them not shown in the table now)`;
    if (!confirm(`Forget ${what}${unseen}? The AP removes ${tags.length === 1 ? "its" : "their"} content, settings and scheduled content. ` +
      `A tag that still polls shows up again, as a new tag without them.`)) return;
    // asked twice: it cannot be undone
    if (!confirm(`Are you sure? Forgetting ${what} cannot be undone.`)) return;
    const c = client;
    sending = true;
    clearTimeout(timer);
    forgetMsg("");
    $("st-forget").disabled = $("st-send").disabled = true;
    let n = 0;
    try {
      for (const t of tags) {
        try {
          await c.call("DELETE", "/api/tags/" + encodeURIComponent(t.mac));
        } catch (e) {
          if (!e.message.startsWith("404")) throw e; // already gone
        }
        stSel.delete(t.mac);
        n++;
      }
      forgetMsg(n === 1 ? `${tags[0].mac} forgotten.` : `${n} tags forgotten.`, "ok");
    } catch (e) {
      forgetMsg((n ? `Only ${n} of ${tags.length} tags forgotten: ` : "Not forgotten: ") + e.message, "error");
    } finally {
      sending = false;
      $("st-forget").disabled = !client;
      stSave();
      if (client === c) refresh();
    }
  };

  // ---- Firmware card ----

  const fwMsg = (text, cls = "") => {
    $("fw-msg").textContent = text;
    $("fw-msg").className = "hint" + (cls ? " " + cls : "");
  };

  // The closed firmware card says in one line what it holds
  function fwSummary(ota, tagfw, img, rimg) {
    const parts = [];
    if (ota) parts.push(`AP ${ota.version}` + (img?.newer ? ` (${img.version} available)` : ""));
    if (ota?.radio?.version) parts.push(`radio ${ota.radio.version}` + (rimg?.newer ? ` (${rimg.version} available)` : ""));
    for (const r of tagFirmwareRows(tagfw)) if (r.version) parts.push(`${r.model}" ${r.version}: ${r.text}`);
    return parts.join(" · ") || "AP and tag updates";
  }
  const openFw = () => ($("dash-fw-box").open = true);

  function renderFirmware() {
    const { ota, tagfw } = last;
    const facts = !ota ? [] : [
      ["AP firmware", `${ota.version} (${ota.slot}${ota.confirmed === false ? ", not confirmed yet" : ""})`],
      ["Other slot", ota.other_slot ? `${ota.other_slot}: ${ota.other_version || "empty"}` : "none (old partition table)"],
    ];
    if (ota && typeof ota.display === "boolean") facts.push(["Status screen", ota.display ? "yes" : "no"]);
    // An AP with a C6 radio on UART: what the radio runs, and whether the AP hears it
    const radio = ota?.radio;
    if (radio)
      facts.push(["Radio", !radio.link ? "not heard from yet: check the wiring"
        : `${radio.version || "older than 0.8.2"}${radio.display ? ", with screen" : ""}, ` +
          (radio.up ? "answering" : "not answering") + (radio.ota ? "" : " (update it once over USB)")]);
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
    const rimg = radioImageFor(ota, pageInfo);
    $("dash-fw-sum").textContent = fwSummary(ota, tagfw, img, rimg);
    if (rimg?.newer && radio.up) {
      const dd = [...$("dash-ota").querySelectorAll("dt")].find((dt) => dt.textContent === "Radio")?.nextElementSibling;
      const b = document.createElement("button");
      b.className = "small fw-up";
      b.textContent = "\u2191 " + rimg.version; // arrow up
      b.title = `Update the radio to ${rimg.version}` + (rimg.display ? " (with the screen)" : "");
      b.setAttribute("aria-label", "Update the radio to " + rimg.version);
      b.disabled = sending;
      b.onclick = () => sendPageRadioFirmware(rimg);
      dd?.append(" ", b);
    }
    if (img?.newer) {
      const dd = $("dash-ota").querySelector("dd");
      const b = document.createElement("button");
      b.className = "small fw-up";
      b.textContent = "\u2191 " + img.version; // arrow up
      b.title = `Update the AP to ${img.version}` + (img.display ? " (with the status screen)" : "");
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

  // The choices in "Send": the files published with this page, then a file of your own. The AP
  // images are one per board (ap_boards in info.json; one from before that has the C6 ones)
  const TAG_FILES = Object.entries(TAG_MODELS).map(([name, m]) => [`tag-${m.build}-ota`, `Tag firmware for the ${name}"`]);
  const apBoards = (info) => info?.ap_boards || [
    { file: "ap-openesl", target: "esp32c6", display: false },
    { file: "ap-openesl-lcd", target: "esp32c6", display: true },
  ];
  // The images for a C6 radio on UART that the AP passes on (radios in info.json with "ota")
  const radioFiles = (info) => (info?.radios || []).filter((r) => r.kind === "uart" && r.ota);
  const radioFileLabel = (r) => "Radio firmware for the S3's C6" + (r.display ? " with screen" : "");
  const apFileLabel = (b) => "AP firmware" + (b.target !== "esp32c6" ? " for the " + b.target.toUpperCase().replace(/^ESP32/, "ESP32-") : "") +
    (b.display ? " with the status screen" : "");
  // The tag images published with this page, read once: model -> { data, kind }
  const pageImgs = {};
  const pageData = {};
  let pageInfo = null;    // firmware/info.json
  async function loadPageTagImages(info) {
    for (const k of Object.values(TAG_MODELS).map((m) => `tag-${m.build}`)) {
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
    // "Send" can give the ticked tags this firmware
    const models = Object.keys(pageImgs);
    const o = $("st-what").querySelector('option[value="fw"]');
    o.disabled = o.hidden = !models.length;
    o.textContent = "New firmware (" + models.map((m) => `${m}" ${pageImgs[m].version}`).join(", ") + ")";
    if (models.length && store.get("st.what") === "fw") $("st-what").value = "fw";
    if (last) render();
  }

  fetch(`${firmwareBase}firmware/info.json`, { cache: "no-cache" })
    .then((r) => (r.ok ? r.json() : null))
    .catch(() => null)
    .then((info) => {
      pageInfo = info;
      // each file with its own version: the AP's and the tags' can differ (ap_version, tag_version)
      const ver = (k) => (k.startsWith("ap") ? info.ap_version : k.startsWith("radio") ? radioFiles(info).find((r) => r.ota === k)?.version : info.tag_version) || info.version;
      const files = [...TAG_FILES, ...apBoards(info).map((b) => [b.file + "-ota", apFileLabel(b)]), ...radioFiles(info).map((r) => [r.ota, radioFileLabel(r)])];
      const opts = files.filter(([k]) => info?.ota?.[k.replace(/-ota$/, "")]).map(([k, label]) => [k, `${label}, ${ver(k)}`]);
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
    if (on) openFw();
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
    const board = apBoards(pageInfo).find((b) => b.file + "-ota" === v) || radioFiles(pageInfo).find((r) => r.ota === v);
    return sendData(data, board ? !!board.display : null);
  }

  // The arrow-up button next to the radio
  async function sendPageRadioFirmware(img) {
    if (!client || sending) return;
    let data;
    try {
      data = await pageFile(img.file);
    } catch (e) {
      return fwMsg("Could not read the firmware: " + e.message, "error");
    }
    openFw();
    $("fw-msg").scrollIntoView({ block: "nearest" });
    return sendData(data, img.display);
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
    openFw();
    $("fw-msg").scrollIntoView({ block: "nearest" });
    return sendData(data, img.display);
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
    openFw();
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

  // lcd: whether an AP or radio image is the one with the screen (null: a file of your own);
  // to: the one tag a tag image is for (null: all of its model)
  async function sendData(data, lcd = null, to = null) {
    const kind = firmwareKind(data);
    if (!kind.kind) return fwMsg("Not sent: " + kind.error + ".", "error");
    const what = kind.kind === "tag" ? `tag firmware ${kind.version} for the ${kind.model}"`
      : `${kind.kind === "radio" ? "radio" : "AP"} firmware${kind.version ? " " + kind.version : ""}`;
    if (kind.kind === "ap" && !confirm(apSendQuestion(kind, last?.ota, lcd))) return;
    if (kind.kind === "radio" && !confirm(radioSendQuestion(kind, last?.ota, lcd))) return;
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
      } else if (kind.kind === "radio") {
        const r = await sendRadioFirmware(c, data, progress);
        fwMsg(`The radio has firmware ${r.version} and is restarting into it ...`);
        const back = await waitRadioBack(c, r.version, { alive: () => client === c });
        fwMsg(back.text, back.ok ? "ok" : "error");
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
  // The folded cards stay as they were left
  for (const id of ["dash-ap-box", "dash-fw-box"]) {
    $(id).open = store.get(id) === "open";
    $(id).addEventListener("toggle", () => store.set(id, $(id).open ? "open" : "closed"));
  }
  $("dash-auto").addEventListener("change", () => (client && $("dash-auto").checked ? refresh() : clearTimeout(timer)));
  document.addEventListener("visibilitychange", () => visible() && client && $("dash-auto").checked && refresh());
  window.addEventListener("hashchange", () => visible() && client && refresh());

  // The USB port opened or closed elsewhere (the WiFi card on the Set up tab)
  onSerialChange((ap) => {
    if (src() !== "serial") return;
    if (ap && !client) setConnected(ap, "the AP over USB");
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
        setConnected(serialAp() || (await connectSerial()), "the AP over USB");
      } else {
        const token = $("dash-token").value.trim();
        if (sameOrigin) {
          if (!token) throw new Error("enter the AP's API token (set when it was flashed, or on its /setup page).");
          store.set("dash.token", token);  // kept in this browser, for this AP's address only
          setConnected(new ApClient("", token), location.host || "this AP");
          return;
        }
        const { base, error } = apBase($("dash-url").value);
        if (error) throw new Error(error + " Or use USB.");
        setConnected(new ApClient(base, token), base.replace(/^https?:\/\//, ""));
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
