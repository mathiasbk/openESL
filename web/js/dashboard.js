// dashboard.js - the Dashboard tab: the AP's state, its WiFi and every tag, from GET /api/tags,
// /api/health and /api/wifi. Over the AP's USB port (js/apport.js, shared with the WiFi card)
// or over HTTP through the bridge in tools/esl-client (js/apclient.js).
import { ApClient } from "./apclient.js";
import { describeWifi } from "./apserial.js";
import { hasSerial, serialAp, connectSerial, disconnectSerial, onSerialChange } from "./apport.js";
import { fmtAgo, fmtTagFw, fmtBattery, tagRows, healthLine, apFacts } from "./dashdata.js";

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
  let last = null;        // { list, health, wifi }

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
      if (c !== client) return;
      last = { list, health, wifi };
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
        [fmtTagFw(tag.fw)],
        [state.text, "state state-" + state.level],
      ];
      for (const [text, cls] of cells) {
        const td = document.createElement("td");
        td.textContent = text;
        if (cls) td.className = cls;
        tr.append(td);
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
