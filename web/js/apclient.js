// apclient.js - the few AP calls the designer makes (docs/openapi.yaml), through the bridge
// in tools/esl-client (`esl_client.py --serial /dev/ttyACM0 serve`, http://localhost:8080)
// or straight to an AP whose address the page may reach.

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

  // { ap, tags }
  tags() { return this.call("GET", "/api/tags"); }

  // { model, changed, version, ... }
  putLayouts(model, file) { return this.call("PUT", "/api/layouts/" + encodeURIComponent(model), file); }

  // { mac, content_id, changed, pending }
  putContent(mac, content) { return this.call("PUT", "/api/tags/" + encodeURIComponent(mac), content); }

  tag(mac) { return this.call("GET", "/api/tags/" + encodeURIComponent(mac)); }
}
