// apsources.js - the weather on the Dashboard (ESP32-S3 and ESP32-C5 APs, docs/data-sources.md):
// "The weather" in the Send form for the ticked tags, with a dialog to choose the place, and the
// "Weather" card listing the places the AP fetches (GET/PUT /api/sources, POST
// /api/sources/refresh). A place is a data source: the AP fetches its forecast from MET Norway
// and puts it on its tags in the layout "weather" (templates/home/weather.json), which the dialog
// offers to add to the AP's layout file of a model that lacks it. On an AP without data sources
// (404) the choice and the card stay hidden.

import { addTemplate } from "./catalog.js";
import { modelName } from "./models.js";

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
  // a countdown fetches nothing: its time left
  if (s.type === "countdown") return { text: v.left || "Not counted yet", bad: false };
  if (!st.fetched_at) return { text: "Not fetched yet", bad: false };
  const what = s.type === "power" ? (v.now ? `${v.now} kr/kWh` : "") : s.type === "rss" ? v.h1 || ""
    : s.type === "ical" ? [v.day1, v.time1, v.what1].filter(Boolean).join(" ") || "No events ahead"
    : [v.temp !== undefined ? `${v.temp}°` : "", v.desc || ""].filter(Boolean).join(", ");
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
  // a weather place the move emptied goes; another type's source that had no tags yet stays
  const empty = new Set(sources.filter((s) => !(s.tags || []).length).map((s) => s.id));
  list = list.filter((s) => s.tags.length || s.id === id || (s.type !== "met" && empty.has(s.id)));
  if (list.length > SOURCES_MAX) return { error: `The AP takes at most ${SOURCES_MAX} places: give the tags one it has` };
  return { sources: list, id };
}

// The sources with `macs` showing the PNG at `url` (type "image", AP 0.11.0 and later): the
// source that already fetches this URL takes them, else a new one. As weatherPlan, the tags
// leave every other source, and a source left without tags goes. { sources, id } or { error }.
export function picturePlanUrl(sources, macs, { url, every_min = 5, dither = false }) {
  url = String(url || "").trim();
  // as the AP checks it (esl_source_image.c): 159 bytes of UTF-8, no user:password@
  if (!/^https?:\/\/[^/?#]+/.test(url) || !/^[!-~]+$/.test(url) || url.length > 159) return { error: "The address must start with http:// or https:// (up to 159 characters, no spaces or letters outside ASCII: use %-escapes)" };
  if (/^https?:\/\/[^/?#]*@/.test(url)) return { error: "The address cannot hold a user name or password: the AP fetches it without a login" };
  const every = Number(every_min);
  if (!Number.isInteger(every) || every < 5 || every > 1440) return { error: "Every: 5 to 1440 minutes" };
  const moving = new Set(macs);
  let list = sources.map(({ status, ...s }) => ({ ...s, tags: (s.tags || []).filter((m) => !moving.has(m)) }));
  let target = list.find((s) => s.type === "image" && s.url === url);
  if (!target) {
    const taken = new Set(list.map((s) => s.id));
    let id = "picture";
    for (let n = 2; taken.has(id); n++) id = `picture-${n}`;
    target = { id, type: "image", url, tags: [] };
    list.push(target);
  }
  Object.assign(target, { every_min: every, dither: !!dither });
  target.tags = [...target.tags, ...macs];
  if (target.tags.length > TAGS_MAX) return { error: `A source can have at most ${TAGS_MAX} tags` };
  // a source the move emptied goes; another type's source that had no tags yet stays
  const empty = new Set(sources.filter((s) => !(s.tags || []).length).map((s) => s.id));
  list = list.filter((s) => s.tags.length || s === target || (s.type !== "met" && empty.has(s.id)));
  if (list.length > SOURCES_MAX) return { error: `The AP takes at most ${SOURCES_MAX} sources (weather places and pictures)` };
  return { sources: list, id: target.id };
}

// The kinds of content the AP makes by itself on the built-in layout "text" (AP 0.11.0 and
// later): the id a new source gets, and what to call it in messages
export const FEEDS = {
  power: { id: "power", what: "the electricity price" },
  countdown: { id: "countdown", what: "the countdown" },
  rss: { id: "news", what: "the headlines" },
  ical: { id: "calendar", what: "the calendar" },
};

const utf8Len = (t) => new TextEncoder().encode(t).length;

// A source of one of FEEDS from what the dialog gives, checked as the AP checks it; { error }
export function feedSource(spec) {
  const t = spec?.type;
  if (!FEEDS[t]) return { error: "Choose what the tags show" };
  const lang = spec.lang === "en" ? "en" : "nb";
  if (t === "power") {
    const area = String(spec.area || "");
    if (!/^NO[1-5]$/.test(area)) return { error: "Choose a price area, NO1 to NO5" };
    return { type: t, area, vat: spec.vat === undefined ? area !== "NO4" : !!spec.vat, lang };
  }
  if (t === "countdown") {
    const place = String(spec.place || "").trim();
    if (!place) return { error: "Give it a name: it is shown on the tag (Christmas, Holiday)" };
    if (utf8Len(place) > 32) return { error: "The name: at most 32 bytes" };
    const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(spec.date || "");
    const d = m && new Date(Date.UTC(+m[1], +m[2] - 1, +m[3]));
    if (!m || +m[1] < 1900 || +m[1] > 2100 || d.getUTCMonth() !== +m[2] - 1 || d.getUTCDate() !== +m[3]) return { error: "The date: a day from 1900 to 2100" };
    const src = { type: t, place, date: spec.date, yearly: !!spec.yearly, lang };
    if (spec.time) {
      if (!/^([01]\d|2[0-3]):[0-5]\d$/.test(spec.time)) return { error: "The time: HH:MM, or leave it empty" };
      src.time = spec.time;
    }
    return src;
  }
  let url = String(spec.url || "").trim();
  if (t === "ical") url = url.replace(/^webcal:\/\//i, "https://");
  // as esl_source_url_ok: printable ASCII only
  if (!/^https?:\/\/[^/?#]+/.test(url) || !/^[!-~]+$/.test(url) || url.length > 159) return { error: `The address must start with http:// or https://${t === "ical" ? " (or webcal://)" : ""}, up to 159 characters, no spaces or letters outside ASCII (use %-escapes)` };
  if (/^https?:\/\/[^/?#]*@/.test(url)) return { error: "The address cannot hold a user name or password: the AP fetches it without a login" };
  return t === "ical" ? { type: t, url, lang } : { type: t, url };
}

// Whether two sources of FEEDS show the same thing: then one source takes both sets of tags
function sameFeed(a, b) {
  if (a.type !== b.type) return false;
  if (a.type === "power") return a.area === b.area && !!a.vat === !!b.vat && (a.lang || "nb") === (b.lang || "nb");
  if (a.type === "countdown") return a.place === b.place && a.date === b.date && (a.time || "") === (b.time || "") && !!a.yearly === !!b.yearly && (a.lang || "nb") === (b.lang || "nb");
  if (a.type === "rss") return a.url === b.url; // rss has no language
  return a.url === b.url && (a.lang || "nb") === (b.lang || "nb");
}

// The sources with `macs` showing a source of FEEDS (spec: as feedSource takes it) on the
// built-in layout "text". As weatherPlan, the tags leave every other source, and a source left
// without tags goes. { sources, id } or { error }.
export function feedPlan(sources, macs, spec) {
  const src = feedSource(spec);
  if (src.error) return src;
  const moving = new Set(macs);
  let list = sources.map(({ status, ...s }) => ({ ...s, tags: (s.tags || []).filter((m) => !moving.has(m)) }));
  let target = list.find((s) => sameFeed(s, src));
  if (!target) {
    const taken = new Set(list.map((s) => s.id));
    // a countdown is named after what it counts to
    const base = src.type === "countdown" ? placeId(src.place, new Set()) : FEEDS[src.type].id;
    let id = base;
    for (let n = 2; taken.has(id); n++) id = `${base.slice(0, 16 - String(n).length - 1)}-${n}`;
    target = { id, ...src, layout: "text", tags: [] };
    list.push(target);
  }
  target.tags = [...target.tags, ...macs];
  if (target.tags.length > TAGS_MAX) return { error: `A source can have at most ${TAGS_MAX} tags` };
  const empty = new Set(sources.filter((s) => !(s.tags || []).length).map((s) => s.id));
  list = list.filter((s) => s.tags.length || s === target || (s.type !== "met" && empty.has(s.id)));
  if (list.length > SOURCES_MAX) return { error: `The AP takes at most ${SOURCES_MAX} sources: remove one in the Data sources card` };
  return { sources: list, id: target.id };
}

// The models of the tags whose layout file on the AP has no layout called "weather", from
// GET /api/layouts
// (a model whose same_as names another stands for that model: it is that model's file)
export function missingModels(tags, layouts) {
  const has = new Set();
  const fileOf = {};
  for (const m of layouts?.layouts || []) {
    if (m.same_as) fileOf[m.model] = m.same_as;
    if ((m.layouts || []).some((l) => (typeof l === "string" ? l : l.name) === WEATHER_LAYOUT)) has.add(m.model);
  }
  return [...new Set(tags.map((t) => fileOf[t.model] || t.model).filter((m) => m && !has.has(m)))];
}

// "host/…/file.png" of a picture's address, to name it in lists
export function shortUrl(url) {
  const m = /^https?:\/\/([^/?#]+)([^?#]*)/.exec(url || "");
  if (!m) return url || "";
  const parts = m[2].split("/").filter(Boolean);
  return !parts.length ? m[1] : parts.length === 1 ? `${m[1]}/${parts[0]}` : `${m[1]}/…/${parts[parts.length - 1]}`;
}

// Only the host of an address ("calendar.example"), for one whose path is a secret
export function hostOf(url) {
  const m = /^[a-z]+:\/\/([^/?#]+)/.exec(url || "");
  return m ? m[1] : "";
}

// What each tag shows now from a source, by MAC: "the weather for Oslo", "the headlines from
// news.example", for the Send list
export function showsOf(sources) {
  const out = new Map();
  const what = (s) => ({
    met: () => `the weather for ${s.place || s.id}`,
    image: () => `the picture from ${shortUrl(s.url)}`,
    power: () => `the electricity price in ${s.area}`,
    countdown: () => `the countdown to ${s.place || s.id}`,
    rss: () => `the headlines from ${hostOf(s.url)}`,
    ical: () => `the calendar from ${hostOf(s.url)}`,
  })[s.type]?.() ?? `the source ${s.id}`;
  for (const s of sources || []) for (const m of s.tags || []) out.set(m, what(s));
  return out;
}

// The weather place each tag is in now, by MAC (not a picture from a URL)
export function placeOf(sources) {
  const out = new Map();
  for (const s of sources || []) if (s.type === "met") for (const m of s.tags || []) out.set(m, s.place || s.id);
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
  const options = () => $("st-what").querySelectorAll('option[value="weather"], option[value="feed"]');
  // The choices in the Send form, only on an AP with data sources
  const offer = (on) => {
    for (const o of options()) o.hidden = o.disabled = !on;
    if (!on && ["weather", "feed"].includes($("st-what").value)) {
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
    $("src-sum").textContent = list.length ? `${list.length} source${list.length === 1 ? "" : "s"}` : "";
    $("src-list").replaceChildren(...list.map((s) => {
      const tr = document.createElement("tr");
      const sum = sourceSummary(s, now);
      // a picture from a URL (type image, set with PUT /api/sources) is listed with its URL
      const pic = s.type === "image";
      // the electricity price (type power) with its price area
      const cells = [pic ? [`${s.id} · ${shortUrl(s.url)}`, s.url] : s.type === "power" ? [`${s.id} · power ${s.area}`, "hvakosterstrommen.no"]
        : s.type === "countdown" ? [`${s.place || s.id} · countdown`, `${s.date}${s.time ? " " + s.time : ""}${s.yearly ? ", every year" : ""}`]
        : s.type === "rss" ? [`${s.id} · ${shortUrl(s.url)}`, s.url]
        // a calendar's address is often its secret: only the host is shown
        : s.type === "ical" ? [`${s.id} · calendar`, hostOf(s.url)] : [`${s.place || s.id}`, `${s.lat}, ${s.lon}`], [String(s.tags.length)], [sum.text]];
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
      del.title = `${s.type === "countdown" ? "Stop" : "Stop fetching"} ${pic ? "this picture" : s.type === "power" ? "the electricity price" : s.type === "countdown" ? "the countdown" : s.type === "rss" ? "the headlines" : s.type === "ical" ? "the calendar" : "the weather for this place"}; its tags keep what they show until they get other content`;
      del.addEventListener("click", () => remove(s));
      act.append(del);
      tr.append(act);
      return tr;
    }));
  }

  async function remove(s) {
    const what = { image: "the picture", power: "the electricity price", countdown: "the countdown", rss: "the headlines", ical: "the calendar" }[s.type] || "the weather";
    if (!confirm(`Stop ${what} for ${s.place || s.id}? Its ${s.tags.length === 1 ? "tag keeps" : "tags keep"} what ${s.tags.length === 1 ? "it shows" : "they show"} now.`)) return;
    try {
      // a fresh list: the shown one may be a minute old
      const now = await client.call("GET", "/api/sources");
      data = await client.call("PUT", "/api/sources", { sources: now.sources.filter((x) => x.id !== s.id).map(({ status, ...x }) => x) });
      render();
      msg("src-msg", `Stopped ${what} for ${s.place || s.id}.`, "ok");
    } catch (e) {
      msg("src-msg", "Not changed: " + e.message, "error");
    }
  }

  // ---- the dialog ----

  let layouts = null; // GET /api/layouts when the dialog opened

  function placesShown() {
    const list = (data?.sources || []).filter((s) => s.type === "met");
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
    $("wx-layout-text").textContent = `Add the Weather layout to the AP's ${missing.map(modelName).join(" and ")} layout ${missing.length === 1 ? "file" : "files"} (the tags need it; the layouts there stay)`;
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
      if (!added) throw new Error(`the weather template has no layout for the ${modelName(model)}`);
      msg("wx-msg", `Adding the Weather layout to the ${modelName(model)} layout file …`);
      try {
        await c.call("PUT", "/api/layouts/" + encodeURIComponent(model), added.doc);
      } catch (e) {
        throw new Error(`the ${modelName(model)} layout file did not take the Weather layout: ${e.message}`);
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
      return msg("wx-msg", `The ${missing.map(modelName).join(" and ")} tags need a layout called "weather" on the AP: tick the box, or add it in the Design tab.`, "error");
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

  // ---- Send → Price, countdown, news or calendar: one of FEEDS on the ticked tags ----

  let feedFor = null; // { tags, done } while the dialog is open
  const URL_HINTS = {
    rss: ["https://news.example/top.rss", "Most news sites, blogs and status pages have a feed: look for RSS, or an address ending in .rss, .xml or /feed. The first 128 KB of a long feed are read."],
    ical: ["webcal://calendar.example/family.ics", "The calendar's address in iCal format (Google: Settings, the calendar, Secret address in iCal format). Anyone who has it can read the calendar, and the AP's API shows it: protect the API with a token."],
  };
  function feedChanged() {
    const t = $("feed-type").value;
    $("feed-power").hidden = t !== "power";
    $("feed-countdown").hidden = t !== "countdown";
    $("feed-url-box").hidden = t !== "rss" && t !== "ical";
    $("feed-lang-label").hidden = t === "rss";
    if (URL_HINTS[t]) {
      $("feed-url").placeholder = URL_HINTS[t][0];
      $("feed-url-hint").textContent = URL_HINTS[t][1];
    }
    msg("feed-msg", "");
  }
  function feedSpec() {
    const t = $("feed-type").value;
    return { type: t, lang: $("feed-lang").value, area: $("feed-area").value, vat: $("feed-vat").checked,
      place: $("feed-place").value, date: $("feed-date").value, time: $("feed-time").value,
      yearly: $("feed-yearly").checked, url: $("feed-url").value };
  }
  function openFeed(tags, done) {
    feedFor = { tags, done };
    $("feed-n").textContent = tags.length === 1 ? "1 tag" : `${tags.length} tags`;
    feedChanged();
    $("feed-ok").disabled = false;
    $("feed-dlg").showModal();
    $("feed-type").focus();
  }
  $("feed-type").addEventListener("change", feedChanged);
  // NO4 has no VAT on electricity: the box follows the area
  $("feed-area").addEventListener("change", () => ($("feed-vat").checked = $("feed-area").value !== "NO4"));
  $("feed-cancel").addEventListener("click", () => $("feed-dlg").close());
  $("feed-form").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    if (!feedFor) return;
    const spec = feedSpec();
    const check = feedSource(spec);
    if (check.error) return msg("feed-msg", check.error, "error");
    $("feed-ok").disabled = true;
    msg("feed-msg", "Saving …");
    try {
      const s = await api.addFeed(feedFor.tags.map((t) => t.mac), spec);
      const n = feedFor.tags.length;
      $("feed-dlg").close();
      feedFor.done?.(`The AP shows ${FEEDS[spec.type].what} on ${n === 1 ? "the tag" : `${n} tags`} (source ${s?.id ?? "saved"}); they get it within a few minutes.`);
      feedFor = null;
    } catch (e) {
      msg("feed-msg", "Not saved: " + e.message, "error");
      $("feed-ok").disabled = false;
    }
  });
  $("src-refresh").addEventListener("click", async () => {
    try {
      await client.call("POST", "/api/sources/refresh");
      msg("src-msg", "Fetching now; the tags change at their next poll if the weather did.", "ok");
      setTimeout(load, 5000);
    } catch (e) {
      msg("src-msg", e.message, "error");
    }
  });

  const api = {
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
    showsOf: () => showsOf(data?.sources),
    // Send → The weather: the dialog for these tags; done(message) once it is saved
    open: (tags, done) => open(tags, done).catch((e) => done?.(null, e)),
    // Send → Price, countdown, news or calendar: the dialog for these tags; done(message)
    openFeed: (tags, done) => openFeed(tags, done),
    // Other content was sent to tags: the AP took them out of their places, so read the list again
    reload: () => load(),
    // Whether the AP has data sources (a picture from a URL needs them)
    available: () => !!data,
    // The PNG at opts.url ({ url, every_min, dither }) on these tags, fetched by the AP: the
    // source as the AP saved it. Throws with a message when it cannot be done.
    // A source of FEEDS (spec as feedSource takes it) on these tags: the source as the AP saved
    // it. Throws with a message when it cannot be done.
    async addFeed(macs, spec) {
      const c = client;
      if (!c || !data) throw new Error("this AP cannot fetch content by itself");
      const now = await c.call("GET", "/api/sources");
      const plan = feedPlan(now.sources || [], macs, spec);
      if (plan.error) throw new Error(plan.error);
      try {
        data = await c.call("PUT", "/api/sources", { sources: plan.sources });
      } catch (e) {
        // an AP that does not know the type names the ones it knows
        if (/type: one of /.test(e.message) && !e.message.includes(`"${spec.type}"`)) throw new Error(`this AP needs newer firmware (0.11.0 or later) for ${FEEDS[spec.type].what}`);
        throw e;
      }
      render();
      setTimeout(load, 5000);
      return data.sources.find((x) => x.id === plan.id);
    },
    async addPicture(macs, opts) {
      const c = client;
      if (!c || !data) throw new Error("this AP cannot fetch pictures by itself");
      const now = await c.call("GET", "/api/sources");
      const plan = picturePlanUrl(now.sources || [], macs, opts);
      if (plan.error) throw new Error(plan.error);
      try {
        data = await c.call("PUT", "/api/sources", { sources: plan.sources });
      } catch (e) {
        // an AP before 0.11.0 has sources of type "met" only
        if (/type: one of "met" \([^)]*\)\s*$/.test(e.message)) throw new Error("this AP needs firmware 0.11.0 or later to fetch a picture from a web address");
        throw e;
      }
      render();
      setTimeout(load, 5000);
      return data.sources.find((x) => x.id === plan.id);
    },
  };
  return api;
}
