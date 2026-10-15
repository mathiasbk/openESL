// wifisetup.js - "Set the WiFi over USB" on the Set up tab: GET and PUT /api/wifi over the
// AP's USB port with Web Serial (js/apserial.js), for when the setup network is no help.
import { describeWifi, checkWifi } from "./apserial.js";
import { hasSerial, serialAp, connectSerial, disconnectSerial, onSerialChange } from "./apport.js";

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const JOIN_WAIT_MS = 30000;   // how long the page watches the AP join after a change
const JOIN_POLL_MS = 2000;

export function initWifiSetup() {
  const btn = $("wifi-connect");
  if (!hasSerial()) {
    $("wifi-serial-missing").hidden = false;
    btn.disabled = true;
    return;
  }

  const msg = (text, cls = "") => {
    $("wifi-msg").textContent = text;
    $("wifi-msg").className = "hint" + (cls ? " " + cls : "");
  };
  const busy = (on) => {
    for (const id of ["wifi-save", "wifi-forget", "wifi-connect"]) $(id).disabled = on;
  };

  const show = (w) => {
    $("wifi-state").textContent = describeWifi(w);
    $("wifi-networks").replaceChildren(...(w.networks || []).map((n) => {
      const o = document.createElement("option");
      o.value = n.ssid;
      o.label = `${n.rssi} dBm`;
      return o;
    }));
    if (!$("wifi-ssid").value && w.ssid) $("wifi-ssid").value = w.ssid;
  };

  // The port opened or closed, here or on the dashboard
  onSerialChange(async (ap) => {
    btn.textContent = ap ? "Disconnect" : "Connect AP";
    $("wifi-form").hidden = !ap;
    if (!ap) {
      $("wifi-state").textContent = "";
      msg("Disconnected.");
      return;
    }
    try {
      const w = await ap.wifi();
      show(w);
      msg(w.networks?.length ? "Pick a network from the list or type its name." : "Type the network's name (the AP has no scan list).");
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

  $("wifi-show").addEventListener("change", (ev) => {
    $("wifi-pass").type = ev.target.checked ? "text" : "password";
  });

  // After a change: watch the AP until it has joined, or give up after JOIN_WAIT_MS
  async function change(ssid, password) {
    const ap = serialAp();
    if (!ap) return;
    busy(true);
    try {
      let w = await ap.setWifi(ssid, password);
      show(w);
      if (!ssid) {
        msg("The AP forgot the network and opened its setup network.", "ok");
        return;
      }
      msg(`Saved. The AP is joining "${ssid}" ...`);
      const end = Date.now() + JOIN_WAIT_MS;
      while (!w.connected && Date.now() < end) {
        await sleep(JOIN_POLL_MS);
        if (serialAp() !== ap) return;
        w = await ap.wifi();
        show(w);
      }
      if (w.connected)
        msg(`Done: the AP is on "${w.ssid}" at http://${w.ip}/ (also http://openesl.local/). If no API token is set yet, choose one at http://${w.ip}/setup.`, "ok");
      else
        msg(`Saved, but the AP has not joined "${ssid}" yet. Check the name and password; it keeps trying, and opens its setup network again if it cannot join.`, "error");
    } catch (e) {
      msg("Failed: " + e.message, "error");
    } finally {
      busy(false);
    }
  }

  $("wifi-form").addEventListener("submit", (ev) => {
    ev.preventDefault();
    const ssid = $("wifi-ssid").value;
    const pass = $("wifi-pass").value;
    const err = checkWifi(ssid, pass);
    if (err) {
      msg(err, "error");
      return;
    }
    change(ssid, pass);
  });

  $("wifi-forget").addEventListener("click", () => {
    if (confirm("Forget the saved network? The AP leaves it and opens its setup network.")) change(null, "");
  });
}
