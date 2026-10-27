// apsources.js - the "Weather and data sources" card on the Dashboard: the AP's data sources
// (GET/PUT /api/sources, POST /api/sources/refresh; ESP32-S3 APs, docs/data-sources.md). A source
// is a place whose forecast the AP fetches from MET Norway every so often; its fields fill a
// layout on the tags given to it. The card stays hidden on an AP without data sources (404).

const $ = (id) => document.getElementById(id);
const EVERY = [30, 60, 120, 180, 360];

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
  return {
    text: `${what || "Fetched"} · fetched ${relTime(st.fetched_at, now)} · ${st.tags_set} tag(s), ${st.tags_changed} changed`,
    bad: false,
  };
}

// The source the form describes, as the API takes it; { error } when something is missing
export function formSource(f, defaults) {
  const id = f.id.trim().toLowerCase();
  if (!/^[a-z0-9_-]{1,16}$/.test(id)) return { error: "Name: 1 to 16 of a-z, 0-9, _ and -" };
  const lat = Number(String(f.lat).replace(",", "."));
  const lon = Number(String(f.lon).replace(",", "."));
  if (!(String(f.lat).trim() && lat >= -90 && lat <= 90)) return { error: "Latitude: -90 to 90 (e.g. 59.9139)" };
  if (!(String(f.lon).trim() && lon >= -180 && lon <= 180)) return { error: "Longitude: -180 to 180 (e.g. 10.7522)" };
  if (!f.layout) return { error: "Choose a layout" };
  const s = { id, type: "met", lat, lon, place: f.place.trim(), lang: f.lang, every_min: +f.every, layout: f.layout, tags: f.tags };
  const fields = f.fields.map((x) => x.trim());
  while (fields.length && !fields[fields.length - 1]) fields.pop();
  if (fields.length && JSON.stringify(fields) !== JSON.stringify(defaults)) s.fields = fields;
  return s;
}

// The configuration without each source's status, ready for PUT
export function configOf(sources) {
  return { sources: sources.map(({ status, ...s }) => s) };
}

export function initSources() {
  let client = null;
  let data = null;      // the last GET /api/sources
  let editing = -1;     // the source in the form, -1 for a new one, null when the form is closed
  let timer = 0;

  const msg = (text, cls = "") => {
    const m = $("src-msg");
    m.textContent = text;
    m.className = "hint " + cls;
  };

  async function load() {
    clearTimeout(timer);
    if (!client) return;
    try {
      data = await client.call("GET", "/api/sources");
    } catch (e) {
      // an AP without data sources answers 404: no card; anything else is shown, and tried again
      if (e.message.startsWith("404")) $("src-box").hidden = true;
      else msg(e.message, "error");
      if (!e.message.startsWith("404")) timer = setTimeout(load, 60000);
      return;
    }
    $("src-box").hidden = false;
    render();
    timer = setTimeout(load, 60000);
  }

  function render() {
    const now = Date.now() / 1000;
    const list = data?.sources || [];
    $("src-sum").textContent = list.length ? `${list.length} source${list.length === 1 ? "" : "s"}` : "none";
    $("src-list").replaceChildren(...list.map((s, i) => {
      const tr = document.createElement("tr");
      const sum = sourceSummary(s, now);
      const cells = [
        s.id,
        `${s.place || "–"} (${s.lat}, ${s.lon})`,
        s.layout,
        String(s.tags.length),
      ];
      for (const c of cells) {
        const td = document.createElement("td");
        td.textContent = c;
        tr.append(td);
      }
      const st = document.createElement("td");
      st.textContent = sum.text;
      if (sum.bad) st.className = "error";
      tr.append(st);
      const act = document.createElement("td");
      const ed = document.createElement("button");
      ed.type = "button";
      ed.textContent = "Edit";
      ed.addEventListener("click", () => openForm(i));
      const del = document.createElement("button");
      del.type = "button";
      del.textContent = "Remove";
      del.title = "Its tags keep what they show until they get other content";
      del.addEventListener("click", () => remove(i));
      act.append(ed, " ", del);
      tr.append(act);
      return tr;
    }));
    $("src-empty").hidden = list.length > 0;
  }

  async function openForm(i) {
    editing = i;
    const s = i >= 0 ? data.sources[i] : null;
    const defaults = data?.types?.met?.fields || [];
    $("src-form").hidden = false;
    $("src-id").value = s?.id || (data.sources.length ? "" : "home");
    $("src-id").disabled = !!s;
    $("src-place").value = s?.place || "";
    $("src-lat").value = s?.lat ?? "";
    $("src-lon").value = s?.lon ?? "";
    $("src-lang").value = s?.lang || "nb";
    $("src-every").replaceChildren(...EVERY.map((m) => new Option(m < 60 ? `${m} min` : `${m / 60} h`, m)));
    $("src-every").value = String(s?.every_min || 30);
    const fields = s?.fields || defaults;
    document.querySelectorAll("#src-fields input").forEach((inp, k) => (inp.value = fields[k] || ""));
    $("src-values").textContent = (data?.types?.met?.values || []).map((v) => `{${v}}`).join(" ");
    // the layouts in the AP's files, and its tags
    const [lays, tags] = await Promise.all([
      client.call("GET", "/api/layouts").catch(() => null),
      client.call("GET", "/api/tags").catch(() => null),
    ]);
    const names = new Set(["text"]);
    for (const m of lays?.layouts || []) for (const n of m.layouts || []) names.add(typeof n === "string" ? n : n.name);
    if (s?.layout) names.add(s.layout);
    const sel = $("src-layout");
    sel.replaceChildren(...[...names].map((n) => new Option(n === "text" ? "text (built in, four lines)" : n, n)));
    sel.value = s?.layout || (names.has("weather") ? "weather" : "text");
    $("src-no-weather").hidden = names.has("weather");
    const chosen = new Set(s?.tags || []);
    const all = (tags?.tags || []).map((t) => ({ mac: t.mac, model: t.model }));
    for (const mac of chosen) if (!all.some((t) => t.mac === mac)) all.push({ mac, model: "" });
    $("src-tags").replaceChildren(...all.map((t) => {
      const l = document.createElement("label");
      l.className = "check";
      const c = document.createElement("input");
      c.type = "checkbox";
      c.value = t.mac;
      c.checked = chosen.has(t.mac);
      l.append(c, ` ${t.mac}${t.model ? ` (${t.model}")` : ""}`);
      return l;
    }));
    msg("");
    $("src-place").focus();
  }

  function closeForm() {
    editing = null;
    $("src-form").hidden = true;
  }

  async function save(list, done) {
    try {
      data = await client.call("PUT", "/api/sources", configOf(list));
      render();
      msg(done, "ok");
      return true;
    } catch (e) {
      msg("Not saved: " + e.message, "error");
      return false;
    }
  }

  async function remove(i) {
    const s = data.sources[i];
    if (!confirm(`Remove the data source "${s.id}"? Its tags keep what they show now.`)) return;
    const list = data.sources.filter((_, k) => k !== i);
    if (await save(list, `Removed ${s.id}.`) && editing === i) closeForm();
  }

  $("src-add").addEventListener("click", () => openForm(-1));
  $("src-cancel").addEventListener("click", closeForm);
  $("src-refresh").addEventListener("click", async () => {
    try {
      await client.call("POST", "/api/sources/refresh");
      msg("Fetching now; the tags change at their next poll if the weather did.", "ok");
      setTimeout(load, 5000);
    } catch (e) {
      msg(e.message, "error");
    }
  });
  $("src-form").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    const s = formSource({
      id: $("src-id").value, place: $("src-place").value, lat: $("src-lat").value, lon: $("src-lon").value,
      lang: $("src-lang").value, every: $("src-every").value, layout: $("src-layout").value,
      tags: [...document.querySelectorAll("#src-tags input:checked")].map((c) => c.value),
      fields: [...document.querySelectorAll("#src-fields input")].map((i) => i.value),
    }, data?.types?.met?.fields || []);
    if (s.error) return msg(s.error, "error");
    const list = data.sources.slice();
    if (editing >= 0) list[editing] = s;
    else if (list.some((x) => x.id === s.id)) return msg(`There is already a source called ${s.id}`, "error");
    else list.push(s);
    if (await save(list, `Saved. The AP fetches the weather for ${s.place || s.id} now.`)) {
      closeForm();
      setTimeout(load, 5000);
    }
  });

  return {
    // The dashboard connected to an AP (or disconnected: null)
    connected(c) {
      client = c;
      data = null;
      closeForm();
      $("src-box").hidden = true;
      clearTimeout(timer);
      if (c) load();
    },
  };
}
