// A simulated AP with the calls the backup uses (docs/openapi.yaml), for backup.test.mjs and
// the compatibility test with the backups of earlier releases (backup_compat.test.mjs, compat/).
export const PIC = Buffer.from(Uint8Array.from({ length: 16 }, (_, i) => i)).toString("base64");

export class FakeAp {
  constructor({ clock = true, webhook = true } = {}) {
    this.clock = clock;
    this.hasWebhook = webhook;
    this.config = { poll_s: 0, builtin_poll_s: 0, webhook: null };
    if (clock) Object.assign(this.config, { ntp: "pool.ntp.org", builtin_ntp: "pool.ntp.org", tz: null });
    this.layouts = {};   // model -> file
    this.tags = new Map(); // mac -> { mac, model, content, config }
    this.scheduled = [];
    this.time = 1792065600;
    this.calls = [];
    this.fonts = null;   // name -> font file; null: an AP without font files
    this.fw = "0.8.1";   // the AP's firmware, as GET /api/tags gives it
  }
  tag(mac) {
    if (!this.tags.has(mac)) this.tags.set(mac, { mac, model: null, content: null, config: {} });
    return this.tags.get(mac);
  }
  check(c) {
    // a layout file as the AP takes it (a list of layouts with names), or shortened to { name: ... }
    const has = (f) => (Array.isArray(f.layouts) ? f.layouts.some((l) => l.name === c.layout) : f.layouts?.[c.layout]);
    if (c.layout !== "text" && c.layout !== "price" && c.layout !== "image" && !Object.values(this.layouts).some(has))
      throw new Error("422: layout not in the file");
  }
  setContent(mac, c) {
    this.check(c);
    const { at, ...content } = c;
    if (at && at > this.time) this.scheduled.push({ mac, at, ...content });
    else this.tag(mac).content = content;
  }
  async call(method, path, body) {
    this.calls.push(method + " " + path);
    let m;
    if (method === "GET" && path === "/api/tags")
      return {
        ap: { fw: this.fw, board: "s3-n16r8-c6", board_name: "ESP32-S3 + C6 on cable", channel: 25, time: this.time, poll_s: this.config.poll_s },
        tags: [...this.tags.values()].map((t) => ({ mac: t.mac, model: t.model, content_id: t.content ? 7 : 0,
          config: { poll_s: null, max_poll_s: null, lost_ap_s: null, long_poll_s: null, long_after_min: null, ...t.config } })),
      };
    if (method === "GET" && path === "/api/config") return { ...this.config };
    if (method === "GET" && path === "/api/health" && this.health) return this.health;
    if (method === "PUT" && path === "/api/config") {
      for (const k of Object.keys(body)) {
        if (!(k in this.config) || k.startsWith("builtin")) throw new Error("422: unknown");
        if (k === "webhook" && !this.hasWebhook) throw new Error("422: this AP has no webhook");
      }
      for (const [k, v] of Object.entries(body)) this.config[k] = v === null && "builtin_" + k in this.config ? this.config["builtin_" + k] : v;
      return { ...this.config };
    }
    if (method === "GET" && path === "/api/wifi") return { hostname: "openesl", ssid: "home" };
    if (method === "PUT" && path === "/api/time") return {};
    if (method === "GET" && path === "/api/layouts")
      return { layouts: ["2.9", "1.54"].map((model) => ({ model, version: this.layouts[model] ? 1 : null })) };
    if ((m = path.match(/^\/api\/layouts\/(.+)$/))) {
      const model = decodeURIComponent(m[1]);
      if (method === "GET") return this.layouts[model];
      if (body.fonts?.some((f) => !this.fonts?.[f])) throw new Error("422: unknown font");
      this.layouts[model] = body;
      return { model, changed: true };
    }
    if (this.fonts && method === "GET" && path === "/api/fonts")
      return { fonts: Object.keys(this.fonts).map((name, slot) => ({ name, slot, used_by: [] })) };
    if (this.fonts && (m = path.match(/^\/api\/fonts\/(.+)$/))) {
      const name = decodeURIComponent(m[1]);
      if (method === "GET") return this.fonts[name];
      this.fonts[name] = body;
      return { font: { name }, replaced: false, redrawn: [] };
    }
    if (method === "GET" && path === "/api/tag-firmware")
      return { images: [{ model: "2.9", version: 82, crc: 1234, tags: { pending: 1, failed: 0, done: 3 } }, { model: "1.54", version: null }] };
    if (method === "GET" && path === "/api/schedule") return { time: this.time, scheduled: this.scheduled };
    if (method === "PUT" && path === "/api/tags") {
      body.tags.forEach((t) => this.check(t)); // all or nothing, as the AP
      for (const { mac, ...c } of body.tags) this.setContent(mac, c);
      return { results: [] };
    }
    if ((m = path.match(/^\/api\/tags\/([0-9A-F]{16})\/config$/)) && method === "PUT") {
      this.tag(m[1]).config = { ...body };
      return {};
    }
    if ((m = path.match(/^\/api\/tags\/([0-9A-F]{16})\/image$/)) && method === "GET") {
      const t = this.tags.get(m[1]);
      return { mac: t.mac, width: 4, height: 32, red: false, data: t.content.picture };
    }
    if ((m = path.match(/^\/api\/tags\/([0-9A-F]{16})$/))) {
      if (method === "GET") {
        const t = this.tags.get(m[1]);
        return { mac: t.mac, fields: [], price_highlight: false, frame: false, ...t.content, picture: undefined };
      }
      this.setContent(m[1], body);
      return {};
    }
    throw new Error("404: " + method + " " + path);
  }
  async send(method, path, bytes) {
    this.calls.push(method + " " + path);
    const m = path.match(/^\/api\/tags\/([0-9A-F]{16})\/image$/);
    const t = this.tags.get(m[1]);
    if (!t?.model) return { status: 409, body: new TextEncoder().encode('{"error":"model not known"}') };
    t.content = { layout: "image", fields: [], picture: Buffer.from(bytes).toString("base64") };
    return { status: 200, body: new TextEncoder().encode("{}") };
  }
}

export function filledAp() {
  const ap = new FakeAp();
  ap.config = { ...ap.config, poll_s: 120, webhook: "http://hook.local/", tz: "CET-1CEST,M3.5.0,M10.5.0/3" };
  ap.layouts["2.9"] = { layouts: { shelf: { fields: 3 } } };
  Object.assign(ap.tag("0000000000000001"), { model: "2.9", content: { layout: "shelf", fields: ["Milk", "24.90", "1 L"], price_highlight: true } });
  Object.assign(ap.tag("0000000000000002"), { model: "2.9", content: { layout: "image", fields: [], picture: PIC } });
  Object.assign(ap.tag("0000000000000003"), { model: "1.54", config: { poll_s: 600 } });
  ap.tag("0000000000000004").model = "1.54"; // nothing of its own: not in the backup
  ap.scheduled.push({ mac: "0000000000000001", at: ap.time + 3600, layout: "shelf", fields: ["Milk", "19.90", "Offer"], price_highlight: true, frame: false });
  return ap;
}
