import { test } from "node:test";
import assert from "node:assert/strict";
import { canon, compareLayouts, tagLayouts, countByLayout } from "../js/layoutinfo.js";

test("canon ignores key order", () => {
  assert.equal(canon({ b: 1, a: [{ y: 2, x: 1 }] }), canon({ a: [{ x: 1, y: 2 }], b: 1 }));
  assert.notEqual(canon({ a: 1 }), canon({ a: 2 }));
});

test("compareLayouts tells the AP's, changed, new and gone layouts apart", () => {
  const ap = { layouts: [{ name: "shelf", fields: ["a"] }, { name: "sign", fields: ["b"] }, { name: "old", fields: [] }] };
  const mine = { layouts: [{ fields: ["a"], name: "shelf" }, { name: "sign", fields: ["c"] }, { name: "new", fields: [] }] };
  assert.deepEqual(compareLayouts(mine, ap), { states: { shelf: "ap", sign: "changed", new: "new" }, gone: ["old"] });
  assert.deepEqual(compareLayouts(mine, null).states, { shelf: "new", sign: "new", new: "new" });
});

test("tagLayouts asks only for tags it does not know, and countByLayout counts by model", async () => {
  const asked = [];
  const client = { call: async (m, path) => { const mac = path.split("/").pop(); asked.push(mac); if (mac === "C") throw new Error("404"); return mac === "B" ? {} : { layout: "shelf" }; } };
  const tags = [{ mac: "A", model: "2.9" }, { mac: "B", model: "2.9" }, { mac: "C", model: "2.9" }, { mac: "D", model: "1.54" }];
  const m = await tagLayouts(client, tags, { D: "shelf" });
  assert.deepEqual(asked.sort(), ["A", "B", "C"]);
  assert.deepEqual(m, { D: "shelf", A: "shelf", B: "" });
  assert.deepEqual(countByLayout(tags, m, "2.9"), { shelf: 1 });
});

test("tagLayouts asks every tag by default, also past 200 (#302)", async () => {
  const client = { call: async (m, path) => ({ layout: path.endsWith("/T266") ? "rare" : "shelf" }) };
  const tags = Array.from({ length: 267 }, (_, i) => ({ mac: "T" + i, model: "2.9" }));
  const m = await tagLayouts(client, tags);
  assert.equal(Object.keys(m).length, 267);
  assert.deepEqual(countByLayout(tags, m, "2.9"), { shelf: 266, rare: 1 });
  assert.equal(Object.keys(await tagLayouts(client, tags, {}, { max: 10 })).length, 10);
});
