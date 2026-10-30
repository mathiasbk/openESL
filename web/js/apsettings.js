// apsettings.js - the Settings page of the dashboard, a menu of groups shown one at a time:
// General (the AP's name, time zone and the tags' poll interval), Network (WiFi, webhook and NTP
// server), Radio (the channel and the transmit power, on an AP with a radio), Security (the API token) and
// Backup (js/backup.js). Through the same client as the
// rest of the dashboard (GET/PUT /api/config, /api/wifi, PUT /api/time). A group of settings
// has one Save, shown with Undo only once something in it differs from what the AP has.
import { checkToken, newToken, checkHostname, checkWifi, describeWifi } from "./apserial.js";
import { fmtInterval, parsePoll, pollNote, POLL_PRESETS, TAG_BUILTIN_POLL_S, txOptions, parseTx, txWarning, TX_NOTE,
  channelOptions, channelMove, channelBlock, CHANNEL_NOTE } from "./dashdata.js";
import { makeBackup, restoreBackup, parseBackup, backupSummary, backupName } from "./backup.js";

// Time zones as POSIX TZ strings, which is what the AP understands (it has no tz database):
// [tz, label, the browser's zone names that use it]
export const TZ_PRESETS = [
  ["", "UTC", ["UTC", "Etc/UTC", "Etc/GMT"]],
  ["CET-1CEST,M3.5.0,M10.5.0/3", "Central Europe (Norway, Sweden, Denmark, Germany …)",
    ["Europe/Oslo", "Europe/Stockholm", "Europe/Copenhagen", "Europe/Berlin", "Europe/Amsterdam", "Europe/Brussels",
      "Europe/Paris", "Europe/Vienna", "Europe/Zurich", "Europe/Rome", "Europe/Madrid", "Europe/Warsaw", "Europe/Prague",
      "Europe/Budapest", "Arctic/Longyearbyen"]],
  ["GMT0BST,M3.5.0/1,M10.5.0", "United Kingdom", ["Europe/London"]],
  ["IST-1GMT0,M10.5.0,M3.5.0/1", "Ireland", ["Europe/Dublin"]],
  ["WET0WEST,M3.5.0/1,M10.5.0", "Portugal", ["Europe/Lisbon"]],
  ["EET-2EEST,M3.5.0/3,M10.5.0/4", "Eastern Europe (Finland, the Baltics, Greece …)",
    ["Europe/Helsinki", "Europe/Tallinn", "Europe/Riga", "Europe/Vilnius", "Europe/Athens", "Europe/Bucharest", "Europe/Sofia"]],
  ["EST5EDT,M3.2.0,M11.1.0", "US Eastern", ["America/New_York", "America/Toronto", "America/Detroit"]],
  ["CST6CDT,M3.2.0,M11.1.0", "US Central", ["America/Chicago", "America/Winnipeg"]],
  ["MST7MDT,M3.2.0,M11.1.0", "US Mountain", ["America/Denver", "America/Edmonton"]],
  ["PST8PDT,M3.2.0,M11.1.0", "US Pacific", ["America/Los_Angeles", "America/Vancouver"]],
  ["AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia East", ["Australia/Sydney", "Australia/Melbourne", "Australia/Hobart"]],
];

// The preset for the browser's own zone ("Europe/Oslo" -> the CET entry), or null
export function tzForZone(zone) {
  return TZ_PRESETS.find(([, , zones]) => zones.includes(zone)) || null;
}

// The same checks as esl_tz_ok() and esl_ntp_name_ok() on the AP: an error, or null
export function checkTz(tz) {
  if (!tz) return null;
  // the POSIX form, a name and its offset: the AP takes a tzdata name such as Europe/Oslo as UTC
  if (tz.length > 63 || !/^([A-Za-z]{3,}|<[A-Za-z0-9+-]+>)[+-]?[0-9][A-Za-z0-9+,./:<>-]*$/.test(tz))
    return "A time zone is a POSIX TZ string of at most 63 characters, such as CET-1CEST,M3.5.0,M10.5.0/3 (not a name such as Europe/Oslo).";
  return null;
}
export function checkNtp(name) {
  if (!name) return null;
  if (name.length > 63 || !/^[A-Za-z0-9][A-Za-z0-9.-]*$/.test(name))
    return "An NTP server is a host name or IP address of at most 63 letters, digits, '-' and '.'.";
  return null;
}
// The same check as esl_webhook_url_ok() on the AP, in short: plain http to a host
export function checkWebhook(url) {
  if (!url) return null;
  if (url.length > 127 || !/^http:\/\/[^/][\x21-\x7e]*$/.test(url) || /["\\]/.test(url))
    return "The webhook must be an http://host[:port][/path] address of at most 127 characters (https is not supported).";
  return null;
}

// "the AP's clock is 2 min 5 s behind this computer" or "" when within a few seconds
export function clockSkew(apTime, nowS) {
  const d = Math.round(apTime - nowS);
  if (Math.abs(d) < 5) return "";
  const a = Math.abs(d);
  const t = a >= 3600 ? `${Math.floor(a / 3600)} h ${Math.floor((a % 3600) / 60)} min`
    : a >= 60 ? `${Math.floor(a / 60)} min ${a % 60} s` : `${a} s`;
  return `${t} ${d < 0 ? "behind" : "ahead of"} this computer`;
}

// What the AP has for the settings on the page, as the fields show them: "" for none or
// built in, 0 for the tags' own poll default. Keys the AP does not have are left out
// (an AP without WiFi has no name, older firmware no time zone or NTP server, an AP
// without a radio no transmit power or channel). The channel is the one the AP moves to while
// a move is waiting.
export function currentSettings(last) {
  const c = last?.config || {}, cur = {};
  if (last?.wifi) cur.hostname = last.wifi.hostname || "";
  if ("tz" in c) cur.tz = c.tz || "";
  if (last?.list) cur.poll_s = last.list.ap.poll_s || 0;
  if (last?.config) cur.webhook = c.webhook || "";
  if ("ntp" in c) cur.ntp = c.ntp && c.ntp !== c.builtin_ntp ? c.ntp : "";
  if (Number.isInteger(c.tx_power)) cur.tx_power = c.tx_power;
  if (typeof c.radio_secure === "boolean") cur.radio_secure = c.radio_secure;
  if (Number.isInteger(c.channel)) cur.channel = Number.isInteger(c.channel_next) ? c.channel_next : c.channel;
  return cur;
}

// The keys of chosen that differ from current (and that current has)
export function settingsChanges(chosen, current) {
  return Object.fromEntries(Object.entries(chosen).filter(([k, v]) => k in current && v !== current[k]));
}

// The PUT /api/config body for changes; the AP's built-in values go as null (a reset), so a
// later firmware default applies
export function configBody(changes, config = {}) {
  const body = {};
  if ("tz" in changes) body.tz = changes.tz || null;
  if ("ntp" in changes) body.ntp = changes.ntp || null;
  if ("webhook" in changes) body.webhook = changes.webhook || null;
  if ("poll_s" in changes) body.poll_s = changes.poll_s === config.builtin_poll_s ? null : changes.poll_s;
  if ("tx_power" in changes) body.tx_power = changes.tx_power === config.builtin_tx_power ? null : changes.tx_power;
  if ("radio_secure" in changes) body.radio_secure = changes.radio_secure;
  if ("channel" in changes) body.channel = changes.channel === config.builtin_channel ? null : changes.channel;
  return body;
}

const $ = (id) => document.getElementById(id);
const plural = (n, one, many) => `${n} ${n === 1 ? one : many}`;

// getClient() is the dashboard's client (null when not connected), overUsb() whether it is the
// USB port, refresh() reads the AP again, onToken(token) is told a new token was saved
export function initSettings({ getClient, refresh, overUsb = () => false, onToken = () => {} }) {
  let last = null;
  const say = (id, text, cls = "") => {
    $(id).textContent = text;
    $(id).className = "hint" + (cls ? " " + cls : "");
  };
  const busy = async (button, fn) => {
    button.disabled = true;
    try {
      await fn();
    } finally {
      button.disabled = false;
    }
  };

  // ---- the two views of the dashboard, and the menu of groups ----
  const views = [...document.querySelectorAll("#dash-views button")];
  const showView = (v) => {
    for (const b of views) b.setAttribute("aria-pressed", String(b.dataset.view === v));
    $("dash-overview").hidden = v !== "overview";
    $("dash-settings").hidden = v !== "settings";
  };
  for (const b of views) b.addEventListener("click", () => showView(b.dataset.view));
  const tabs = [...document.querySelectorAll(".set-nav button")];
  const showPane = (name) => {
    for (const t of tabs) {
      const on = t.dataset.set === name;
      t.setAttribute("aria-selected", String(on));
      t.tabIndex = on ? 0 : -1;
      $("set-" + t.dataset.set).hidden = !on;
    }
  };
  for (const t of tabs) {
    t.addEventListener("click", () => showPane(t.dataset.set));
    t.addEventListener("keydown", (ev) => {
      const shown = tabs.filter((x) => !x.hidden);
      const i = shown.indexOf(t) + (ev.key === "ArrowDown" || ev.key === "ArrowRight" ? 1 : ev.key === "ArrowUp" || ev.key === "ArrowLeft" ? -1 : 0);
      if (i === shown.indexOf(t)) return;
      ev.preventDefault();
      const next = shown[(i + shown.length) % shown.length];
      showPane(next.dataset.set);
      next.focus();
    });
  }
  showPane("general");

  // A group of settings with one Save: read() is what the fields say ({ key: value } or
  // { error }), fill(cur) puts what the AP has in them, save(changes) stores the changes.
  // While nothing was typed the fields follow the AP; after that they keep what was typed.
  function group(form, msgId, { read, fill, save }) {
    const undo = form.querySelector(".set-undo"), saveBtn = form.querySelector(".set-save");
    const g = { dirty: false };
    const changes = () => {
      const r = read();
      return r.error ? r : settingsChanges(r, currentSettings(last));
    };
    // the count of changes, unless what the last Save said is still up
    g.show = () => {
      const c = changes();
      const n = c.error ? 1 : Object.keys(c).length;
      undo.hidden = saveBtn.hidden = !n;
      const m = $(msgId);
      if (!m.classList.contains("error") && !m.classList.contains("ok"))
        m.textContent = n && !c.error ? plural(n, "change", "changes") : "";
    };
    g.render = () => {
      if (!g.dirty) fill(currentSettings(last));
      g.show();
    };
    g.reset = () => {
      g.dirty = false;
      say(msgId, "");
      if (last) g.render();
    };
    const changed = () => {
      g.dirty = true;
      say(msgId, "");
      g.show();
    };
    form.addEventListener("input", changed);
    form.addEventListener("change", changed);
    undo.addEventListener("click", g.reset);
    form.addEventListener("submit", async (ev) => {
      ev.preventDefault();
      const client = getClient();
      const c = changes();
      if (c.error) return say(msgId, c.error, "error");
      if (!client || !Object.keys(c).length) return;
      await busy(saveBtn, async () => {
        try {
          const done = await save(client, c);
          g.dirty = false;
          say(msgId, done, "ok");
          refresh();
        } catch (e) {
          say(msgId, "Not saved: " + e.message, "error");
        }
      });
      g.show();
    });
    return g;
  }

  // ---- General: name, time zone, poll interval ----
  const zone = (() => { try { return Intl.DateTimeFormat().resolvedOptions().timeZone; } catch { return ""; } })();
  const tzSel = $("set-tz-sel");
  for (const [tz, label] of TZ_PRESETS) tzSel.append(new Option(label, tz));
  tzSel.append(new Option("Other …", "other"));
  const pollSel = $("set-poll-sel");
  pollSel.append(new Option(`${fmtInterval(TAG_BUILTIN_POLL_S)} (tag default)`, "0"));
  for (const s of POLL_PRESETS) pollSel.append(new Option(fmtInterval(s), String(s)));
  pollSel.append(new Option("Other …", "other"));
  const showOthers = () => {
    $("set-tz-other").hidden = tzSel.value !== "other";
    const other = pollSel.value === "other";
    $("set-poll-s").hidden = $("set-poll-unit").hidden = !other;
    const p = other ? parsePoll($("set-poll-s").value) : { poll_s: Number(pollSel.value) };
    $("set-poll-note").textContent = p.error ? "" : pollNote(p.poll_s, last?.list?.tags);
  };
  tzSel.addEventListener("change", () => {
    if (tzSel.value === "other" && !$("set-tz").value) $("set-tz").value = last?.config?.tz || "";
  });
  pollSel.addEventListener("change", () => {
    if (pollSel.value === "other" && !$("set-poll-s").value) $("set-poll-s").value = last?.list?.ap.poll_s || TAG_BUILTIN_POLL_S;
    if (pollSel.value === "other") $("set-poll-s").focus();
  });
  const general = group($("set-general"), "set-general-msg", {
    read() {
      showOthers();
      const cur = currentSettings(last), r = {};
      if ("hostname" in cur) {
        r.hostname = $("set-hostname").value.trim();
        const err = r.hostname !== cur.hostname && checkHostname(r.hostname);
        if (err) return { error: err };
      }
      if ("tz" in cur) {
        r.tz = tzSel.value === "other" ? $("set-tz").value.trim() : tzSel.value;
        const err = checkTz(r.tz);
        if (err) return { error: err };
      }
      const p = pollSel.value === "other" ? parsePoll($("set-poll-s").value) : { poll_s: Number(pollSel.value) };
      if (p.error) return p;
      r.poll_s = p.poll_s;
      return r;
    },
    fill(cur) {
      $("set-hostname").value = cur.hostname ?? "";
      const preset = TZ_PRESETS.some(([tz]) => tz === cur.tz);
      tzSel.value = preset ? cur.tz : "other";
      $("set-tz").value = preset ? "" : cur.tz ?? "";
      const pp = [...pollSel.options].some((o) => o.value === String(cur.poll_s));
      pollSel.value = pp ? String(cur.poll_s) : "other";
      $("set-poll-s").value = pp ? "" : cur.poll_s;
      showOthers();
    },
    async save(client, c) {
      const body = configBody(c, last.config);
      if (Object.keys(body).length) await client.call("PUT", "/api/config", body);
      // the name last: the AP rejoins the network with it
      if ("hostname" in c) await client.call("PUT", "/api/wifi", { hostname: c.hostname });
      return "Saved." + ("poll_s" in c ? " The tags take the new interval at their next poll." : "") +
        ("hostname" in c ? ` The AP rejoins the network as ${c.hostname}.local; this page may lose it for a few seconds.` : "");
    },
  });
  function renderClock() {
    const c = last.config || {}, ap = last.list.ap;
    $("set-tz-field").hidden = !("tz" in c);
    if (!("tz" in c)) $("set-tz-other").hidden = true;
    let line;
    if (!("time" in ap)) line = "This AP has no clock (built without scheduling).";
    else if (!ap.time) line = "The AP does not know the time yet: no answer from the time server.";
    else {
      line = `AP time ${new Date(ap.time * 1000).toLocaleString([], { dateStyle: "medium", timeStyle: "short" })}`;
      const skew = clockSkew(ap.time, Date.now() / 1000);
      line += skew ? `, ${skew}.` : ", same as this computer.";
    }
    if (!("tz" in c)) line += " This AP's firmware has no time zone setting yet: update it to choose one.";
    else if (!c.tz) {
      const mine = tzForZone(zone);
      if (mine?.[0]) line += ` The AP is on UTC; this computer is in ${mine[1].replace(/ \(.*/, "")}.`;
    }
    $("set-time").textContent = line;
    $("set-time-now").hidden = !("time" in ap) || (!!ap.time && !clockSkew(ap.time, Date.now() / 1000));
  }
  $("set-time-now").addEventListener("click", () => busy($("set-time-now"), async () => {
    const client = getClient();
    if (!client) return;
    try {
      await client.call("PUT", "/api/time", { time: Math.round(Date.now() / 1000) });
      say("set-general-msg", "The AP's clock is set from this computer. The time server sets it again when it answers.", "ok");
      refresh();
    } catch (e) {
      say("set-general-msg", "Not set: " + e.message, "error");
    }
  }));

  // ---- Network: WiFi (joined at once), webhook and NTP server (one Save) ----
  const netcfg = group($("set-netcfg"), "set-netcfg-msg", {
    read() {
      const cur = currentSettings(last), r = { webhook: $("set-hook-url").value.trim() };
      let err = checkWebhook(r.webhook);
      if ("ntp" in cur) {
        r.ntp = $("set-ntp").value.trim();
        err = err || checkNtp(r.ntp);
      }
      return err ? { error: err } : r;
    },
    fill(cur) {
      $("set-hook-url").value = cur.webhook ?? "";
      $("set-ntp").value = cur.ntp ?? "";
    },
    async save(client, c) {
      await client.call("PUT", "/api/config", configBody(c, last.config));
      return "Saved." + ("webhook" in c ? (c.webhook ? " The AP posts to the webhook from the next change." : " The webhook is off.") : "");
    },
  });
  const ssidSel = $("set-ssid-sel");
  let wifiDirty = false;
  $("set-wifi").addEventListener("input", () => { wifiDirty = true; say("set-wifi-msg", ""); });
  $("set-wifi").addEventListener("change", () => {
    wifiDirty = true;
    $("set-ssid-other").hidden = ssidSel.value !== "other";
  });
  // the network fields only after "Change network …": joining another one is rarely wanted
  const editWifi = (on) => {
    $("set-wifi-edit").hidden = !on;
    $("set-wifi-change").hidden = on;
    if (!on) {
      wifiDirty = false;
      $("set-pass").value = "";
      if (last) renderNet();
    }
  };
  $("set-wifi-change").addEventListener("click", () => {
    editWifi(true);
    say("set-wifi-msg", "");
    ssidSel.focus();
  });
  $("set-wifi-cancel").addEventListener("click", () => editWifi(false));
  function renderNet() {
    const w = last.wifi, c = last.config || {};
    $("set-wifi").hidden = !w;
    $("set-ntp-field").hidden = !("ntp" in c);
    if (c.builtin_ntp) $("set-ntp").placeholder = `${c.builtin_ntp} (built in)`;
    if (!w) return;
    $("set-wifi-now").textContent = describeWifi(w);
    $("set-host-note").textContent = w.hostname ? `Reached at http://${w.hostname}.local` : "";
    if (!wifiDirty) {
      const names = [...new Set((w.networks || []).map((n) => n.ssid).filter(Boolean))];
      if (w.ssid && !names.includes(w.ssid)) names.unshift(w.ssid);
      ssidSel.replaceChildren(...names.map((n) => new Option(n + (n === w.ssid ? (w.connected ? " (joined)" : " (saved)") : ""), n)),
        new Option("Other …", "other"));
      ssidSel.value = w.ssid || (names[0] ?? "other");
      $("set-ssid-other").hidden = ssidSel.value !== "other";
    }
  }
  $("set-wifi").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const client = getClient();
    const ssid = ssidSel.value === "other" ? $("set-ssid").value : ssidSel.value, password = $("set-pass").value;
    const err = checkWifi(ssid, password);
    if (err) return say("set-wifi-msg", err, "error");
    if (!client) return;
    const w = last?.wifi;
    const away = !overUsb(); // over WiFi this page goes with it
    if (!confirm(`The AP leaves ${w?.ssid ? `"${w.ssid}"` : "its setup network"} and joins "${ssid}".` +
      (away ? " This page loses it until you open it on the new network." : "") +
      " If it cannot join, it opens its setup network again after a while, where you can fix it. Go on?")) return;
    await busy($("set-wifi-save"), async () => {
      try {
        await client.call("PUT", "/api/wifi", { ssid, password });
        wifiDirty = false;
        $("set-pass").value = "";
        editWifi(false);
        say("set-wifi-msg", `The AP is joining "${ssid}".`, "ok");
        refresh();
      } catch (e) {
        say("set-wifi-msg", "Not saved: " + e.message, "error");
      }
    });
  });

  // ---- Radio: the channel and the transmit power, on an AP that has them, and whether it answers only paired tags ----
  const chSel = $("set-ch-sel");
  const showMove = () => {
    const move = channelMove(last?.config);
    $("set-ch-move").textContent = move;
    $("set-ch-move-box").hidden = !move;
  };
  $("set-ch-now").addEventListener("click", () => busy($("set-ch-now"), async () => {
    const client = getClient(), c = last?.config;
    if (!client || !Number.isInteger(c?.channel_next)) return;
    const n = c.channel_waiting || 0;
    if (n && !confirm(`The AP moves to channel ${c.channel_next} now. ${n === 1 ? "1 tag has" : `${n} tags have`} not been told yet: `
      + (c.channel_next === c.builtin_channel ? "tags with firmware 0.9.0 or newer find it on the built-in channel after three missed polls"
        : "tags with firmware 0.9.0 or newer look for it on the other channels after three missed polls")
      + `; older ones lose the AP until it is back on channel ${c.channel}. Move now?`)) return;
    try {
      await client.call("PUT", "/api/config", { channel: c.channel_next === c.builtin_channel ? null : c.channel_next, channel_now: true });
      say("set-radio-msg", `The AP is on channel ${c.channel_next}.`, "ok");
      refresh();
    } catch (e) {
      say("set-radio-msg", "Not moved: " + e.message, "error");
    }
  }));
  const txSel = $("set-tx-sel");
  const showTx = () => {
    const other = txSel.value === "other";
    $("set-tx-db").hidden = $("set-tx-unit").hidden = !other;
    const c = other ? parseTx($("set-tx-db").value) : { dbm: Number(txSel.value) };
    $("set-tx-note").textContent = [c.error ? "" : txWarning(c.dbm), TX_NOTE].filter(Boolean).join(" ");
  };
  txSel.addEventListener("change", () => {
    if (txSel.value === "other" && !$("set-tx-db").value) $("set-tx-db").value = last?.config?.tx_power ?? "";
    if (txSel.value === "other") $("set-tx-db").focus();
  });
  const radio = group($("set-radio"), "set-radio-msg", {
    read() {
      showTx();
      const cur = currentSettings(last), r = {};
      if ("radio_secure" in cur) r.radio_secure = $("set-secure").checked;
      if ("channel" in cur) r.channel = Number(chSel.value);
      if (!("tx_power" in cur)) return r;
      const c = txSel.value === "other" ? parseTx($("set-tx-db").value) : { dbm: Number(txSel.value) };
      return c.error ? c : { ...r, tx_power: c.dbm };
    },
    fill(cur) {
      $("set-secure-field").hidden = !("radio_secure" in cur);
      $("set-secure").checked = !!cur.radio_secure;
      const c = last.config;
      $("set-ch-field").hidden = !("channel" in cur);
      if ("channel" in cur) {
        chSel.replaceChildren(...channelOptions(c.builtin_channel).map(([v, text]) => new Option(text, String(v))));
        chSel.value = String(cur.channel);
        $("set-ch-note").textContent = [channelBlock(c), CHANNEL_NOTE].filter(Boolean).join(" ");
      }
      showMove();
      $("set-tx-field").hidden = !("tx_power" in cur);
      if (!("tx_power" in cur)) return;
      txSel.replaceChildren(...txOptions(c.tx_power, c.builtin_tx_power).map(([v, text]) => new Option(text, String(v))),
        new Option("Other …", "other"));
      txSel.value = String(cur.tx_power);
      $("set-tx-db").value = "";
      showTx();
    },
    async save(client, c) {
      const body = configBody(c, last.config);
      let r;
      try {
        r = await client.call("PUT", "/api/config", body);
      } catch (e) {
        // tags with old firmware: moving anyway leaves them behind, so only when asked
        if (!/^409: .*"force"/.test(e.message) || !confirm(e.message.slice(5).replace(/,? or send.*$/, ".")
          + " Change the channel anyway?")) throw e;
        r = await client.call("PUT", "/api/config", { ...body, force: true });
      }
      const said = [];
      if ("channel" in c)
        said.push(c.channel === r?.channel ? `The AP stays on channel ${c.channel}.`
          : `The tags are told at their next poll, and the AP moves to channel ${c.channel} once they have confirmed it.`);
      if ("tx_power" in c) said.push(`The radio sends at ${c.tx_power} dBm from its next reply.`);
      if ("radio_secure" in c) said.push(c.radio_secure ? "The AP now answers only paired tags." : "The AP answers unpaired tags again.");
      return "Saved. " + said.join(" ");
    },
  });

  // ---- Security: the API token ----
  $("set-token-gen").addEventListener("click", () => {
    $("set-token-new").value = newToken();
    say("set-token-msg", "Copy it before you change it: the AP never shows it again.");
  });
  $("set-security").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const client = getClient();
    const token = $("set-token-new").value.trim();
    const err = checkToken(token);
    if (err) return say("set-token-msg", err, "error");
    if (!client) return;
    if (!confirm("Every app that uses the old token (Home Assistant, esl-client, other browsers) gets 401 until it has the new one. Change it?")) return;
    await busy($("set-token-save"), async () => {
      try {
        await client.call("PUT", "/api/wifi", { token });
        onToken(token);
        $("set-token-new").value = "";
        say("set-token-msg", "Changed. This page uses the new token from now on.", "ok");
      } catch (e) {
        say("set-token-msg", "Not changed: " + e.message, "error");
      }
    });
  });

  // ---- backup ----
  let loaded = null; // the backup read from a file, waiting for Restore
  const bar = $("bk-progress");
  const showProgress = ({ done, total }) => {
    bar.hidden = false;
    bar.max = total || 1;
    bar.value = done;
  };
  const showLog = (r) => {
    const li = (text, cls = "") => {
      const e = document.createElement("li");
      e.textContent = text;
      if (cls) e.className = cls;
      return e;
    };
    $("bk-log").replaceChildren(...r.failed.map((t) => li(t, "bad")), ...r.notes.map((t) => li(t)), ...r.done.map((t) => li(t)),
      ...r.waiting.map((m) => li(`Picture for ${m}: waits until the AP has heard the tag; restore again then`)));
    $("bk-log-box").hidden = false;
  };
  $("bk-save").addEventListener("click", () => busy($("bk-save"), async () => {
    const client = getClient();
    if (!client) return;
    say("bk-msg", "Reading the AP …");
    try {
      const b = await makeBackup(client, { progress: showProgress });
      const url = URL.createObjectURL(new Blob([JSON.stringify(b, null, 1)], { type: "application/json" }));
      const a = Object.assign(document.createElement("a"), { href: url, download: backupName(b) });
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 10000);
      const s = backupSummary(b);
      say("bk-msg", `Saved ${backupName(b)}: ${s.tags + s.pictures} tags with content (${s.pictures} pictures), ` +
        `${s.layouts.length} layout file${s.layouts.length === 1 ? "" : "s"}, ${s.scheduled} scheduled.`, "ok");
    } catch (e) {
      say("bk-msg", "No backup: " + e.message, "error");
    } finally {
      bar.hidden = true;
    }
  }));
  $("bk-load").addEventListener("click", () => $("bk-file").click());
  $("bk-file").addEventListener("change", async () => {
    const f = $("bk-file").files[0];
    $("bk-file").value = "";
    if (!f) return;
    try {
      loaded = parseBackup(await f.text());
    } catch (e) {
      loaded = null;
      $("bk-ask").hidden = true;
      return say("bk-msg", `${f.name}: ${e.message}.`, "error");
    }
    const s = backupSummary(loaded);
    const from = [loaded.ap?.hostname, loaded.ap?.board].filter(Boolean).join(", ");
    $("bk-what").textContent = `${f.name}, made ${new Date(loaded.taken).toLocaleString()}${from ? ` on ${from}` : ""}: ` +
      `${s.tags + s.pictures} tags with content (${s.pictures} pictures), ${s.configs} with their own settings, ` +
      `layout files for ${s.layouts.map((m) => m + '"').join(", ") || "no model"}, ${s.scheduled} scheduled. ` +
      "What is on the AP for the same tags and models is replaced; other tags are left as they are." +
      (loaded.radio_key ? " It has the radio key of the paired tags: Settings puts it on this AP." : "");
    $("bk-ask").hidden = false;
    $("bk-log-box").hidden = true;
    say("bk-msg", "");
  });
  $("bk-cancel").addEventListener("click", () => {
    loaded = null;
    $("bk-ask").hidden = true;
  });
  $("bk-go").addEventListener("click", () => busy($("bk-go"), async () => {
    const client = getClient();
    if (!client || !loaded) return;
    const parts = Object.fromEntries(["settings", "layouts", "tags", "scheduled"].map((k) => [k, $("bk-p-" + k).checked]));
    say("bk-msg", "Restoring …");
    try {
      const r = await restoreBackup(client, loaded, { parts, progress: showProgress });
      showLog(r);
      const wait = r.waiting.length ? ` ${r.waiting.length} picture${r.waiting.length === 1 ? "" : "s"} wait for tags the AP has not heard yet: restore again once they have polled.` : "";
      say("bk-msg", r.failed.length ? `Restored with ${r.failed.length} problem${r.failed.length === 1 ? "" : "s"} (below).${wait}` : `Restored.${wait}`,
        r.failed.length ? "error" : "ok");
      if (!r.waiting.length) $("bk-ask").hidden = true;
      if (r.notes.length && !r.failed.length) say("bk-msg", $("bk-msg").textContent + " See the notes below.", "ok");
      refresh();
    } catch (e) {
      say("bk-msg", "Not restored: " + e.message, "error");
    } finally {
      bar.hidden = true;
    }
  }));

  return {
    render(l) {
      last = l;
      if (!last?.list) return;
      const w = last.wifi;
      $("set-host-field").hidden = !w;
      $("set-tab-security").hidden = !w;
      if (!w && !$("set-security").hidden) showPane("general");
      const radioSet = Number.isInteger(last.config?.tx_power) || Number.isInteger(last.config?.channel) ||
        typeof last.config?.radio_secure === "boolean";
      $("set-tab-radio").hidden = !radioSet;
      if (!radioSet && !$("set-radio").hidden) showPane("general");
      renderClock();
      renderNet();
      general.render();
      netcfg.render();
      radio.render();
    },
    // a new connection: what was typed for the old AP is dropped
    reset() {
      general.dirty = netcfg.dirty = radio.dirty = false;
      editWifi(false);
      loaded = null;
      $("bk-ask").hidden = true;
      $("bk-log-box").hidden = true;
      for (const id of ["set-general-msg", "set-netcfg-msg", "set-radio-msg", "set-wifi-msg", "set-token-msg", "bk-msg"]) say(id, "");
    },
  };
}
