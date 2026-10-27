// The weather on the Dashboard (js/apsources.js): what it sends and shows, without a page.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { relTime, sourceSummary, parseCoords, placeUrls, parseNorway, parseWorld, mergePlaces, placeId, weatherPlan, leavePlan, missingModels, placeOf } from "../js/apsources.js";
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

test("tags sent other content leave their places", () => {
  const bergen = { id: "bergen", type: "met", lat: 60.39, lon: 5.32, place: "Bergen", layout: "weather", tags: [C] };
  assert.equal(leavePlan([oslo, bergen], ["x"]), null);
  assert.equal(leavePlan(null, [A]), null);
  // A leaves Oslo (B stays); C leaves Bergen, which goes; no status sent back
  assert.deepEqual(leavePlan([oslo, bergen], [A, C]), {
    sources: [{ id: "oslo", type: "met", lat: 59.9139, lon: 10.7522, place: "Oslo", lang: "nb", every_min: 30, layout: "weather", fields: [], tags: [B] }],
    places: ["Oslo", "Bergen"],
  });
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

test("places suggested as you type", () => {
  const u = placeUrls(" Berg* ", "nb");
  const no = new URL(u.no), wu = new URL(u.world);
  assert.equal(no.host, "ws.geonorge.no");
  assert.equal(no.searchParams.get("sok"), "Berg*");
  assert.equal(wu.host, "geocoding-api.open-meteo.com");
  assert.equal(wu.searchParams.get("name"), "Berg");
  assert.equal(new URL(placeUrls("Oslo", "en").world).searchParams.get("language"), "en");
  // as Kartverket answers "Trom*"
  const norway = parseNorway({ navn: [
    { skrivemåte: "Troms", navneobjekttype: "Fylke", representasjonspunkt: { nord: 69.65278, "øst": 18.94419 }, fylker: [{ fylkesnavn: "Troms - Romsa - Tromssa" }], kommuner: [{ kommunenavn: "Tromsø" }, { kommunenavn: "Harstad - Hárstták" }] },
    { skrivemåte: "Tromsa", navneobjekttype: "Øy i sjø", representasjonspunkt: { nord: 63.7, "øst": 8.8 }, fylker: [{ fylkesnavn: "Trøndelag - Trööndelage" }], kommuner: [{ kommunenavn: "Frøya" }] },
    { skrivemåte: "Tromsø", navneobjekttype: "By", representasjonspunkt: { nord: 69.65455, "øst": 18.96366 }, fylker: [{ fylkesnavn: "Troms - Romsa - Tromssa" }], kommuner: [{ kommunenavn: "Tromsø" }] },
    { skrivemåte: "Tromvegen", navneobjekttype: "Veg", representasjonspunkt: { nord: 63.3, "øst": 10.0 } },
  ] }, "Trom");
  assert.deepEqual(norway, [
    { name: "Tromsø", detail: "Troms", lat: 69.6546, lon: 18.9637, place: true },
    { name: "Tromsa", detail: "Frøya, Trøndelag", lat: 63.7, lon: 8.8, place: false },
  ]);
  // the name typed in full first, among the towns and villages
  const two = parseNorway({ navn: [
    { skrivemåte: "Bergen", navneobjekttype: "By", representasjonspunkt: { nord: 60.39, "øst": 5.32 } },
    { skrivemåte: "Berg", navneobjekttype: "Grend", representasjonspunkt: { nord: 59.1, "øst": 11.3 } },
  ] }, "berg");
  assert.deepEqual(two.map((h) => h.name), ["Berg", "Bergen"]);
  assert.deepEqual(parseNorway({}), []);
  // as Open-Meteo answers "Bergen"
  const world = parseWorld({ results: [
    { name: "Bergen", latitude: 60.39299, longitude: 5.32415, admin1: "Vestland", country: "Norge" },
    { name: "Bergen", latitude: 52.81, longitude: 9.96, admin1: "Niedersachsen", country: "Tyskland" },
    { name: "Bad", latitude: "x", longitude: 1 },
  ] });
  assert.equal(world.length, 2);
  assert.deepEqual(parseWorld({}), []);
  const bergen = { name: "Bergen", detail: "Vestland", lat: 60.3932, lon: 5.3245, place: true };
  const bergsoya = { name: "Bergsøya", detail: "Gjemnes, Møre og Romsdal", lat: 62.9, lon: 7.7, place: false };
  // Bergen once (Kartverket's and GeoNames' points are a few hundred metres apart); the world's
  // Bergen before Norway's islands
  let m = mergePlaces("Bergen", [bergen, bergsoya], world);
  assert.deepEqual(m.map((h) => h.detail), ["Vestland", "Niedersachsen, Tyskland", "Gjemnes, Møre og Romsdal"]);
  assert.deepEqual(Object.keys(m[0]), ["name", "detail", "lat", "lon"]);
  // while typing: Norway's Bergen before a Berg in Belgium
  const belgium = { name: "Berg", detail: "Flanders, Belgia", lat: 50.93, lon: 4.54 };
  m = mergePlaces("Berg", [bergen, bergsoya], [belgium]);
  assert.deepEqual(m.map((h) => h.name), ["Bergen", "Berg", "Bergsøya"]);
  assert.equal(mergePlaces("berg", [bergen, bergsoya], [belgium], 1).length, 1);
});
