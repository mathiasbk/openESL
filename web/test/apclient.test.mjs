// The designer's AP calls (js/apclient.js) against a small fake AP.
import { test } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import { ApClient, apBase } from "../js/apclient.js";

function fakeAp() {
  const seen = [];
  const srv = http.createServer((req, res) => {
    let body = "";
    req.on("data", (c) => (body += c));
    req.on("end", () => {
      seen.push({ method: req.method, url: req.url, auth: req.headers.authorization, body: body ? JSON.parse(body) : null });
      const send = (s, o) => { res.writeHead(s, { "Content-Type": "application/json" }); res.end(JSON.stringify(o)); };
      if (req.method === "GET" && req.url === "/api/tags") send(200, { ap: { fw: "0.1.0" }, tags: [{ mac: "000002079350341C", model: "1.54" }] });
      else if (req.method === "PUT" && req.url === "/api/layouts/2.9") send(200, { model: "2.9", changed: true, version: 7 });
      else if (req.method === "PUT" && req.url.startsWith("/api/tags/")) send(422, { error: "text too long" });
      else send(404, { error: "not found" });
    });
  });
  return new Promise((ok) => srv.listen(0, "127.0.0.1", () => ok({ srv, seen, url: `http://127.0.0.1:${srv.address().port}/` })));
}

test("calls go to the right paths, with the token and JSON", async () => {
  const { srv, seen, url } = await fakeAp();
  try {
    const ap = new ApClient(url, "secret");
    assert.equal((await ap.tags()).tags[0].mac, "000002079350341C");
    assert.equal((await ap.putLayouts("2.9", { format: "esl-layouts" })).version, 7);
    assert.deepEqual(seen[1], { method: "PUT", url: "/api/layouts/2.9", auth: "Bearer secret", body: { format: "esl-layouts" } });
    await assert.rejects(ap.putContent("000002079350341C", { layout: "x", fields: [] }), /422: text too long/);
  } finally {
    srv.close();
  }
});

test("no answer gives a hint about the bridge", async () => {
  const ap = new ApClient("http://127.0.0.1:1");
  await assert.rejects(ap.tags(), /bridge/);
});

test("apBase: a bare address gets http://, https pages are sent to the bridge", () => {
  assert.deepEqual(apBase("", "https:"), { base: "http://localhost:8080" });
  assert.deepEqual(apBase("localhost:8080/", "https:"), { base: "http://localhost:8080" });
  assert.deepEqual(apBase("http://127.0.0.1:9000", "https:"), { base: "http://127.0.0.1:9000" });
  assert.deepEqual(apBase("192.168.3.169", "http:"), { base: "http://192.168.3.169" });
  assert.deepEqual(apBase(" openesl.local ", "http:"), { base: "http://openesl.local" });
  assert.match(apBase("192.168.3.169", "https:").error, /--url http:\/\/192\.168\.3\.169 .*serve/);
  assert.match(apBase("http://openesl.local", "https:").error, /localhost:8080/);
  assert.match(apBase("ftp://x", "http:").error, /not an http address/);
  assert.match(apBase("http://", "http:").error, /not an address/);
});

test("PortClient makes the same calls over the USB port", async () => {
  const { PortClient } = await import("../js/apclient.js");
  const calls = [];
  const port = {
    async call(method, path, body) {
      calls.push([method, path, body]);
      if (path === "/api/tags/X") throw new Error("422: text too long");
      return { ok: true };
    },
  };
  const c = new PortClient(port);
  assert.deepEqual(await c.tags(), { ok: true });
  await c.putLayouts("2.9", { format: "esl-layouts" });
  await assert.rejects(c.putContent("X", { layout: "price" }), /^Error: 422: text too long/);
  assert.deepEqual(calls, [["GET", "/api/tags", undefined], ["PUT", "/api/layouts/2.9", { format: "esl-layouts" }], ["PUT", "/api/tags/X", { layout: "price" }]]);
});
