// The data sources card (js/apsources.js): what it sends and shows, without a page.
import test from "node:test";
import assert from "node:assert/strict";
import { relTime, sourceSummary, formSource, configOf } from "../js/apsources.js";

const DEFAULTS = ["{place}", "{temp}", "{symbol}", "{desc}", "{min}° / {max}°", "{wind} m/s  {rain} mm"];
const form = (o = {}) => ({
  id: "Home", place: " Oslo ", lat: "59,9139", lon: "10.7522", lang: "nb", every: "30", layout: "weather",
  tags: ["00000123456789A8"], fields: DEFAULTS.concat([""]).slice(0, 6), ...o,
});

test("the form becomes a source as PUT /api/sources takes it", () => {
  assert.deepEqual(formSource(form(), DEFAULTS), {
    id: "home", type: "met", lat: 59.9139, lon: 10.7522, place: "Oslo", lang: "nb", every_min: 30,
    layout: "weather", tags: ["00000123456789A8"],
  });
  // own fields are sent, the defaults are not
  const own = formSource(form({ fields: ["{temp}°", "{desc}", "", "", "", ""] }), DEFAULTS);
  assert.deepEqual(own.fields, ["{temp}°", "{desc}"]);
  assert.match(formSource(form({ id: "hjem me" }), DEFAULTS).error, /^Name/);
  assert.match(formSource(form({ lat: "" }), DEFAULTS).error, /^Latitude/);
  assert.match(formSource(form({ lat: "91" }), DEFAULTS).error, /^Latitude/);
  assert.match(formSource(form({ lon: "x" }), DEFAULTS).error, /^Longitude/);
  assert.match(formSource(form({ layout: "" }), DEFAULTS).error, /layout/);
});

test("the configuration leaves each source's status out", () => {
  assert.deepEqual(configOf([{ id: "a", status: { ok: true } }]), { sources: [{ id: "a" }] });
});

test("a source's state in one line", () => {
  const now = 1793030400;
  assert.equal(relTime(0, now), "never");
  assert.equal(relTime(now - 600, now), "10 min ago");
  assert.equal(relTime(now + 7200 * 2, now), "in 4 h");
  assert.deepEqual(sourceSummary({ status: { error: "HTTP 403 from api.met.no" } }, now), { text: "HTTP 403 from api.met.no", bad: true });
  assert.deepEqual(sourceSummary({ status: { fetched_at: 0 } }, now), { text: "Not fetched yet", bad: false });
  const s = sourceSummary({ status: { fetched_at: now - 120, tags_set: 2, tags_changed: 1, values: { temp: "-3", desc: "Lett snø" } } }, now);
  assert.equal(s.text, "-3°, Lett snø · fetched 2 min ago · 2 tag(s), 1 changed");
});
