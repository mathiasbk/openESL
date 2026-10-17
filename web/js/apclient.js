// apclient.js - the few AP calls the designer makes (docs/openapi.yaml), through the bridge
// in tools/esl-client (`esl_client.py --serial /dev/ttyACM0 serve`, http://localhost:8080)
// or straight to an AP whose address the page may reach.

// What the user typed as the AP's address -> { base } or { error }. A bare address such as
// "192.168.3.169" gets http:// (without it fetch reads it as a path on this page's own site,
// which answers 404). A page on https may only call http://localhost (the bridge): browsers
// block plain http to anything else from it, so that is said up front.
export function apBase(input, pageProtocol = globalThis.location?.protocol) {
  let t = String(input || "").trim() || "http://localhost:8080";
  if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(t)) t = "http://" + t;
  let u;
  try {
    u = new URL(t);
  } catch {
    return { error: `"${input}" is not an address` };
  }
  if (u.protocol !== "http:" && u.protocol !== "https:") return { error: `"${input}" is not an http address` };
  const local = ["localhost", "127.0.0.1", "[::1]"].includes(u.hostname);
  if (pageProtocol === "https:" && u.protocol === "http:" && !local)
    return {
      error: `this page is on https, so the browser will not let it call ${u.origin} directly. ` +
        `Run the bridge on this computer: python3 tools/esl-client/esl_client.py --url ${u.origin} --token <token> serve, ` +
        "and connect to http://localhost:8080.",
    };
  return { base: u.origin + u.pathname.replace(/\/+$/, "") };
}

export class ApClient {
  constructor(base, token = "", fetchFn = globalThis.fetch.bind(globalThis)) {
    this.base = String(base || "").replace(/\/+$/, "");
    this.token = token;
    this.fetch = fetchFn;
  }

  async call(method, path, body) {
    const headers = {};
    if (this.token) headers.Authorization = "Bearer " + this.token;
    if (body !== undefined) headers["Content-Type"] = "application/json";
    let r;
    try {
      r = await this.fetch(this.base + path, {
        method, headers, body: body === undefined ? undefined : JSON.stringify(body),
      });
    } catch {
      throw new Error(`no answer from ${this.base}. Is the bridge running (esl_client.py --serial /dev/ttyACM0 serve)?`);
    }
    let data = null;
    try {
      data = await r.json();
    } catch { /* empty body */ }
    if (!r.ok) throw new Error(`${r.status}: ${data?.error || r.statusText || "error"}`);
    return data;
  }

  // A binary body (firmware); { status, body } with the body as bytes, as SerialAp.send
  async send(method, path, bytes) {
    const headers = { "Content-Type": "application/octet-stream" };
    if (this.token) headers.Authorization = "Bearer " + this.token;
    let r;
    try {
      r = await this.fetch(this.base + path, { method, headers, body: bytes });
    } catch {
      throw new Error(`no answer from ${this.base}. Is the bridge running (esl_client.py --serial /dev/ttyACM0 serve)?`);
    }
    return { status: r.status, body: new Uint8Array(await r.arrayBuffer()) };
  }

  // { ap, tags }
  tags() { return this.call("GET", "/api/tags"); }

  // { model, changed, version, ... }
  putLayouts(model, file) { return this.call("PUT", "/api/layouts/" + encodeURIComponent(model), file); }

  // { mac, content_id, changed, pending }
  putContent(mac, content) { return this.call("PUT", "/api/tags/" + encodeURIComponent(mac), content); }

  tag(mac) { return this.call("GET", "/api/tags/" + encodeURIComponent(mac)); }
}
