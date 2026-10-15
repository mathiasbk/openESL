// The designer's AP calls (js/apclient.js) against a small fake AP.
import { test } from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import { ApClient } from "../js/apclient.js";

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
