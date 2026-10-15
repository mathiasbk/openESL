// wifisetup.js - "Set the WiFi over USB" on the Set up tab: GET and PUT /api/wifi over the
// AP's USB port with Web Serial (js/apserial.js), for when the setup network is no help.
import { WebSerialTransport } from "./zbs.js";
import { SerialAp, describeWifi, checkWifi } from "./apserial.js";

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const JOIN_WAIT_MS = 30000;   // how long the page watches the AP join after a change
const JOIN_POLL_MS = 2000;

export function initWifiSetup() {
  const btn = $("wifi-connect");
  if (!("serial" in navigator)) {
    $("wifi-serial-missing").hidden = false;
    btn.disabled = true;
    return;
  }

  let transport = null;
  let ap = null;
  let queue = Promise.resolve();
  // One request at a time on the port
  const run = (f) => (queue = queue.then(f, f));

  const msg = (text, cls = "") => {
    $("wifi-msg").textContent = text;
    $("wifi-msg").className = "hint" + (cls ? " " + cls : "");
  };
  const busy = (on) => {
    for (const id of ["wifi-save", "wifi-forget", "wifi-connect"]) $(id).disabled = on;
  };

  const show = (w) => {
    $("wifi-state").textContent = describeWifi(w);
    const list = $("wifi-networks");
    list.replaceChildren(...(w.networks || []).map((n) => {
      const o = document.createElement("option");
      o.value = n.ssid;
      o.label = `${n.rssi} dBm`;
      return o;
    }));
    if (!$("wifi-ssid").value && w.ssid) $("wifi-ssid").value = w.ssid;
  };

  const closed = async () => {
    if (transport) await transport.close().catch(() => {});
    transport = ap = null;
    btn.textContent = "Connect AP";
    $("wifi-form").hidden = true;
    $("wifi-state").textContent = "";
  };

  btn.addEventListener("click", async () => {
    if (transport) {
      await closed();
      msg("Disconnected.");
      return;
    }
    let port;
    try {
      port = await navigator.serial.requestPort();
    } catch {
      return; // no port chosen
    }
    busy(true);
    try {
      // DTR and RTS stay as the port opened them, as with esl_client.py
      transport = await WebSerialTransport.open(port, { lowSignals: false });
      ap = new SerialAp(transport);
      msg("Port open, waiting for the AP ...");
      const w = await run(() => ap.waitReady());
      show(w);
      btn.textContent = "Disconnect";
      $("wifi-form").hidden = false;
      msg(w.networks?.length ? "Pick a network from the list or type its name." : "Type the network's name (the AP has no scan list).");
    } catch (e) {
      msg("Could not connect: " + e.message, "error");
      await closed();
    } finally {
      busy(false);
    }
  });

  navigator.serial.addEventListener("disconnect", (ev) => {
    if (transport && ev.target === transport.port) {
      closed();
      msg("The AP was unplugged.", "error");
    }
  });

  $("wifi-show").addEventListener("change", (ev) => {
    $("wifi-pass").type = ev.target.checked ? "text" : "password";
  });

  // After a change: watch the AP until it has joined, or give up after JOIN_WAIT_MS
  async function change(ssid, password) {
    busy(true);
    try {
      let w = await run(() => ap.setWifi(ssid, password));
      show(w);
      if (!ssid) {
        msg("The AP forgot the network and opened its setup network.", "ok");
        return;
      }
      msg(`Saved. The AP is joining "${ssid}" ...`);
      const end = Date.now() + JOIN_WAIT_MS;
      while (!w.connected && Date.now() < end && ap) {
        await sleep(JOIN_POLL_MS);
        if (!ap) return;
        w = await run(() => ap.wifi());
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
