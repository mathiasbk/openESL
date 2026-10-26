// tplpicker.js - "New from template…" in the designer (editor.js): a dialog that lists the
// template catalog (templates/, see catalog.js) by category, draws each template on the current
// tag model with the renderer and its sample text, and adds the chosen one to the layout file.
//
// The catalog is the one published next to the page (web/build.py), else the one on GitHub
// Pages. The AP's own designer has no catalog: it reads GitHub Pages (which allows that), and
// without internet offers the example layouts the AP carries instead.
//
// A template is only added when the file can take it: the file with it added is compiled by the
// renderer first (the AP's compiler with the limits the editor uses), and a template that does
// not fit is refused with the compiler's reason.

import { loadRenderer, MODELS, LAYOUT_FILE } from "./renderer.js";
import { PAGES, variantFor, soloFile, addTemplate, search } from "./catalog.js";

const $ = (id) => document.getElementById(id);
const ISSUES = "https://github.com/mathiasbk/openESL/issues/new";
const REPO = "https://github.com/mathiasbk/openESL/blob/master/";

// opts (from editor.js): model(), doc(), renderer(), onAp, exampleDoc(model),
// add({ doc, index, name }, template, model), closed()
let opts = null;
const ui = { cat: "all", all: false, query: "", open: null };
let catalog = null;        // { index, base, offline } once loaded
const files = {};          // template id -> template (the file)
let observer = null;

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

const modelName = (m) => m + '"';

// ---------- the catalog ----------

async function fetchJson(url) {
  const r = await fetch(url, { cache: "no-cache" });
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return r.json();
}

// The example layouts of the AP (or of the page) as a catalog of their own, for when there is no
// internet: one category, each layout a template for the current model only
async function offlineCatalog() {
  const model = opts.model();
  const d = await opts.exampleDoc(model);
  const templates = (d.layouts || []).map((l) => {
    const t = { id: "example-" + l.name, title: l.name, description: l.description || "", fields: {}, variants: { [model]: { layout: l } } };
    for (const f of l.fields || []) t.fields[f] = "";
    // only the pictures this layout uses: each one costs room in the file
    const used = (l.elements || []).filter((e) => e.type === "icon" && d.icons?.[e.icon]).map((e) => e.icon);
    if (used.length) t.variants[model].icons = Object.fromEntries(used.map((n) => [n, d.icons[n]]));
    t.example = true;      // not a template of the library: no "_template" on the layout
    files[t.id] = t;
    return { id: t.id, category: "examples", title: l.name, description: t.description, tags: [], fields: l.fields || [], layout: l.name, models: { [model]: {} } };
  });
  return {
    offline: true,
    index: { categories: [{ id: "examples", title: "Example file", description: "The example layouts this designer carries" }], templates },
  };
}

async function loadCatalog() {
  if (catalog && !catalog.offline) return catalog;
  const bases = opts.onAp ? [] : [new URL("../templates/", import.meta.url).href];
  bases.push(PAGES + "templates/");
  for (const base of bases) {
    try {
      const index = await fetchJson(base + "index.json");
      if (index?.format === "esl-template-index" && Array.isArray(index.templates)) return (catalog = { index, base });
    } catch { /* the next one */ }
  }
  return (catalog = await offlineCatalog());
}

async function templateFile(entry) {
  if (!files[entry.id]) files[entry.id] = await fetchJson(catalog.base + entry.file);
  return files[entry.id];
}

// A template by id (its file), for the offer to add it for another model; null when the catalog
// cannot be read or has no such template
export async function templateById(id) {
  try {
    const c = await loadCatalog();
    const entry = c.index.templates.find((t) => t.id === id);
    return entry ? { entry, t: await templateFile(entry) } : null;
  } catch {
    return null;
  }
}

// ---------- drawing and the fit check ----------

// Draws the template's variant (for `model`, or another model's when it has none) with its
// sample text into a canvas. Uses the renderer's layout slot: the editor compiles its file again
// when the dialog closes.
async function drawPreview(canvas, t, model) {
  if (!$("tpl-dlg").open) return;      // the editor has the renderer again once the dialog is closed
  const m = variantFor(t, model) ? model : Object.keys(t.variants).find((k) => MODELS[k] && variantFor(t, k));
  if (!m) return;
  const r = await loadRenderer(m);
  const res = r.loadLayouts(JSON.stringify(soloFile(t, m)));
  if (!res.ok) return;
  const out = r.render(LAYOUT_FILE, 0, sampleOf(variantFor(t, m).layout));
  if (!out.ok) return;
  const { width: w, height: h } = r;
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d");
  const img = ctx.createImageData(w, h);
  for (let i = 0; i < w * h; i++) {
    const v = out.pixels[i];
    img.data.set(v === 2 ? [200, 30, 30, 255] : v ? [17, 17, 17, 255] : [255, 255, 255, 255], i * 4);
  }
  ctx.putImageData(img, 0, 0);
  canvas.classList.add("drawn");
}

// The layout's sample text, or its field names for a layout without one (an example layout)
function sampleOf(l) {
  return l._sample || (l.fields || []).map((f) => f.charAt(0).toUpperCase() + f.slice(1).replace(/_/g, " "));
}

// The names of the example file's layouts, by model (filled when the dialog opens), so a full
// file can be told which of its layouts are only the examples
const examples = {};

// What to delete to make room: the example layouts the file still has, by name
function makeRoom(doc, model) {
  const ex = (doc.layouts || []).map((l) => l.name).filter((n) => examples[model]?.has(n));
  return ex.length
    ? `Delete layouts you do not use first, such as the example layouts ${ex.slice(0, 4).join(", ")}${ex.length > 4 ? " …" : ""}.`
    : "Delete a layout you do not use first.";
}

// Whether the current file can take the template: { ok, added, size, json } or { ok: false, why }
export function fitCheck(t, model) {
  if (!variantFor(t, model)) return { ok: false, why: `There is no ${modelName(model)} version of this template yet.` };
  const r = opts.renderer();
  const doc = opts.doc();
  const now = r.loadLayouts(JSON.stringify(doc));
  if (!now.ok) return { ok: false, why: "Your layout file has an error already: " + now.error };
  if ((doc.layouts || []).length >= 16) return { ok: false, why: "The layout file has 16 layouts, the most it can hold. " + makeRoom(doc, model) };
  const added = addTemplate(doc, t, model);
  // the JSON limit usually fills first, and the compiler's message for it says nothing about what to do
  const json = new TextEncoder().encode(JSON.stringify(added.doc)).length;
  if (json > r.limits.jsonBytes)
    return { ok: false, why: `With it the layout file would be ${json} bytes of JSON, more than the AP takes (${r.limits.jsonBytes}). ` + makeRoom(doc, model) };
  const res = r.loadLayouts(JSON.stringify(added.doc));
  if (!res.ok) return { ok: false, why: `The AP would refuse the layout file with it: ${res.error}. ` + makeRoom(doc, model) };
  return { ok: true, added, size: res.size, json };
}

// ---------- the dialog ----------

function usageText(u) {
  if (!u || u.bytes === undefined) return "";
  const parts = [`${u.bytes} bytes compiled`, `${u.runs} text lines`, `${u.rects} ${u.rects === 1 ? "box" : "boxes"}`];
  if (u.lines) parts.push(`${u.lines} lines`);
  if (u.icons) parts.push(`${u.icons} ${u.icons === 1 ? "icon" : "icons"}`);
  if (u.barcodes) parts.push("a barcode");
  if (u.qrs) parts.push("a QR code");
  return parts.join(", ");
}

function badges(entry, model) {
  const b = el("span", "tpl-models");
  for (const m of Object.keys(MODELS)) {
    const has = !!entry.models?.[m];
    const s = el("span", "tpl-badge" + (has ? "" : " off") + (m === model ? " cur" : ""), modelName(m));
    s.title = has ? `Made for the ${modelName(m)}` : `No ${modelName(m)} version yet`;
    b.append(s);
  }
  return b;
}

function refreshCats() {
  const { index } = catalog;
  const model = opts.model();
  const shown = index.templates.filter((t) => ui.all || t.models?.[model]);
  const cats = [{ id: "all", title: "All" }, ...index.categories];
  $("tpl-cats").replaceChildren(...cats.map((c) => {
    const n = shown.filter((t) => c.id === "all" || t.category === c.id).length;
    const b = el("button", "chip" + (ui.cat === c.id ? " on" : ""));
    b.type = "button";
    b.setAttribute("role", "tab");
    b.setAttribute("aria-selected", String(ui.cat === c.id));
    b.title = c.description || "";
    b.append(c.title + " ", el("span", "n", String(n)));
    b.addEventListener("click", () => {
      ui.cat = c.id;
      refreshCats();
      refreshGrid();
    });
    return b;
  }));
}

function refreshGrid() {
  const { index } = catalog;
  const model = opts.model();
  observer?.disconnect();
  let list = index.templates.filter((t) => ui.cat === "all" || t.category === ui.cat);
  list = search(list, ui.query);
  const avail = list.filter((t) => t.models?.[model]);
  const other = ui.all ? list.filter((t) => !t.models?.[model]) : [];
  const grid = $("tpl-grid");
  const cards = [...avail, ...other].map((entry) => card(entry, model));
  if (!cards.length) {
    const p = el("p", "hint tpl-empty", ui.query ? `No template matches “${ui.query}”.` : `No template in this category for the ${modelName(model)} yet.`);
    if (!ui.all && index.templates.some((t) => !t.models?.[model])) p.append(" Turn on “All models” to see the others.");
    grid.replaceChildren(p);
    return;
  }
  grid.replaceChildren(...cards);
  // draw the previews as their cards come into view
  observer = new IntersectionObserver((seen) => {
    for (const s of seen) {
      if (!s.isIntersecting) continue;
      observer.unobserve(s.target);
      const entry = index.templates.find((t) => t.id === s.target.dataset.id);
      templateFile(entry).then((t) => drawPreview(s.target.querySelector("canvas"), t, model)).catch(() => {
        s.target.querySelector(".tpl-shot").classList.add("failed");
      });
    }
  }, { root: grid, rootMargin: "200px" });
  grid.querySelectorAll(".tpl-card").forEach((c) => observer.observe(c));
}

function card(entry, model) {
  const has = !!entry.models?.[model];
  const c = el("button", "tpl-card" + (has ? "" : " off"));
  c.type = "button";
  c.dataset.id = entry.id;
  const shot = el("span", "tpl-shot");
  shot.append(el("canvas"));
  c.append(shot, el("b", "tpl-title", entry.title), el("span", "tpl-desc", entry.description));
  const foot = el("span", "tpl-foot");
  foot.append(badges(entry, model));
  if (!has) foot.append(el("span", "tpl-na", `not available for ${modelName(model)}`));
  c.append(foot);
  c.title = has ? "Details and add" : `This template has no ${modelName(model)} version yet`;
  c.addEventListener("click", () => openDetail(entry));
  return c;
}

async function openDetail(entry) {
  const model = opts.model();
  ui.open = entry.id;
  $("tpl-list").hidden = true;
  const box = $("tpl-detail");
  box.hidden = false;
  box.replaceChildren(el("p", "hint", "Loading…"));
  let t;
  try {
    t = await templateFile(entry);
  } catch (e) {
    box.replaceChildren(el("p", "hint error", "Could not load the template: " + e.message), backButton());
    return;
  }
  if (ui.open !== entry.id) return;
  const v = variantFor(t, model);
  const shotModel = v ? model : Object.keys(t.variants).find((k) => MODELS[k] && variantFor(t, k)) || model;
  const shot = el("div", "tpl-big");
  shot.style.maxWidth = MODELS[shotModel].width * 2 + 24 + "px";
  const canvas = el("canvas");
  shot.append(canvas);

  const info = el("div", "tpl-info");
  const h = el("h4", "tpl-h", entry.title);
  info.append(h, el("p", "tpl-text", entry.description), badges(entry, model));
  if (!v) info.append(el("p", "hint", `Shown on the ${modelName(shotModel)}: there is no ${modelName(model)} version yet.`));
  const lay = (v || variantFor(t, shotModel))?.layout;
  // the fields, in the order a client sends them
  const fl = el("dl", "tpl-fields");
  for (const [i, f] of (lay?.fields || entry.fields).entries()) {
    const dt = el("dt");
    dt.append(el("code", "", f));
    const dd = el("dd", "", t.fields?.[f] || "");
    const s = lay?._sample?.[i];
    if (s && s.trim()) dd.append(el("span", "muted small", (t.fields?.[f] ? " · e.g. " : "e.g. ") + s));
    fl.append(dt, dd);
  }
  info.append(el("h5", "sub-h", `Fields (layout “${lay?.name || entry.layout}”)`), fl);
  const note = (v || variantFor(t, shotModel))?.note;
  if (note) info.append(el("p", "hint", "Note: " + note));
  const use = usageText(entry.models?.[v ? model : shotModel]);
  if (use) info.append(el("p", "hint", "Uses " + use + "."));
  if (t.docs) {
    const a = el("a", "", "How to fill it: " + t.docs.split("/").pop());
    a.href = REPO + t.docs;
    a.target = "_blank";
    a.rel = "noopener";
    const p = el("p", "small");
    p.append(a);
    info.append(p);
  }

  const status = el("p", "tpl-fit", "");
  status.setAttribute("role", "status");
  const add = el("button", "primary", `Add to the ${modelName(model)} layout file`);
  add.type = "button";
  const buttons = el("div", "dlg-buttons");
  buttons.append(backButton(), el("span", "sep"));
  if (!v) {
    const a = el("a", "button", `Ask for a ${modelName(model)} version`);
    a.href = `${ISSUES}?title=${encodeURIComponent(`Template ${t.id}: a ${model}" version`)}&labels=enhancement`;
    a.target = "_blank";
    a.rel = "noopener";
    buttons.append(a);
  } else {
    buttons.append(add);
  }
  box.replaceChildren(shot, info, status, buttons);
  await drawPreview(canvas, t, shotModel);
  if (!v) return;
  const fit = fitCheck(t, model);
  opts.redraw();
  if (!fit.ok) {
    status.className = "tpl-fit error";
    status.textContent = "Does not fit: " + fit.why;
    add.disabled = true;
    return;
  }
  // the limit that comes closest to full
  const lim = opts.renderer().limits;
  const [n, max, what] = fit.json / lim.jsonBytes > fit.size / lim.fileBytes
    ? [fit.json, lim.jsonBytes, "bytes of JSON"] : [fit.size, lim.fileBytes, "bytes compiled"];
  status.className = "tpl-fit ok";
  status.textContent = `Fits: the file becomes ${n} of the ${max} ${what} the AP takes (${Math.round((100 * n) / max)} %).`;
  add.addEventListener("click", () => {
    const again = fitCheck(t, model);     // the file may have changed since
    if (!again.ok) {
      status.className = "tpl-fit error";
      status.textContent = "Does not fit: " + again.why;
      add.disabled = true;
      return;
    }
    $("tpl-dlg").close();
    opts.add(again.added, t, model);
  });
}

function backButton() {
  const b = el("button", "", "← All templates");
  b.type = "button";
  b.addEventListener("click", closeDetail);
  return b;
}

function closeDetail() {
  ui.open = null;
  $("tpl-detail").hidden = true;
  $("tpl-list").hidden = false;
  opts.redraw();
}

// show: the id of a template to open the details of (the offer after a change of model), with
// the search and category cleared
export async function openTemplatePicker(show) {
  const dlg = $("tpl-dlg");
  if (show) {
    ui.query = "";
    ui.cat = "all";
    $("tpl-search").value = "";
  }
  ui.open = null;
  $("tpl-detail").hidden = true;
  $("tpl-list").hidden = false;
  $("tpl-model").textContent = modelName(opts.model());
  $("tpl-grid").replaceChildren(el("p", "hint", "Loading the templates…"));
  $("tpl-cats").replaceChildren();
  if (!dlg.open) dlg.showModal();
  await loadCatalog();
  if (!catalog.offline && catalog.index.categories.every((c) => c.id !== ui.cat)) ui.cat = "all";
  if (catalog.offline) ui.cat = "all";
  const note = $("tpl-note");
  note.hidden = !catalog.offline;
  note.textContent = catalog.offline
    ? "The template library needs internet (it is on GitHub Pages). Until then, here are the example layouts this designer carries."
    : "";
  refreshCats();
  refreshGrid();
  const model = opts.model();
  if (!examples[model]) {
    try {
      examples[model] = new Set(((await opts.exampleDoc(model)).layouts || []).map((l) => l.name));
    } catch {
      examples[model] = new Set();
    }
  }
  const entry = show && catalog.index.templates.find((t) => t.id === show);
  if (entry) openDetail(entry);
}

export function initTemplatePicker(o) {
  opts = o;
  $("tpl-search").addEventListener("input", () => {
    ui.query = $("tpl-search").value;
    if (!catalog) return;
    closeDetail();
    refreshGrid();
  });
  $("tpl-all").addEventListener("change", () => {
    ui.all = $("tpl-all").checked;
    if (!catalog) return;
    refreshCats();
    refreshGrid();
  });
  $("tpl-dlg").addEventListener("close", () => {
    observer?.disconnect();
    if (catalog?.offline) catalog = null;    // try the internet again next time
    opts.redraw();
  });
}
