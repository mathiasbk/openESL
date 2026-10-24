// apport.js - the page's one connection to the AP's USB port (Web Serial), shared by
// "Set the WiFi over USB" and the dashboard: a port can be open in one place only.
import { WebSerialTransport } from "./zbs.js";
import { SerialAp } from "./apserial.js";

let transport = null;
let ap = null;
const listeners = new Set();

const notify = () => listeners.forEach((f) => f(ap));

export const hasSerial = () => "serial" in navigator;

// The connected SerialAp, or null
export const serialAp = () => ap;

// f(ap or null) whenever the connection opens or closes
export function onSerialChange(f) {
  listeners.add(f);
}

// Asks the user for the port, opens it and waits until the AP answers
export async function connectSerial() {
  if (ap) return ap;
  return connectSerialPort(await navigator.serial.requestPort());
}

// The same with a port the page already has (the one the Flash AP tab just flashed)
export async function connectSerialPort(port) {
  if (ap) return ap;
  // DTR and RTS stay as the port opened them, as with esl_client.py
  transport = await WebSerialTransport.open(port, { lowSignals: false });
  const a = new SerialAp(transport);
  try {
    await a.waitReady();
  } catch (e) {
    await transport.close().catch(() => {});
    transport = null;
    throw e;
  }
  ap = a;
  notify();
  return ap;
}

export async function disconnectSerial() {
  if (!transport) return;
  const t = transport;
  transport = ap = null;
  await t.close().catch(() => {});
  notify();
}

if (hasSerial()) {
  navigator.serial.addEventListener("disconnect", (ev) => {
    if (transport && ev.target === transport.port) disconnectSerial();
  });
}

// The port of the open connection, or null
export const serialPort = () => transport?.port || null;
