// apsources.js - the weather on the Dashboard (ESP32-S3 and ESP32-C5 APs, docs/data-sources.md):
// "The weather" in the Send form for the ticked tags, with a dialog to choose the place, and the
// "Weather" card listing the places the AP fetches (GET/PUT /api/sources, POST
// /api/sources/refresh). A place is a data source: the AP fetches its forecast from MET Norway
// and puts it on its tags in the layout "weather" (templates/home/weather.json), which the dialog
// offers to add to the AP's layout file of a model that lacks it. On an AP without data sources
// (404) the choice and the card stay hidden.

import { addTemplate } from "./catalog.js";

const $ = (id) => document.getElementById(id);
export const WEATHER_LAYOUT = "weather";
const TAGS_MAX = 32;   // ESL_SOURCE_TAGS_MAX
const SOURCES_MAX = 8; // ESL_SOURCE_MAX

// "12 min ago" / "in 5 min" from Unix seconds against now (s)
export function relTime(t, now) {
  if (!t) return "never";
  const d = Math.round((t - now) / 60);
  if (Math.abs(d) < 1) return "now";
  const n = Math.abs(d) < 120 ? `${Math.abs(d)} min` : `${Math.round(Math.abs(d) / 60)} h`;
  return d < 0 ? `${n} ago` : `in ${n}`;
}

// One line on a source's state: what it shows and when it last fetched, or what went wrong
export function sourceSummary(s, now) {
  const st = s.status || {};
  const v = st.values || {};
  if (st.error) return { text: st.error, bad: true };
  if (!st.fetched_at) return { text: "Not fetched yet", bad: false };
  const what = [v.temp !== undefined ? `${v.temp}°` : "", v.desc || ""].filter(Boolean).join(", ");
  return { text: `${what || "Fetched"} · fetched ${relTime(st.fetched_at, now)}`, bad: false };
}

// Latitude and longitude from what a map gives when its place is copied: "59.9139, 10.7522",
// "59,9139 10,7522" or "59.9139 N, 10.7522 E"; null when it is not two such numbers
export function parseCoords(text) {
  const s = String(text).replace(/[°NnEeØø]/g, " ").trim();
  let m = s.match(/^(-?\d+(?:\.\d+)?)\s*[,;\s]\s*(-?\d+(?:\.\d+)?)$/);
  if (!m) m = s.match(/^(-?\d+(?:,\d+)?)\s+(-?\d+(?:,\d+)?)$/); // decimal commas
  if (!m) return null;
  const lat = Number(m[1].replace(",", "."));
  const lon = Number(m[2].replace(",", "."));
  if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)) return null;
  return { lat: Math.round(lat * 1e4) / 1e4, lon: Math.round(lon * 1e4) / 1e4 };
}

// The place search, asked by the browser as you type (the AP only stores the coordinates):
// Kartverket's place names (prefix search, Norway) and Open-Meteo's geocoding (whole names,
// the world, from GeoNames). Both allow a search per keystroke and answer CORS with *
const NO_TYPES = ["By", "Tettsted", "Bydel", "Tettsteddel", "Tettbebyggelse", "Bygd", "Grend", "Kommune", "Øy", "Øy i sjø", "Fjell", "Nasjonalpark", "Turisthytte"];
const round4 = (x) => Math.round(Number(x) * 1e4) / 1e4;
const okCoords = (lat, lon) => Number.isFinite(lat) && Number.isFinite(lon) && Math.abs(lat) <= 90 && Math.abs(lon) <= 180;
// "Troms - Romsa - Tromssa" -> "Troms"
const firstName = (s) => String(s ?? "").split(" - ")[0].trim();

export function placeUrls(q, lang = "nb") {
  q = String(q).trim().replace(/[*?]/g, "");
  const no = new URL("https://ws.geonorge.no/stedsnavn/v1/navn");
  no.search = new URLSearchParams({ sok: q + "*", fuzzy: "false", utkoordsys: "4258", treffPerSide: "40" });
  const world = new URL("https://geocoding-api.open-meteo.com/v1/search");
  world.search = new URLSearchParams({ name: q, count: "8", language: lang === "en" ? "en" : "nb", format: "json" });
  return { no: no.href, world: world.href };
}

// Kartverket's answer as [{ name, detail, lat, lon, place }]: towns and villages first (the
// name typed in full before the longer ones), then islands and mountains (place false);
// roads, farms and the like left out
export function parseNorway(json, q = "") {
  const typed = String(q).trim().toLocaleLowerCase("nb");
  const out = [];
  for (const n of Array.isArray(json?.navn) ? json.navn : []) {
    const type = NO_TYPES.indexOf(n.navneobjekttype);
    const p = n.representasjonspunkt ?? {};
    const lat = round4(p.nord), lon = round4(p["øst"]);
    const name = String(n.skrivemåte ?? n.stedsnavn?.[0]?.skrivemåte ?? "").trim();
    if (type < 0 || !name || !okCoords(lat, lon)) continue;
    const kommuner = n.kommuner ?? [];
    const kommune = kommuner.length === 1 ? firstName(kommuner[0].kommunenavn) : "";
    const fylke = firstName(n.fylker?.[0]?.fylkesnavn);
    const detail = [kommune !== name ? kommune : "", fylke].filter(Boolean).join(", ");
    const place = type <= NO_TYPES.indexOf("Kommune");
    const rank = (place ? 0 : 2) + (name.toLocaleLowerCase("nb") === typed ? 0 : 1);
    out.push({ name, detail, lat, lon, place, rank });
  }
  // a stable sort keeps Kartverket's own order within a rank
  return out.sort((x, y) => x.rank - y.rank).map(({ rank, ...h }) => h);
}

// Open-Meteo's answer as [{ name, detail, lat, lon }] (it sorts by population)
export function parseWorld(json) {
  const out = [];
  for (const r of Array.isArray(json?.results) ? json.results : []) {
    const lat = round4(r.latitude), lon = round4(r.longitude);
    const name = String(r.name ?? "").trim();
    if (!name || !okCoords(lat, lon)) continue;
    out.push({ name, detail: [r.admin1, r.country].filter(Boolean).join(", "), lat, lon });
  }
  return out;
}

// The suggestions: Norway's towns and villages, then the world's places called just what was
// typed, then Norway's islands and mountains, then the rest of the world's; one place once
// (the same name within about 5 km), at most `max`
export function mergePlaces(q, norway, world, max = 8) {
  const key = (s) => s.toLocaleLowerCase("nb");
  const exact = (h) => key(h.name) === key(String(q).trim());
  const out = [];
  for (const h of [...norway.filter((h) => h.place), ...world.filter(exact), ...norway.filter((h) => !h.place), ...world.filter((h) => !exact(h))]) {
    if (out.some((o) => key(o.name) === key(h.name) && Math.abs(o.lat - h.lat) < 0.05 && Math.abs(o.lon - h.lon) < 0.1)) continue;
    out.push({ name: h.name.slice(0, 32), detail: h.detail, lat: h.lat, lon: h.lon });
    if (out.length === max) break;
  }
  return out;
}

// A source id (a-z 0-9 _ -, at most 16) from a place's name that `taken` does not have
export function placeId(place, taken) {
  const base = String(place).toLowerCase()
    .replace(/æ/g, "ae").replace(/[øö]/g, "o").replace(/[åä]/g, "a")
    .normalize("NFD").replace(/[̀-ͯ]/g, "")
    .replace(/[^a-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 16) || "place";
  let id = base;
  for (let n = 2; taken.has(id); n++) id = `${base.slice(0, 16 - String(n).length - 1)}-${n}`;
  return id;
}

// The sources with `macs` moved to one place: `to` is a source's id, or a new place
// { place, lat, lon, lang }. The tags leave every other place, and a place left without tags
// goes. { sources, id } as PUT /api/sources takes them (no status), or { error }.
export function weatherPlan(sources, macs, to) {
  const moving = new Set(macs);
  let list = sources.map(({ status, ...s }) => ({ ...s, tags: (s.tags || []).filter((m) => !moving.has(m)) }));
  let id = typeof to === "string" ? to : null;
  if (id === null) {
    if (!to?.place?.trim()) return { error: "Give the place a name: it is shown on the tag" };
    if (!Number.isFinite(to.lat) || !Number.isFinite(to.lon)) return { error: "Coordinates: latitude and longitude, such as 59.9139, 10.7522" };
    id = placeId(to.place, new Set(list.map((s) => s.id)));
    list.push({ id, type: "met", lat: to.lat, lon: to.lon, place: to.place.trim(), lang: to.lang || "nb", layout: WEATHER_LAYOUT, tags: [] });
  }
  const target = list.find((s) => s.id === id);
  if (!target) return { error: `There is no place called ${id} on the AP` };
  target.tags = [...target.tags, ...macs];
  if (target.tags.length > TAGS_MAX) return { error: `A place can have at most ${TAGS_MAX} tags (${target.place || id} would have ${target.tags.length})` };
  list = list.filter((s) => s.tags.length || s.id === id);
  if (list.length > SOURCES_MAX) return { error: `The AP takes at most ${SOURCES_MAX} places: give the tags one it has` };
  return { sources: list, id };
}

// The models of the tags whose layout file on the AP has no layout called "weather", from
// GET /api/layouts
export function missingModels(tags, layouts) {
  const has = new Set();
  for (const m of layouts?.layouts || [])
    if ((m.layouts || []).some((l) => (typeof l === "string" ? l : l.name) === WEATHER_LAYOUT)) has.add(m.model);
  return [...new Set(tags.map((t) => t.model).filter((m) => m && !has.has(m)))];
}

// The place each tag is in now, by MAC
export function placeOf(sources) {
  const out = new Map();
  for (const s of sources || []) for (const m of s.tags || []) out.set(m, s.place || s.id);
  return out;
}

// The weather template: published next to the page (web/templates.mjs) and packed on the AP
// (ap/main/webui.py), at templates/home/weather.json
async function weatherTemplate() {
  const r = await fetch(new URL("../templates/home/weather.json", import.meta.url), { cache: "no-cache" });
  if (!r.ok) throw new Error(`the weather template could not be loaded (${r.status})`);
  return r.json();
}

export function initSources() {
  let client = null;
  let data = null;      // the last GET /api/sources, null when the AP has none
  let timer = 0;
  let pending = null;   // the dialog's tags and what to call when it is done

  const msg = (id, text, cls = "") => {
    $(id).textContent = text;
    $(id).className = "hint" + (cls ? " " + cls : "");
  };
  const option = () => $("st-what").querySelector('option[value="weather"]');
  // The choice in the Send form, only on an AP with data sources
  const offer = (on) => {
    option().hidden = option().disabled = !on;
    if (!on && $("st-what").value === "weather") {
      $("st-what").value = "ap";
      $("st-what").dispatchEvent(new Event("change", { bubbles: true }));
    }
  };

  async function load() {
    clearTimeout(timer);
    if (!client) return;
    const c = client;
    try {
      data = await c.call("GET", "/api/sources");
    } catch (e) {
      if (c !== client) return;
      // an AP without data sources answers 404: no weather here
      if (e.message.startsWith("404")) {
        data = null;
        offer(false);
        $("src-box").hidden = true;
        return;
      }
      msg("src-msg", e.message, "error");
      timer = setTimeout(load, 60000);
      return;
    }
    if (c !== client) return;
    offer(true);
    render();
    timer = setTimeout(load, 60000);
  }

  function render() {
    const now = Date.now() / 1000;
    const list = data?.sources || [];
    $("src-box").hidden = !list.length;
    $("src-sum").textContent = list.length ? `${list.length} place${list.length === 1 ? "" : "s"}` : "";
    $("src-list").replaceChildren(...list.map((s) => {
      const tr = document.createElement("tr");
      const sum = sourceSummary(s, now);
      const cells = [[`${s.place || s.id}`, `${s.lat}, ${s.lon}`], [String(s.tags.length)], [sum.text]];
      for (const [text, title] of cells) {
        const td = document.createElement("td");
        td.textContent = text;
        if (title) td.title = title;
        tr.append(td);
      }
      if (sum.bad) tr.lastChild.className = "error";
      const act = document.createElement("td");
      const del = document.createElement("button");
      del.type = "button";
      del.className = "small";
      del.textContent = "Remove";
      del.title = "Stop fetching this place; its tags keep what they show until they get other content";
      del.addEventListener("click", () => remove(s));
      act.append(del);
      tr.append(act);
      return tr;
    }));
  }

  async function remove(s) {
    if (!confirm(`Stop the weather for ${s.place || s.id}? Its ${s.tags.length === 1 ? "tag keeps" : "tags keep"} what ${s.tags.length === 1 ? "it shows" : "they show"} now.`)) return;
    try {
      // a fresh list: the shown one may be a minute old
      const now = await client.call("GET", "/api/sources");
      data = await client.call("PUT", "/api/sources", { sources: now.sources.filter((x) => x.id !== s.id).map(({ status, ...x }) => x) });
      render();
      msg("src-msg", `Stopped the weather for ${s.place || s.id}.`, "ok");
    } catch (e) {
      msg("src-msg", "Not changed: " + e.message, "error");
    }
  }

  // ---- the dialog ----

  let layouts = null; // GET /api/layouts when the dialog opened

  function placesShown() {
    const list = data?.sources || [];
    const radios = list.map((s) => {
      const l = document.createElement("label");
      const r = document.createElement("input");
      r.type = "radio";
      r.name = "wx-place";
      r.value = s.id;
      l.append(r, ` ${s.place || s.id} `, Object.assign(document.createElement("span"), {
        className: "muted", textContent: `(${s.tags.length} ${s.tags.length === 1 ? "tag" : "tags"})` }));
      return l;
    });
    const l = document.createElement("label");
    const r = document.createElement("input");
    r.type = "radio";
    r.name = "wx-place";
    r.value = "";
    l.append(r, " A new place");
    radios.push(l);
    $("wx-places").replaceChildren(...radios);
    $("wx-places").hidden = !list.length;
    // the place most of the ticked tags are in already, else the first, else a new one
    const counts = new Map();
    const where = placeOf(list);
    for (const t of pending.tags) if (where.has(t.mac)) {
      const s = list.find((x) => x.tags.includes(t.mac));
      counts.set(s.id, (counts.get(s.id) || 0) + 1);
    }
    const best = [...counts].sort((a, b) => b[1] - a[1])[0]?.[0] ?? list[0]?.id ?? "";
    for (const x of $("wx-places").querySelectorAll("input")) x.checked = x.value === best;
  }

  const chosen = () => $("wx-places").querySelector("input:checked")?.value ?? "";

  // The suggestions under the place field, as you type
  let hits = [], active = -1, seq = 0, typing = 0;
  let picked = ""; // the coordinates a suggestion filled in: a new name clears only those
  const list = $("wx-hits");
  function pick(h) {
    $("wx-place").value = h.name;
    $("wx-coords").value = picked = `${h.lat}, ${h.lon}`;
    close();
    msg("wx-msg", `${h.name}${h.detail ? ", " + h.detail : ""} (${h.lat}, ${h.lon})`, "ok");
  }
  function close() {
    list.hidden = true;
    active = -1;
    $("wx-place").setAttribute("aria-expanded", "false");
    $("wx-place").removeAttribute("aria-activedescendant");
  }
  function show() {
    list.replaceChildren(...hits.map((h, i) => {
      const li = document.createElement("li");
      li.id = "wx-hit-" + i;
      li.setAttribute("role", "option");
      li.setAttribute("aria-selected", String(i === active));
      const b = document.createElement("b");
      b.textContent = h.name;
      const d = document.createElement("span");
      d.textContent = h.detail;
      li.append(b, d);
      // mousedown, so the field keeps the focus and does not close the list first
      li.addEventListener("mousedown", (ev) => {
        ev.preventDefault();
        pick(h);
      });
      return li;
    }));
    list.hidden = !hits.length;
    $("wx-place").setAttribute("aria-expanded", String(!list.hidden));
    if (active >= 0) $("wx-place").setAttribute("aria-activedescendant", "wx-hit-" + active);
    else $("wx-place").removeAttribute("aria-activedescendant");
    $("wx-osm").hidden = !hits.length;
  }
  // Asks both services; the newest question wins. Resolves to the suggestions (empty when
  // nothing was found), or throws when neither service answered
  function lookup(q) {
    const n = ++seq;
    const urls = placeUrls(q, $("wx-lang").value);
    const get = (url, parse) => fetch(url).then((r) => (r.ok ? r.json() : Promise.reject(new Error(`HTTP ${r.status}`)))).then(parse);
    return Promise.allSettled([get(urls.no, (j) => parseNorway(j, q)), get(urls.world, parseWorld)]).then(([no, world]) => {
      if (no.status === "rejected" && world.status === "rejected") throw no.reason;
      const found = mergePlaces(q, no.value ?? [], world.value ?? []);
      if (n === seq) {
        hits = found;
        active = -1;
        if (document.activeElement === $("wx-place")) show();
      }
      return found;
    });
  }
  function typed() {
    if ($("wx-coords").value === picked) $("wx-coords").value = "";
    picked = "";
    msg("wx-msg", "");
    clearTimeout(typing);
    const q = $("wx-place").value.trim();
    if (q.length < 2) {
      seq++;
      hits = [];
      return close();
    }
    typing = setTimeout(() => lookup(q).catch(() => {}), 250);
  }
  // At "Show the weather" with a name typed but none picked: the first suggestion, looked up
  // now when it is not there yet. False when there is none (the message says why)
  async function firstHit() {
    const q = $("wx-place").value.trim();
    clearTimeout(typing);
    msg("wx-msg", `Looking for ${q} …`);
    let found;
    try {
      found = await lookup(q);
    } catch (e) {
      msg("wx-msg", `The place search did not answer (${e.message}): give the coordinates under Coordinates instead.`, "error");
      $("wx-coords-box").open = true;
      return false;
    }
    if (!found.length) return msg("wx-msg", `Found no place called ${q}: check the spelling, or give the coordinates.`, "error"), false;
    pick(found[0]);
    return true;
  }

  function dialogChanged() {
    $("wx-new").hidden = chosen() !== "";
    const missing = missingModels(pending.tags, layouts);
    $("wx-layout-label").hidden = !missing.length;
    $("wx-layout-text").textContent = `Add the Weather layout to the AP's ${missing.map((m) => m + '"').join(" and ")} layout ${missing.length === 1 ? "file" : "files"} (the tags need it; the layouts there stay)`;
  }

  async function open(tags, done) {
    pending = { tags, done };
    layouts = await client.call("GET", "/api/layouts").catch(() => null);
    $("wx-n").textContent = tags.length === 1 ? "1 tag" : `${tags.length} tags`;
    placesShown();
    hits = [];
    seq++;
    close();
    $("wx-osm").hidden = true;
    $("wx-place").value = $("wx-coords").value = picked = "";
    $("wx-coords-box").open = false;
    msg("wx-msg", "");
    dialogChanged();
    $("wx-ok").disabled = false;
    $("wx-dlg").showModal();
    (chosen() === "" ? $("wx-place") : $("wx-ok")).focus();
  }

  // The weather layout into the AP's layout file of each model that lacks it
  async function addLayouts(c, models) {
    const t = await weatherTemplate();
    for (const model of models) {
      let doc;
      try {
        doc = await c.call("GET", "/api/layouts/" + encodeURIComponent(model));
      } catch (e) {
        if (!e.message.startsWith("404")) throw e;
        doc = { format: "esl-layouts", version: 1, model, layouts: [] }; // no file yet
      }
      const added = addTemplate(doc, t, model);
      if (!added) throw new Error(`the weather template has no layout for the ${model}"`);
      msg("wx-msg", `Adding the Weather layout to the ${model}" layout file …`);
      try {
        await c.call("PUT", "/api/layouts/" + encodeURIComponent(model), added.doc);
      } catch (e) {
        throw new Error(`the ${model}" layout file did not take the Weather layout: ${e.message}`);
      }
    }
  }

  async function submit(ev) {
    ev.preventDefault();
    const c = client;
    const macs = pending.tags.map((t) => t.mac);
    const id = chosen();
    let to = id;
    if (id === "") {
      if (!$("wx-place").value.trim()) return msg("wx-msg", "Write a place, such as Bergen: its name is shown on the tag.", "error");
      // a place typed but not looked up yet: look it up now and take the first one found
      if (!$("wx-coords").value.trim() && !(await firstHit())) return;
      const coords = parseCoords($("wx-coords").value);
      if (!coords) return msg("wx-msg", "Coordinates: latitude and longitude, such as 59.9139, 10.7522.", "error");
      to = { place: $("wx-place").value, ...coords, lang: $("wx-lang").value };
    }
    const missing = missingModels(pending.tags, layouts);
    if (missing.length && !$("wx-layout").checked)
      return msg("wx-msg", `The ${missing.join('" and ')}" tags need a layout called "weather" on the AP: tick the box, or add it in the Design tab.`, "error");
    $("wx-ok").disabled = true;
    try {
      // the latest list, so a change made elsewhere meanwhile is kept
      const now = await c.call("GET", "/api/sources");
      const plan = weatherPlan(now.sources || [], macs, to);
      if (plan.error) throw new Error(plan.error);
      if (missing.length) await addLayouts(c, missing);
      msg("wx-msg", "Saving the place on the AP …");
      data = await c.call("PUT", "/api/sources", { sources: plan.sources });
      render();
      const s = data.sources.find((x) => x.id === plan.id);
      $("wx-dlg").close();
      pending.done?.(`The AP fetches the weather for ${s?.place || plan.id} now; ${macs.length === 1 ? "the tag shows" : "the tags show"} it at the next poll.`);
      setTimeout(load, 5000);
    } catch (e) {
      msg("wx-msg", "Not done: " + e.message, "error");
      $("wx-ok").disabled = false;
    }
  }

  $("wx-form").addEventListener("submit", submit);
  $("wx-place").addEventListener("input", typed);
  $("wx-place").addEventListener("keydown", (ev) => {
    if (list.hidden || !hits.length) return;
    if (ev.key === "ArrowDown" || ev.key === "ArrowUp") {
      ev.preventDefault();
      active = (active + (ev.key === "ArrowDown" ? 1 : hits.length - 1 + (active < 0 ? 1 : 0))) % hits.length;
      show();
    } else if (ev.key === "Enter" && active >= 0) {
      ev.preventDefault();
      pick(hits[active]);
    } else if (ev.key === "Escape") {
      ev.preventDefault();
      ev.stopPropagation();
      close();
    }
  });
  $("wx-place").addEventListener("blur", close);
  $("wx-place").addEventListener("focus", () => hits.length && !$("wx-coords").value && show());
  $("wx-form").addEventListener("change", dialogChanged);
  $("wx-cancel").addEventListener("click", () => $("wx-dlg").close());
  $("src-refresh").addEventListener("click", async () => {
    try {
      await client.call("POST", "/api/sources/refresh");
      msg("src-msg", "Fetching now; the tags change at their next poll if the weather did.", "ok");
      setTimeout(load, 5000);
    } catch (e) {
      msg("src-msg", e.message, "error");
    }
  });

  return {
    // The dashboard connected to an AP (or disconnected: null)
    connected(c) {
      client = c;
      data = null;
      if ($("wx-dlg").open) $("wx-dlg").close();
      offer(false);
      $("src-box").hidden = true;
      clearTimeout(timer);
      if (c) load();
    },
    // The place each ticked tag shows now, for the Send form's list
    placeOf: () => placeOf(data?.sources),
    // Send → The weather: the dialog for these tags; done(message) once it is saved
    open: (tags, done) => open(tags, done).catch((e) => done?.(null, e)),
    // Other content was sent to tags: the AP took them out of their places, so read the list again
    reload: () => load(),
  };
}
