// apserial.js - the AP's HTTP API over its USB port (ap/main/serial_api.c), from the browser
// with Web Serial. A port of SerialClient in tools/esl-client/esl_client.py:
//
//   page -> AP:  @ESL <seq> <METHOD> <uri> <len>\n<len bytes body>
//   AP -> page:  @ESL <seq> <status> <len>[+] <len bytes body>\n
//
// The port also carries the AP's log, so everything that is not the reply to the request
// just sent is skipped. A reply with "+" after its length is one piece of a longer one.
// The USB port needs no API token.
//
// The transport is the one in js/zbs.js: write(Uint8Array), read(n, timeoutMs), flushInput().

const enc = new TextEncoder();
const dec = new TextDecoder();

function indexOf(hay, needle) {
  outer: for (let i = 0; i + needle.length <= hay.length; i++) {
    for (let j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
    return i;
  }
  return -1;
}

export class ApSerialError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

export class SerialAp {
  constructor(transport, { timeoutMs = 10000 } = {}) {
    this.t = transport;
    this.timeoutMs = timeoutMs;
    this.seq = 0;
    this.buf = new Uint8Array(0);
  }

  // Reads more from the port into buf, or throws on timeout
  async fill(end) {
    const left = end - Date.now();
    const got = left > 0 ? await this.t.read(4096, left) : new Uint8Array(0);
    if (!got.length) throw new ApSerialError(0, "no answer from the AP on the USB port");
    const b = new Uint8Array(this.buf.length + got.length);
    b.set(this.buf);
    b.set(got, this.buf.length);
    this.buf = b;
  }

  // Exactly n bytes
  async readBytes(n, end) {
    while (this.buf.length < n) await this.fill(end);
    const out = this.buf.slice(0, n);
    this.buf = this.buf.slice(n);
    return out;
  }

  // Up to and including the next newline
  async readLine(end) {
    let i;
    while ((i = this.buf.indexOf(10)) < 0) await this.fill(end);
    return this.readBytes(i + 1, end);
  }

  // { status, body } with the body as bytes; throws only when no reply comes
  async send(method, path, body = new Uint8Array(0), timeoutMs = this.timeoutMs) {
    if (typeof body === "string") body = enc.encode(body);
    const seq = ++this.seq;
    await this.t.flushInput();
    this.buf = new Uint8Array(0);
    const head = enc.encode(`@ESL ${seq} ${method} ${path} ${body.length}\n`);
    const frame = new Uint8Array(head.length + body.length);
    frame.set(head);
    frame.set(body, head.length);
    await this.t.write(frame);

    const end = Date.now() + timeoutMs;
    const prefix = enc.encode(`@ESL ${seq} `);
    const parts = [];
    for (;;) {
      // A header line: "@ESL <seq> <status> <len>[+] " and the start of the body
      const line = await this.readLine(end);
      const at = indexOf(line, prefix);
      if (at < 0) continue;
      let p = at + prefix.length;
      const field = () => {
        const s = p;
        while (p < line.length && line[p] !== 32 && line[p] !== 10) p++;
        return dec.decode(line.subarray(s, p++));
      };
      const status = Number(field());
      const len = field();
      const more = len.endsWith("+");
      const length = Number(more ? len.slice(0, -1) : len);
      if (!Number.isInteger(status) || !Number.isInteger(length) || length < 0) continue;
      // The body may hold newlines: put back what the line took past the header, then
      // take exactly length bytes and the closing newline
      const rest = line.subarray(Math.min(p, line.length));
      const b = new Uint8Array(rest.length + this.buf.length);
      b.set(rest);
      b.set(this.buf, rest.length);
      this.buf = b;
      parts.push((await this.readBytes(length + 1, end)).subarray(0, length));
      if (!more) {
        const all = new Uint8Array(parts.reduce((n, x) => n + x.length, 0));
        let o = 0;
        for (const x of parts) {
          all.set(x, o);
          o += x.length;
        }
        return { status, body: all };
      }
    }
  }

  // The parsed JSON reply; a status of 400 or more throws with the AP's error text
  async call(method, path, data, timeoutMs) {
    const r = await this.send(method, path, data === undefined ? undefined : JSON.stringify(data), timeoutMs);
    let json = null;
    try {
      json = JSON.parse(dec.decode(r.body));
    } catch {
      /* empty or not JSON */
    }
    if (r.status >= 400) throw new ApSerialError(r.status, `${r.status}: ${json?.error || "error"}`);
    return json;
  }

  wifi(timeoutMs) {
    return this.call("GET", "/api/wifi", undefined, timeoutMs);
  }

  setWifi(ssid, password = "") {
    return this.call("PUT", "/api/wifi", { ssid, password });
  }

  // Opening the port can restart the AP, which then takes a few seconds to answer:
  // asks for the WiFi state until it does
  async waitReady({ tries = 6, timeoutMs = 2500 } = {}) {
    for (let i = 0; i < tries; i++) {
      try {
        return await this.wifi(timeoutMs);
      } catch (e) {
        if (e.status) throw e;
      }
    }
    throw new ApSerialError(0, "the AP does not answer on this port. Is it the AP's USB port, running OpenESL, and is no other program (esl_client.py) using it?");
  }
}

// One line about the WiFi state, as esl_client.py's print_wifi
export function describeWifi(w) {
  let s;
  if (w.connected) s = `Connected to "${w.ssid}", IP ${w.ip}` + (w.rssi != null ? `, ${w.rssi} dBm` : "");
  else if (w.ssid) s = `Trying to join "${w.ssid}"`;
  else s = "No network saved";
  if (w.mode === "off") s += " (WiFi did not start)";
  if (w.setup_ssid) s += `. Setup network "${w.setup_ssid}" is open`;
  return s + ".";
}

// The same checks as esl_wifi_check() on the AP, so the form can say what is wrong first
export function checkWifi(ssid, password) {
  if (!ssid) return "Enter the network name.";
  if (enc.encode(ssid).length > 32) return "The network name is longer than 32 bytes.";
  if (password && (password.length < 8 || password.length > 63)) return "The password must be empty (open network) or 8 to 63 characters.";
  if (/[^\x20-\x7e]/.test(password)) return "The password must be printable ASCII.";
  return null;
}
