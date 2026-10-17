// wifisetup.js - "WiFi and API token over USB": GET and PUT /api/wifi over the AP's USB port
// with Web Serial (js/apserial.js). The same card is on the Flash AP tab (step 2, right after
// flashing) and on the Set up tab; initWifiSetup(prefix) wires one of them, by the prefix of
// its element ids. Both share the one port (js/apport.js).
import { describeWifi, checkWifi, checkToken, newToken } from "./apserial.js";
import { hasSerial, serialAp, connectSerial, disconnectSerial, onSerialChange } from "./apport.js";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const JOIN_WAIT_MS = 30000;   // how long the page watches the AP join after a change
const JOIN_POLL_MS = 2000;

export function initWifiSetup(prefix) {
  const $ = (id) => document.getElementById(`${prefix}-${id}`);
  const btn = $("connect");
  if (!hasSerial()) {
    $("serial-missing").hidden = false;
    btn.disabled = true;
    return;
  }

  const msg = (text, cls = "") => {
    $("msg").textContent = text;
    $("msg").className = "hint" + (cls ? " " + cls : "");
  };
  const busy = (on) => {
    for (const id of ["save", "forget", "connect", "gen"]) $(id).disabled = on;
  };

  const show = (w) => {
    $("state").textContent = describeWifi(w);
    $("networks").replaceChildren(...(w.networks || []).map((n) => {
      const o = document.createElement("option");
      o.value = n.ssid;
      o.label = `${n.rssi} dBm`;
      return o;
    }));
    if (!$("ssid").value && w.ssid) $("ssid").value = w.ssid;
  };

  // The port opened or closed, here or in the other card or on the dashboard
  onSerialChange(async (ap) => {
    btn.textContent = ap ? "Disconnect" : "Connect AP";
    $("form").hidden = !ap;
    if (!ap) {
      $("state").textContent = "";
      msg("Disconnected.");
      return;
    }
    try {
      const w = await ap.wifi();
      show(w);
      // A freshly flashed AP has no token: offer one, so it is not forgotten
      if (w.token_set === false && !$("token").value) $("token").value = newToken();
      const pick = w.networks?.length ? "Pick a network from the list or type its name." : "Type the network's name (the AP has no scan list).";
      msg(w.token_set === false ? pick + " A new API token is filled in: keep a copy of it." : pick);
    } catch (e) {
      msg("The AP did not answer: " + e.message, "error");
    }
  });

  btn.addEventListener("click", async () => {
    if (serialAp()) {
      await disconnectSerial();
      return;
    }
    busy(true);
    msg("Waiting for the AP ...");
    try {
      await connectSerial();
    } catch (e) {
      msg(e.name === "NotFoundError" ? "" : "Could not connect: " + e.message, "error");
    } finally {
      busy(false);
    }
  });

  $("show").addEventListener("change", (ev) => {
    $("pass").type = ev.target.checked ? "text" : "password";
  });
  $("gen").addEventListener("click", () => {
    $("token").value = newToken();
  });

  // After a change: watch the AP until it has joined, or give up after JOIN_WAIT_MS.
  // ssid undefined changes only the token; null forgets the network.
  async function change(ssid, password, token) {
    const ap = serialAp();
    if (!ap) return;
    busy(true);
    const saved = token ? ` The API token is now ${token} (it cannot be read back from the AP).` : "";
    try {
      let w = await ap.setWifi(ssid, password, token);
      show(w);
      if (token) $("token").value = "";
      if (ssid === undefined) {
        msg("Saved." + saved, "ok");
        return;
      }
      if (!ssid) {
        msg("The AP forgot the network and opened its setup network.", "ok");
        return;
      }
      msg(`Saved. The AP is joining "${ssid}" ...` + saved);
      const end = Date.now() + JOIN_WAIT_MS;
      while (!w.connected && Date.now() < end) {
        await sleep(JOIN_POLL_MS);
        if (serialAp() !== ap) return;
        w = await ap.wifi();
        show(w);
      }
      if (w.connected)
        msg(`Done: the AP is on "${w.ssid}" at http://${w.ip}/ (also http://openesl.local/).` + saved +
            (w.token_set ? "" : ` No API token is set yet: choose one here or at http://${w.ip}/setup.`), "ok");
      else
        msg(`Saved, but the AP has not joined "${ssid}" yet. Check the name and password; it keeps trying, and opens its setup network again if it cannot join.` + saved, "error");
    } catch (e) {
      msg("Failed: " + e.message, "error");
    } finally {
      busy(false);
    }
  }

  $("form").addEventListener("submit", (ev) => {
    ev.preventDefault();
    const ssid = $("ssid").value;
    const pass = $("pass").value;
    const token = $("token").value.trim();
    if (!ssid && !token) {
      msg("Enter the network name, a new API token, or both.", "error");
      return;
    }
    const err = (ssid ? checkWifi(ssid, pass) : null) || (token ? checkToken(token) : null);
    if (err) {
      msg(err, "error");
      return;
    }
    change(ssid || undefined, pass, token);
  });

  $("forget").addEventListener("click", () => {
    if (confirm("Forget the saved network? The AP leaves it and opens its setup network.")) change(null, "", "");
  });
}
