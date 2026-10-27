// The weather on the Dashboard (js/apsources.js): what it sends and shows, without a page.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { relTime, sourceSummary, parseCoords, searchUrl, parseHits, placeId, weatherPlan, missingModels, placeOf } from "../js/apsources.js";
import { addTemplate } from "../js/catalog.js";

const A = "00000123456789A8", B = "00000123456789B2", C = "0000020663463415";
const oslo = { id: "oslo", type: "met", lat: 59.9139, lon: 10.7522, place: "Oslo", lang: "nb", every_min: 30, layout: "weather", fields: [], tags: [A, B], status: { ok: true } };

test("coordinates as maps copy them", () => {
  assert.deepEqual(parseCoords("59.9139, 10.7522"), { lat: 59.9139, lon: 10.7522 });
  assert.deepEqual(parseCoords(" 59.91386, 10.75224 "), { lat: 59.9139, lon: 10.7522 });
  assert.deepEqual(parseCoords("59,9139 10,7522"), { lat: 59.9139, lon: 10.7522 });
  assert.deepEqual(parseCoords("59.9139° N, 10.7522° E"), { lat: 59.9139, lon: 10.7522 });
  assert.deepEqual(parseCoords("-33.8688,151.2093"), { lat: -33.8688, lon: 151.2093 });
  assert.equal(parseCoords("Oslo"), null);
  assert.equal(parseCoords("91, 10"), null);
  assert.equal(parseCoords("59.9"), null);
});

test("a place's id from its name", () => {
  assert.equal(placeId("Tromsø", new Set()), "tromso");
  assert.equal(placeId("Ålesund sentrum", new Set()), "alesund-sentrum");
  assert.equal(placeId("Oslo", new Set(["oslo"])), "oslo-2");
  assert.equal(placeId("!!!", new Set()), "place");
  assert.equal(placeId("Kristiansand og Mandal", new Set()).length, 16);
});

test("ticked tags move to a place, and a place left without tags goes", () => {
  // to a new place: the tags leave Oslo; Oslo keeps B
  let p = weatherPlan([oslo], [A, C], { place: " Bergen ", lat: 60.3913, lon: 5.3221, lang: "en" });
  assert.equal(p.id, "bergen");
  assert.deepEqual(p.sources, [
    { id: "oslo", type: "met", lat: 59.9139, lon: 10.7522, place: "Oslo", lang: "nb", every_min: 30, layout: "weather", fields: [], tags: [B] },
    { id: "bergen", type: "met", lat: 60.3913, lon: 5.3221, place: "Bergen", lang: "en", layout: "weather", tags: [A, C] },
  ]);
  // both of Oslo's tags to Bergen: Oslo goes
  p = weatherPlan([oslo], [A, B], { place: "Bergen", lat: 60.3913, lon: 5.3221 });
  assert.deepEqual(p.sources.map((s) => s.id), ["bergen"]);
  // to a place the AP has, without status
  p = weatherPlan([oslo], [C], "oslo");
  assert.deepEqual(p.sources[0].tags, [A, B, C]);
  assert.equal(p.sources[0].status, undefined);
  // a tag already there stays once
  assert.deepEqual(weatherPlan([oslo], [A], "oslo").sources[0].tags, [B, A]);
  assert.match(weatherPlan([oslo], [C], { place: "", lat: 1, lon: 1 }).error, /name/);
  assert.match(weatherPlan([oslo], [C], { place: "X", lat: NaN, lon: 1 }).error, /Coordinates/);
  assert.match(weatherPlan([oslo], [C], "nowhere").error, /no place/);
  const many = Array.from({ length: 33 }, (_, i) => i.toString(16).padStart(16, "0"));
  assert.match(weatherPlan([], many, { place: "X", lat: 1, lon: 1 }).error, /at most 32 tags/);
  const eight = Array.from({ length: 8 }, (_, i) => ({ ...oslo, id: "p" + i, tags: [String(i).padStart(16, "1")] }));
  assert.match(weatherPlan(eight, [C], { place: "X", lat: 1, lon: 1 }).error, /at most 8 places/);
});

test("the models whose layout file lacks the weather layout", () => {
  const tags = [{ mac: A, model: "2.9" }, { mac: B, model: "1.54" }, { mac: C, model: "2.9" }, { mac: "x", model: "" }];
  assert.deepEqual(missingModels(tags, { layouts: [{ model: "2.9", layouts: ["shelf", "weather"] }] }), ["1.54"]);
  assert.deepEqual(missingModels(tags, { layouts: [{ model: "2.9", layouts: [{ name: "shelf" }] }] }), ["2.9", "1.54"]);
  assert.deepEqual(missingModels(tags, null), ["2.9", "1.54"]);
});

test("the weather template adds a layout called weather to any file", () => {
  const t = JSON.parse(readFileSync(new URL("../../templates/home/weather.json", import.meta.url)));
  for (const model of ["2.9", "1.54"]) {
    const added = addTemplate({ format: "esl-layouts", version: 1, model, layouts: [] }, t, model);
    assert.equal(added.name, "weather");
    assert.equal(Object.keys(added.doc.icons).length, 10);
  }
});

test("the place each tag shows, and a place's state in one line", () => {
  assert.deepEqual([...placeOf([oslo])], [[A, "Oslo"], [B, "Oslo"]]);
  const now = 1793030400;
  assert.equal(relTime(0, now), "never");
  assert.equal(relTime(now - 600, now), "10 min ago");
  assert.equal(relTime(now + 7200 * 2, now), "in 4 h");
  assert.deepEqual(sourceSummary({ status: { error: "HTTP 403 from api.met.no" } }, now), { text: "HTTP 403 from api.met.no", bad: true });
  assert.deepEqual(sourceSummary({ status: { fetched_at: 0 } }, now), { text: "Not fetched yet", bad: false });
  const s = sourceSummary({ status: { fetched_at: now - 120, values: { temp: "-3", desc: "Lett snø" } } }, now);
  assert.equal(s.text, "-3°, Lett snø · fetched 2 min ago");
});

test("a place found by its name", () => {
  const u = new URL(searchUrl(" Bergen ", "nb"));
  assert.equal(u.host, "nominatim.openstreetmap.org");
  assert.equal(u.searchParams.get("q"), "Bergen");
  assert.equal(u.searchParams.get("format"), "jsonv2");
  assert.equal(new URL(searchUrl("Oslo", "en")).searchParams.get("accept-language"), "en");
  // as Nominatim answers: the coordinates as strings
  const hits = parseHits([
    { lat: "60.3943055", lon: "5.3259192", name: "Bergen", display_name: "Bergen, Vestland, Norge" },
    { lat: "50.4549568", lon: "3.951958", name: "Mons", display_name: "Mons, Hainaut, Wallonie, Belgia" },
    { lat: "60.3943055", lon: "5.3259192", name: "Bergen", display_name: "Bergen, Vestland, Norge" },
    { lat: "x", lon: "1", name: "Bad" },
    { lat: "1", lon: "1", name: "", display_name: "" },
  ]);
  assert.deepEqual(hits, [
    { name: "Bergen", label: "Bergen, Vestland, Norge", lat: 60.3943, lon: 5.3259 },
    { name: "Mons", label: "Mons, Hainaut, Wallonie, Belgia", lat: 50.455, lon: 3.952 },
  ]);
  assert.deepEqual(parseHits({ error: "x" }), []);
});
