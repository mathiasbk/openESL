// radioconsole.js - sets up the radio over WiFi (ap/netradio/) from the Flash AP tab, over its
// USB console with Web Serial: ssid, pass and host lines, a restart, then its log until it says
// it is connected to the AP (docs/ap-netradio.md).
import { WebSerialTransport } from "./zbs.js";
import { radioCommands, radioLine } from "./apsteps.js";
import { portAgain } from "./apflash.js";

const enc = new TextEncoder();
const dec = new TextDecoder();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Reads lines from t until done(line) returns something or ms pass; null on timeout
async function readLines(t, ms, done, onLine) {
  const end = Date.now() + ms;
  let rest = "";
  while (Date.now() < end) {
    const got = await t.read(4096, Math.min(1000, end - Date.now()));
    if (!got.length) {
      if (t.closed || !t.port.readable) return null;
      continue;
    }
    rest += dec.decode(got, { stream: true });
    const lines = rest.split(/\r?\n/);
    rest = lines.pop();
    for (const raw of lines) {
      // strip the colour codes of the ESP-IDF log
      const line = raw.replace(/\x1b\[[0-9;]*m/g, "").trim();
      if (!line) continue;
      onLine(line);
      const r = done(line);
      if (r) return r;
    }
  }
  return null;
}

// Sends the radio its network and the AP's address, restarts it and watches it join.
// Returns { state: "joined", ip?, rssi? }, or { state: "no-ap" | "no-wifi" | "timeout" }.
// port is closed again afterwards (it is the one returned in .port, after the restart).
export async function setUpRadio(port, cfg, { onLine = () => {}, waitMs = 90000 } = {}) {
  let t = await WebSerialTransport.open(port, { lowSignals: false });
  try {
    for (const line of radioCommands(cfg)) {
      await t.write(enc.encode(line + "\n"));
      await sleep(300);
    }
  } finally {
    await t.close().catch(() => {});
  }
  // the restart takes the chip's own USB away for a moment
  const again = await portAgain(port, 10000);
  if (!again) return { state: "lost", port };
  t = await WebSerialTransport.open(again, { lowSignals: false });
  try {
    let last = null;
    let r = await readLines(t, waitMs, (line) => {
      const s = radioLine(line);
      if (s) last = s;
      return s?.state === "joined" ? s : null;
    }, onLine);
    if (r) {
      // the signal at the radio, from its status line
      await t.write(enc.encode("status\n"));
      const st = await readLines(t, 3000, (line) => radioLine(line)?.rssi != null ? radioLine(line) : null, onLine);
      if (st?.rssi != null) r = { ...r, rssi: st.rssi };
    }
    return { ...(r || (last?.state && last.state !== "wifi" ? last : { state: "timeout" })), port: again };
  } finally {
    await t.close().catch(() => {});
  }
}
