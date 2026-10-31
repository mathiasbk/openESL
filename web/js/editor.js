// editor.js - the layout designer. Edits a layout file (docs/protokoll.md part 3) for one tag
// model and draws it live with the WebAssembly renderer, which is the AP's own drawing code
// and compiler: what the preview shows is the image the tag gets, and an error here
// is the 422 the AP would answer when the file is uploaded.
//
// The page: the layouts of the file and the elements of the one picked on the left, the
// canvas in the middle with a bar by the selected element for how it looks, and the layout's
// fields with their sample text, where the selected element is and what it shows on the right.

import { History } from "./history.js";
import { layoutUsage } from "./limits.js";
import { snapMove, resizeBox } from "./snap.js";
import { ApClient, PortClient, apBase } from "./apclient.js";
import { hasSerial, serialAp, connectSerial, onSerialChange } from "./apport.js";
import { apPageUrl, layoutsMessage, READY, SAVED } from "./handoff.js";
import { compareLayouts, tagLayouts, countByLayout } from "./layoutinfo.js";
import { initTemplatePicker, openTemplatePicker, templateById } from "./tplpicker.js";
import { freeName } from "./catalog.js";
import { layoutPanel } from "./layoutapi.js";
import { layoutModel, layoutUsers } from "./models.js";
import { loadRenderer, MODELS, LAYOUT_PRICE, LAYOUT_TEXT, LAYOUT_FILE, FLAG_PRICE_HIGHLIGHT, FLAG_FRAME } from "./renderer.js";

const FONTS = ["small", "text20", "medium", "mid", "price44", "large"];
const FONT_PX = { small: 17, medium: 24, text20: 20, mid: 36, price44: 44, large: 52 };
const TEXT_COLORS = ["black", "white", "invert", "highlight", "frame"];
const RECT_COLORS = ["black", "white", "frame"];
const LINE_COLORS = ["black", "white"];      // lines and icons
// On a model with red (colors in shared/models.json) the AP also takes these
const TEXT_COLORS_RED = [...TEXT_COLORS, "red", "red_invert"];
const RECT_COLORS_RED = [...RECT_COLORS, "red"];
const LINE_COLORS_RED = [...LINE_COLORS, "red"];
const ICON_MAX = 128;                         // ESL_ICON_MAX_W / _H in shared/proto.h
const ICON_FILE_MAX = 16;                     // ESL_LBIN_ICON_MAX: icons in one layout file
const COLOR_NAMES = {
  black: "Black", white: "White", invert: "Inverted", highlight: "Highlight",
  frame: "Frame", red: "Red", red_invert: "Red inverted",
};
const COLOR_HINTS = {
  invert: "White text in a black box", highlight: "Inverted when the update has price_highlight, otherwise black",
  frame: "Drawn only when the update has frame", red_invert: "White text in a red box",
};
const SWATCH = {
  black: "#111", white: "#fff", invert: "linear-gradient(135deg, #111 50%, #fff 50%)",
  highlight: "linear-gradient(135deg, #111 50%, #888 50%)", frame: "transparent", red: "#c81e1e",
  red_invert: "linear-gradient(135deg, #c81e1e 50%, #fff 50%)",
};
const BUILTIN = {
  price: { id: LAYOUT_PRICE, fields: ["name", "price", "subtitle"] },
  text: { id: LAYOUT_TEXT, fields: ["line1", "line2", "line3", "line4"] },
};
const STORE = "openesl.editor.";
const DASH_URL = "openesl.dash.url";   // the Dashboard tab's AP address, shared with Send to your AP
const DASH_TOKEN = "openesl.dash.token"; // and its token, which the AP's own page shares

const $ = (id) => document.getElementById(id);
const store = {
  get(k) { try { return localStorage.getItem(STORE + k); } catch { return null; } },
  set(k, v) { try { localStorage.setItem(STORE + k, v); } catch { /* private mode */ } },
};

// On the AP's own page (js/apdesign.js, ap/main/webui.py): the AP's API is this page's
// origin, and the layout files stored on the AP are what the editor opens and saves
let onAp = false;

const state = {
  model: "2.9",
  docs: {},            // model -> layout file object
  sel: "file:0",       // "file:<n>" or "builtin:<name>"
  el: -1,              // selected element index
  samples: {},         // "<model>/<layout name>" -> [values]
  renderer: null,
  history: {},         // model -> History of the layout file
  dragging: false,
};

function emptyDoc(model) {
  const { width, height } = MODELS[model];
  return {
    format: "esl-layouts", version: 1, model,
    layouts: [{
      name: "my-label", fields: ["name", "price"],
      elements: [
        { type: "text", field: "name", x: 4, y: 4, w: width - 8, h: 30, fonts: ["medium", "small"], lines: 2 },
        { type: "text", field: "price", x: 4, y: height - 60, w: width - 8, h: 56, fonts: ["large", "mid", "medium"], align: "right", valign: "bottom", color: "highlight" },
      ],
    }],
  };
}

const doc = () => state.docs[state.model];
const isBuiltin = () => state.sel.startsWith("builtin:");
const layout = () => (isBuiltin() ? null : doc().layouts[+state.sel.slice(5)] || null);
const element = () => (layout() && layout().elements[state.el]) || null;
const fieldNames = () => (isBuiltin() ? BUILTIN[state.sel.slice(8)].fields : layout()?.fields || []);
const sampleKey = () => state.model + "/" + (isBuiltin() ? state.sel : layout()?.name);
// Text and barcode elements show a field from each update; the rest are part of the layout
const usesField = (e) => (e.type === "text" && e.text === undefined) || e.type === "barcode" || e.type === "qr" ||
  (e.type === "icon" && e.field !== undefined);
// An icon element names its picture (fixed size), or takes the picture a field's text names, in a box
const iconByField = (e) => e.type === "icon" && e.field !== undefined;
const layoutName = () => (isBuiltin() ? state.sel.slice(8) : layout()?.name);

function defaultSample(name) {
  if (/price|pris/i.test(name)) return "49,90";
  if (/unit|enhet/i.test(name)) return "kr 99,80/kg";
  if (/ean|gtin|barcode|strek|code/i.test(name)) return "7038010009457";
  return name.charAt(0).toUpperCase() + name.slice(1).replace(/_/g, " ");
}

// The sample values of the current layout. Always the same array for a layout (changed in
// place), so the inputs that edit it stay connected to what is drawn.
function samples() {
  const k = sampleKey();
  const names = fieldNames();
  const s = (state.samples[k] ||= [...(layout()?._sample || [])]);
  while (s.length < names.length) s.push(defaultSample(names[s.length]));
  s.length = names.length;
  return s;
}

// ---------- persistence ----------

function save() {
  store.set("doc." + state.model, JSON.stringify(doc()));
  store.set("model", state.model);
  store.set("samples", JSON.stringify(state.samples));
  scheduleRecord();
  if (ap.connected) refreshLayoutList();   // a layout changed here is no longer the AP's
  refreshFileState();
}

// The file as it was last saved to a file, opened, or stored on the AP, so the head can say
// when the editor has changes that are only in this browser. On the AP's own page it is the
// file the AP has, so the head says what is not saved to the AP.
function markClean() {
  store.set("clean." + state.model, JSON.stringify(doc()));
  refreshFileState();
}

function refreshFileState() {
  const clean = store.get("clean." + state.model);
  const fs = $("file-state");
  const users = layoutUsers(state.model);
  fs.replaceChildren(`Layout file for ${users.join('" and ')}"`);
  if (users.length > 1) fs.title = `The ${users.join('", ')}" tags have the same screen and use this one file`;
  else fs.removeAttribute("title");
  if (clean !== JSON.stringify(doc())) {
    const d = document.createElement("span");
    d.className = "dirty";
    d.textContent = onAp ? " · ● not saved to the AP" : " · ● changes only in this browser";
    d.title = onAp
      ? "Kept in this browser. Save to AP stores it on the AP, which then draws its tags with it."
      : "Kept in this browser. Save the layout file or send it to the AP to keep it elsewhere.";
    fs.append(d);
  }
}

// ---------- undo ----------

// Changes are recorded as one undo step once the editing pauses, so typing a name, a run of
// arrow keys or one drag is a single step.
let recordTimer = 0;

function scheduleRecord() {
  clearTimeout(recordTimer);
  if (!state.dragging) recordTimer = setTimeout(record, 400);
  $("undo").disabled = false;     // a change waiting to be recorded can be undone already
}

function record() {
  clearTimeout(recordTimer);
  const h = state.history[state.model];
  if (h) h.push(JSON.stringify(doc()), { sel: state.sel, el: state.el });
  refreshUndo();
}

function undoRedo(redo) {
  record();
  const h = state.history[state.model];
  const entry = redo ? h.redo() : h.undo();
  if (!entry) return;
  state.docs[state.model] = JSON.parse(entry.snap);
  state.sel = entry.extra?.sel || "file:0";
  if (!isBuiltin() && !layout()) state.sel = "file:0";
  state.el = entry.extra?.el ?? -1;
  if (!layout()?.elements?.[state.el]) state.el = -1;
  refreshAll();
  save();
}

function refreshUndo() {
  const h = state.history[state.model];
  $("undo").disabled = !h?.canUndo;
  $("redo").disabled = !h?.canRedo;
}

async function exampleDoc(model) {
  const r = await fetch(new URL("../designs/layouts/" + model + ".json", import.meta.url));
  if (!r.ok) throw new Error("could not load the example (" + r.status + ")");
  return r.json();
}

async function loadModel(model) {
  model = layoutModel(model); // a model whose sameAs names another is edited as that one's file (#181)
  const from = state.model;
  const template = state.docs[from] && layout()?._template;
  state.model = model;
  if (!state.docs[model]) {
    const saved = store.get("doc." + model);
    try {
      state.docs[model] = saved ? JSON.parse(saved) : await exampleDoc(model);
    } catch {
      state.docs[model] = emptyDoc(model);
    }
    if (!saved && store.get("clean." + model) === null) markClean();
  }
  if (!state.history[model]) {
    state.history[model] = new History();
    state.history[model].reset(JSON.stringify(state.docs[model]));
  }
  state.renderer = await loadRenderer(model);
  applyLimits();
  document.querySelectorAll("#model-tabs button").forEach((b) => b.setAttribute("aria-checked", String(b.dataset.model === model)));
  applyZoom();
  document.querySelectorAll(".model-name").forEach((e) => (e.textContent = model));
  state.sel = doc().layouts?.length ? "file:0" : "builtin:price";
  state.el = -1;
  ap.mac = "";
  refreshAll();
  $("tpl-offer").hidden = true;
  if (template && from !== model) offerVariant(template);
  if (onAp && ap.connected && !(model in ap.files))
    await apRun("Reading the AP's layout file", async () => { const r = await apOpen(model); await apReadFile(model); return r; });
  else if (ap.connected && !(model in ap.files)) await apRun("Reading the AP's layout file", async () => (await apReadFile(model), ["", ""]));
}

// ---------- templates (tplpicker.js) ----------

// A template the picker added: the file with it (its icons too), selected, with its sample text
function addFromTemplate(added, t) {
  state.docs[state.model] = added.doc;
  state.sel = "file:" + added.index;
  state.el = -1;
  state.samples[sampleKey()] = [...(layout()._sample || [])];
  refreshAll();
  save();
  setStatus(`Added “${t.title}” as the layout ${added.name}`, "ok");
}

// After a change of model with a layout from a template selected: offer the template's version
// for this model when the file has none (catalog.js; "_template" is the template's id)
async function offerVariant(id) {
  const bar = $("tpl-offer");
  bar.hidden = true;
  if (doc().layouts?.some((l) => l._template === id)) return;
  const found = await templateById(id);
  const model = state.model;
  if (!found || model !== state.model || !found.t.variants?.[model]) return;
  // a layout of the same name without "_template" (from the example file, or added by hand) is
  // most likely the same one
  if (doc().layouts?.some((l) => l.name === found.entry.layout && !l._template)) return;
  $("tpl-offer-text").textContent = `The ${model}" file has no “${found.t.title}” yet.`;
  $("tpl-offer-add").textContent = `Add its ${model}" version`;
  $("tpl-offer-add").onclick = () => {
    bar.hidden = true;
    openTemplatePicker(id);
  };
  bar.hidden = false;
}

// ---------- drawing ----------

function compileAndDraw() {
  const r = state.renderer;
  applyLimits();
  const res = r.loadLayouts(JSON.stringify(doc()));
  let layoutId;
  if (isBuiltin()) {
    layoutId = BUILTIN[state.sel.slice(8)].id;
  } else if (!res.ok) {
    setStatus("The AP would refuse the file: " + res.error, "error");
    refreshMeters(res);
    dimCanvas();
    return;
  } else {
    layoutId = LAYOUT_FILE + +state.sel.slice(5);
  }
  const out = r.render(layoutId, flags(), samples());
  if (!out.ok) {
    setStatus(out.error, "error");
    dimCanvas();
    return;
  }
  paint(out.pixels, r.width, r.height);
  refreshMeters(res);
  if (res.ok) scheduleThumbs();
  if (out.refused) {
    setStatus("The AP would refuse this sample text: " + out.refused, "error");
  } else if (res.ok) {
    setStatus("✓ The AP accepts this layout", "ok");
    $("status").title = `Layout file version ${res.version.toString(16).toUpperCase().padStart(8, "0")}, ${res.size} bytes compiled`;
  } else setStatus("Built-in layout (the layout file has an error: " + res.error + ")", "error");
}

const flags = () => ($("opt-highlight").checked ? FLAG_PRICE_HIGHLIGHT : 0) | ($("opt-frame").checked ? FLAG_FRAME : 0);

// What the layout uses of the AP's limits, so you see a limit coming before the AP says no.
// The status bar shows the fullest one; the list opens from it.
function refreshMeters(res) {
  const r = state.renderer;
  const lim = r.limits;
  const lay = layout();
  const meters = [];
  $("el-add-barcode").disabled = false;
  $("el-add-qr").disabled = false;
  if (lay) {
    const u = layoutUsage(lay, lim);
    meters.push(["Text lines", u.runs, lim.runs, "Lines of text the AP draws at most for one screen of this layout (each text box counts the most lines its fonts can fit)"]);
    meters.push(["Boxes", u.rects, lim.rects, "Boxes and inverted or highlighted text in this layout"]);
    if (u.lines) meters.push(["Lines", u.lines, lim.lines, "Line elements in this layout"]);
    if (u.icons) meters.push(["Icons", u.icons, lim.icons, "Icon elements in this layout"]);
    if (u.barcodes) meters.push(["Barcodes", u.barcodes, lim.barcodes, "Barcodes in this layout"]);
    $("el-add-barcode").disabled = u.barcodes >= lim.barcodes;
    $("el-add-barcode").title = u.barcodes >= lim.barcodes ? "A layout has at most one barcode" : "A field as a Code 128 or EAN-13 barcode (one per layout)";
    $("el-add-qr").disabled = u.qrs >= lim.qrs;
    $("el-add-qr").title = u.qrs >= lim.qrs ? "A layout has at most one QR code" : "A field as a QR code, e.g. a link (one per layout)";
    if (u.qrs) meters.push(["QR codes", u.qrs, lim.qrs, "QR codes in this layout"]);
  }
  meters.push(["Compiled file", res.ok ? res.size : null, lim.fileBytes, "Size of the whole compiled layout file the AP draws with, in bytes"]);
  meters.push(["JSON", new TextEncoder().encode(JSON.stringify(doc())).length, lim.jsonBytes, "Size of the layout file as Send to your AP uploads it (compact JSON), in bytes; it usually fills before the compiled file. Save layout file writes it indented, and esl-client layout-put compacts it again"]);
  if (lay) meters.push(["Fields", lay.fields.length, lim.fields, "Text fields the client sends for this layout, e.g. price and price_ex"]);
  const t = textBudget();
  meters.push(["Sample text", t.n, t.max, "Bytes of text in one update from the AP (the fields share 96 bytes, one per field plus the text)"]);
  $("meters").replaceChildren(...meters.map(([name, n, max, title]) => {
    const m = document.createElement("div");
    m.className = "meter" + (n === null ? "" : n > max ? " over" : n >= max ? " near" : "");
    m.title = title;
    const bar = document.createElement("i");
    bar.style.setProperty("--p", n === null ? "0%" : Math.min(100, (100 * n) / max) + "%");
    const b = document.createElement("b");
    b.textContent = (n === null ? "?" : n) + " / " + max;
    m.append(name, b, bar);
    return m;
  }));
  const worst = Math.max(...meters.map(([, n, max]) => (n === null ? 0 : n / max)));
  const pct = Math.round(100 * worst);
  const l = $("limits");
  l.classList.toggle("over", worst > 1);
  l.classList.toggle("near", worst >= 0.8 && worst <= 1);
  $("limits-ring").style.setProperty("--p", Math.min(100, pct) + "%");
  $("limits-text").textContent = worst > 1 ? "Over the AP's limits" : `${pct} % of the AP's limits`;
  $("limits").title = "What this layout uses of what the AP can draw and store. The AP draws the image; the tag only shows it";
  $("limits-src").textContent = limitsSource();
  if (worst > 1) l.open = true;
}

// Whose limits the meters show: the connected AP's, or the default board's (the smallest)
function limitsSource() {
  const dflt = `Those of an ESP32-C6 AP (${DEFAULT_BOARD}, the smallest)`;
  if (!ap.connected) return dflt + ". Connect under Send to your AP to see your AP's own.";
  if (!ap.limits) return dflt + `: the AP does not report its own (firmware older than this page).`;
  if (!state.renderer.canSetLimits) return dflt + ": this page's renderer is too old to take the AP's.";
  const cut = Object.keys(ap.limits).some((k) => typeof state.renderer.limits[k] === "number" && state.renderer.limits[k] < ap.limits[k]);
  return `Those of the connected AP (${ap.board || "board not named"})` +
    (cut ? ", some lowered to what this page can check" : "") + ".";
}

// The renderer checks against the connected AP's limits, or the default board's without one
// The renderer is shared with the dashboard's Layouts card, which sets its own AP's limits:
// called before each loadLayouts, so the designer checks with the AP it is connected to
function applyLimits() {
  state.renderer?.setLimits(ap.connected ? ap.limits : null);
}

// After connecting or disconnecting: check and draw again with the limits that now apply
function limitsChanged() {
  applyLimits();
  if (state.renderer && doc()) refreshAll();
}

function setStatus(text, cls) {
  const s = $("status");
  s.textContent = text;
  s.className = "status " + (cls || "");
  s.title = "";
}

function paint(pixels, w, h) {
  const c = $("screen");
  if (c.width !== w || c.height !== h) [c.width, c.height] = [w, h];
  const ctx = c.getContext("2d");
  const img = ctx.createImageData(w, h);
  for (let i = 0; i < w * h; i++) {
    const v = pixels[i];
    img.data.set(v === 2 ? [200, 30, 30, 255] : v ? [17, 17, 17, 255] : [255, 255, 255, 255], i * 4);
  }
  ctx.putImageData(img, 0, 0);
  c.style.opacity = 1;
}

function dimCanvas() {
  $("screen").style.opacity = 0.35;
}

// ---------- overlay: boxes you can drag ----------

function drawOverlay() {
  const ov = $("overlay");
  const { width, height } = MODELS[state.model];
  ov.replaceChildren();
  ov.classList.toggle("hide-boxes", !$("opt-boxes").checked);
  const lay = layout();
  if (lay) lay.elements.forEach((e, i) => {
    const b = document.createElement("div");
    b.className = "box" + (e.type === "rect" || e.type === "line" || e.type === "icon" ? " rect" : e.type === "barcode" || e.type === "qr" ? " barcode" : "") +
      (e.type === "icon" && !iconByField(e) ? " fixed-size" : "") + (i === state.el ? " sel" : "");
    Object.assign(b.style, {
      left: (100 * e.x) / width + "%", top: (100 * e.y) / height + "%",
      width: (100 * e.w) / width + "%", height: (100 * e.h) / height + "%",
    });
    b.dataset.i = i;
    const tag = document.createElement("span");
    tag.className = "tag";
    tag.textContent = e.type === "barcode" ? "▥ " + fieldLabel(e.field) : e.type === "qr" ? "▦ " + fieldLabel(e.field) : elementName(e);
    b.append(tag);
    if (e.type !== "icon" || iconByField(e)) for (const h of HANDLES) {
      const g = document.createElement("span");
      g.className = "grip " + h;
      g.dataset.h = h;
      b.append(g);
    }
    ov.append(b);
  });
  refreshList();
  placeBar();
}

// The elements, the one drawn last (in front) at the top, so a box hidden under another can
// still be picked. Drag one to draw it further forward or back.
function refreshList() {
  const ol = $("el-list");
  const lay = layout();
  $("el-head").hidden = ol.hidden = !lay;
  if (!lay) return;
  ol.replaceChildren(...lay.elements.map((e, i) => {
    const li = document.createElement("li");
    li.className = i === state.el ? "sel" : "";
    li.dataset.i = i;
    li.draggable = true;
    const ico = document.createElement("span");
    const inverted = e.type === "text" && ["invert", "highlight", "red_invert"].includes(e.color);
    ico.className = "ico" + (e.type === "rect" || e.type === "line" || e.type === "icon" ? " rect" : inverted ? " inv" : "");
    ico.textContent = { rect: e.radius ? "▢" : "▬", line: e.w > 1 && e.h > 1 ? (e.direction === "up" ? "╱" : "╲") : "─", icon: "◆", barcode: "▥", qr: "▦" }[e.type] ??
      (e.text !== undefined ? "“”" : "T");
    const name = document.createElement("span");
    name.className = "name";
    name.textContent = e.type === "barcode" || e.type === "qr" ? fieldLabel(e.field) : elementName(e);
    const dim = document.createElement("span");
    dim.className = "dim";
    dim.textContent = e.type === "text" ? [(e.fonts || ["small"])[0], e.color && e.color !== "black" ? COLOR_NAMES[e.color]?.split(" ")[0].toLowerCase() : ""].filter(Boolean).join(", ")
      : e.type === "barcode" ? (e.symbology === "ean13" ? "EAN-13" : "Code 128") : e.type === "qr" ? "QR " + (e.ecc || "M") : e.color && e.color !== "black" ? e.color : "";
    li.title = `x ${e.x}, y ${e.y}, ${e.w} × ${e.h} px`;
    li.append(ico, name, dim);
    return li;
  }).reverse());
}

function rectName(e) {
  let kind;
  if (iconByField(e)) kind = "icon named by " + e.field;
  else if (e.type === "icon") kind = "icon " + e.icon;
  else if (e.type === "line") kind = e.h === 1 ? "line" : e.w === 1 ? "vertical line" : "slanted line";
  else kind = e.h <= 2 && e.w > e.h ? "line" : e.w <= 2 && e.h > e.w ? "vertical line" : e.border ? "frame box" : "box";
  return kind + (e.color && e.color !== "black" ? ` (${e.color})` : "");
}

function elementName(e) {
  if (e.type === "barcode") return `barcode (${fieldLabel(e.field)})`;
  if (e.type === "qr") return `QR code (${fieldLabel(e.field)})`;
  if (e.type !== "text") return rectName(e);
  return e.text !== undefined ? `"${e.text}"` : fieldLabel(e.field);
}

function initList() {
  const ol = $("el-list");
  ol.addEventListener("click", (ev) => {
    const li = ev.target.closest("li");
    if (!li) return;
    state.el = +li.dataset.i;
    refreshElement();
    // On a narrow screen the list is under the canvas: bring the tag back into view
    if (narrow.matches) $("stage").scrollIntoView({ block: "center", behavior: "smooth" });
  });
  const hover = (i) => document.querySelectorAll("#overlay .box").forEach((b) => b.classList.toggle("hover", b.dataset.i === i));
  ol.addEventListener("pointerover", (ev) => hover(ev.target.closest("li")?.dataset.i));
  ol.addEventListener("pointerleave", () => hover(null));
  // Drag to reorder. The list shows the front at the top, so "above" means drawn later.
  let from = -1;
  const clear = () => ol.querySelectorAll("li").forEach((l) => l.classList.remove("drop-before", "drop-after"));
  ol.addEventListener("dragstart", (ev) => {
    from = +ev.target.closest("li").dataset.i;
    ev.dataTransfer.effectAllowed = "move";
  });
  ol.addEventListener("dragover", (ev) => {
    const li = ev.target.closest("li");
    if (from < 0 || !li) return;
    ev.preventDefault();
    clear();
    const r = li.getBoundingClientRect();
    li.classList.add(ev.clientY < r.top + r.height / 2 ? "drop-before" : "drop-after");
  });
  ol.addEventListener("dragleave", (ev) => { if (!ol.contains(ev.relatedTarget)) clear(); });
  ol.addEventListener("drop", (ev) => {
    const li = ev.target.closest("li");
    ev.preventDefault();
    if (from < 0 || !li) return;
    const above = li.classList.contains("drop-before");
    clear();
    const els = layout().elements;
    let to = +li.dataset.i + (above ? 1 : 0);   // index in drawing order to insert before
    const [e] = els.splice(from, 1);
    if (to > from) to--;
    els.splice(to, 0, e);
    state.el = to;
    from = -1;
    changed();
  });
  ol.addEventListener("dragend", () => { from = -1; clear(); });
}

function drawGuides(guides) {
  const ov = $("overlay");
  const { width, height } = MODELS[state.model];
  ov.querySelectorAll(".guide").forEach((g) => g.remove());
  for (const g of guides) {
    const d = document.createElement("div");
    d.className = "guide " + g.axis;
    if (g.axis === "x") d.style.left = (100 * g.at) / width + "%";
    else d.style.top = (100 * g.at) / height + "%";
    ov.append(d);
  }
}

const HANDLES = ["nw", "n", "ne", "e", "se", "s", "sw", "w"];

function fieldLabel(f) {
  return typeof f === "number" ? layout()?.fields[f] ?? String(f) : String(f);
}

function initDrag() {
  const ov = $("overlay");
  let drag = null;
  ov.addEventListener("pointerdown", (ev) => {
    closeMenus();
    const box = ev.target.closest(".box");
    if (!box) {
      state.el = -1;
      refreshElement();
      return;
    }
    state.el = +box.dataset.i;
    const e = element();
    const rect = ov.getBoundingClientRect();
    drag = {
      resize: ((element().type !== "icon" || iconByField(element())) && ev.target.dataset.h) || null,
      sx: ev.clientX, sy: ev.clientY,
      scale: rect.width / MODELS[state.model].width,
      start: { ...e },
    };
    state.dragging = true;
    ov.setPointerCapture(ev.pointerId);
    refreshElement();
    ev.preventDefault();
  });
  ov.addEventListener("pointermove", (ev) => {
    if (!drag) return;
    const e = element();
    const { width, height } = MODELS[state.model];
    const dx = Math.round((ev.clientX - drag.sx) / drag.scale);
    const dy = Math.round((ev.clientY - drag.sy) / drag.scale);
    const s = drag.start;
    // Snap to the screen and the other boxes within about 6 screen pixels; Alt turns it off.
    const near = ev.altKey ? -1 : Math.max(1, Math.round(6 / drag.scale));
    const others = layout().elements.filter((o) => o !== e);
    const size = { w: width, h: height };
    let guides = [];
    if (drag.resize) {
      const r = resizeBox(s, drag.resize, dx, dy, others, size, near);
      Object.assign(e, { x: r.x, y: r.y, w: r.w, h: r.h });
      guides = r.guides;
    } else {
      const r = snapMove({ x: s.x + dx, y: s.y + dy, w: s.w, h: s.h }, others, size, near);
      e.x = clamp(r.x, 0, width - s.w);
      e.y = clamp(r.y, 0, height - s.h);
      guides = r.guides;
    }
    changed({ form: true });
    drawGuides(guides);
  });
  const end = () => {
    state.dragging = false;
    drawGuides([]);
    if (drag) save();
    drag = null;
    placeBar();
  };
  ov.addEventListener("pointerup", end);
  ov.addEventListener("pointercancel", end);

  document.addEventListener("keydown", onKey);
}

// Keyboard shortcuts, only while the Design tab is shown and no dialog is open. In a text
// box the browser's own keys win (its undo, arrows, Delete).
function onKey(ev) {
  if ($("tab-design").hidden || document.querySelector("#tab-design dialog[open]")) return;
  if (ev.target.closest("input, textarea, select")) return;
  const mod = ev.ctrlKey || ev.metaKey;
  const key = ev.key.length === 1 ? ev.key.toLowerCase() : ev.key;
  const e = element();
  const { width, height } = MODELS[state.model];
  const step = ev.shiftKey ? 10 : 1;
  const moves = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] };
  if (mod && key === "z") undoRedo(ev.shiftKey);
  else if (mod && key === "y") undoRedo(true);
  else if (mod && key === "d") duplicateElement();
  else if (mod && key === "c" && e && !window.getSelection()?.toString())
    state.clip = { ...structuredClone(e), _icon: e.type === "icon" && !iconByField(e) ? structuredClone(doc().icons?.[e.icon]) : undefined };
  else if (mod && key === "v" && state.clip && layout()) pasteElement();
  else if (mod || ev.altKey) return;
  else if (moves[key] && e) {
    e.x = clamp(e.x + moves[key][0], 0, width - e.w);
    e.y = clamp(e.y + moves[key][1], 0, height - e.h);
    changed({ form: true });
  } else if ((key === "Delete" || key === "Backspace") && e) deleteElement();
  else if (key === "Escape" && (document.querySelector("#tab-design details.menu[open]") || state.el >= 0)) {
    if (document.querySelector("#tab-design details.menu[open]")) closeMenus();
    else {
      state.el = -1;
      refreshElement();
    }
  } else if (key === "Tab" && layout() && ev.target === document.body) {
    const n = layout().elements.length;
    state.el = ((state.el < 0 ? (ev.shiftKey ? 0 : -1) : state.el) + (ev.shiftKey ? n - 1 : 1)) % n;
    refreshElement();
  } else return;
  ev.preventDefault();
}

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

// ---------- menus ----------

function closeMenus(except) {
  document.querySelectorAll("#tab-design details.menu[open], #tab-design details.limits[open]").forEach((d) => {
    if (d !== except && !d.contains(except)) d.open = false;
  });
}

function initMenus() {
  document.addEventListener("pointerdown", (ev) => {
    if (!ev.target.closest("#tab-design details.menu, #tab-design details.limits")) closeMenus();
  });
  $("tab-design").addEventListener("toggle", (ev) => { if (ev.target.open) closeMenus(ev.target); }, true);
  // A click on a plain item closes its menu (the More menu of the element bar stays open)
  $("tab-design").addEventListener("click", (ev) => {
    if (ev.target.closest(".dz-head .menu-list .item")) closeMenus();
  });
}

// ---------- the layouts and the right column ----------

// The layouts of the file, each with a small picture, its fields and, when an AP is connected,
// whether the AP has it as it is and how many of its tags show it. The built-in layouts are
// folded away below.
function refreshLayoutList() {
  const lays = doc().layouts || [];
  const cmp = ap.connected && state.model in ap.files ? compareLayouts(doc(), ap.files[state.model]) : null;
  const counts = cmp ? countByLayout(ap.tags, ap.tagLayouts, state.model) : {};
  $("layout-count").textContent = `${lays.length} of 16`;
  $("layout-list").replaceChildren(...lays.map((l, i) => {
    const key = "file:" + i;
    const li = document.createElement("li");
    li.className = "lay" + (key === state.sel ? " sel" : "");
    li.dataset.sel = key;
    li.title = l.description || "";
    const pic = document.createElement("canvas");
    pic.className = "thumb";
    pic.dataset.i = i;
    paintThumb(pic, thumbs[state.model]?.[i]);
    const t = document.createElement("span");
    t.className = "t";
    const n = document.createElement("span");
    n.className = "name";
    n.textContent = l.name || "(no name)";
    const f = document.createElement("span");
    f.className = "dim";
    const nf = (l.fields || []).length;
    f.textContent = nf === 1 ? "1 field" : nf + " fields";
    t.append(n, f);
    if (cmp) {
      const st = document.createElement("span");
      const s = cmp.states[l.name];
      const c = counts[l.name] || 0;
      st.className = "st " + s;
      st.textContent = s === "ap" ? "On the AP" + (c ? ` · ${c} tag${c === 1 ? "" : "s"}` : "")
        : s === "changed" ? (onAp ? "Changed, not saved" : "Changed, not sent") + (c ? ` · ${c} tag${c === 1 ? "" : "s"}` : "")
        : onAp ? "Not saved to the AP" : "Only in this browser";
      t.append(st);
    }
    const acts = document.createElement("span");
    acts.className = "acts";
    const copy = document.createElement("button");
    copy.textContent = "⧉";
    copy.title = copy.ariaLabel = "Copy this layout";
    copy.dataset.act = "copy";
    const del = document.createElement("button");
    del.textContent = "🗑";
    del.title = del.ariaLabel = lays.length <= 1 ? "The file needs at least one layout" : "Delete this layout";
    del.disabled = lays.length <= 1;
    del.dataset.act = "delete";
    acts.append(infoButton(), copy, del);
    li.append(pic, t, acts);
    return li;
  }));
  const gone = cmp?.gone || [];
  $("layout-gone").hidden = !gone.length;
  $("layout-gone").textContent = gone.length
    ? `On the AP, not in this file: ${gone.join(", ")}. ${onAp ? "Saving" : "Sending"} this file removes ${gone.length === 1 ? "it" : "them"}.`
    : "";
  $("builtin-list").replaceChildren(...Object.keys(BUILTIN).map((name) => {
    const li = document.createElement("li");
    li.className = "builtin" + ("builtin:" + name === state.sel ? " sel" : "");
    li.dataset.sel = "builtin:" + name;
    li.title = "Built in to the firmware; it cannot be edited";
    const n = document.createElement("span");
    n.className = "name";
    n.textContent = name;
    const d = document.createElement("span");
    d.className = "dim";
    d.textContent = BUILTIN[name].fields.length + " fields";
    const acts = document.createElement("span");
    acts.className = "acts";
    acts.append(infoButton());
    li.append(n, d, acts);
    return li;
  }));
  if (isBuiltin()) $("builtin-box").open = true;
}

function infoButton() {
  const b = document.createElement("button");
  b.textContent = "API";
  b.className = "info";
  b.title = b.ariaLabel = "What a tag needs for this layout: send it to a tag, or see the request for curl, esl-client and Home Assistant";
  b.dataset.act = "info";
  return b;
}

// The window with what a client sends for a layout of the file or a built-in one (key as
// state.sel), the same panel as the Dashboard's Layouts card (js/layoutapi.js)
let infoPanel = null;
let infoKey = "";
async function showLayoutInfo(key = infoKey) {
  infoKey = key;
  const dlg = $("lay-api-dlg");
  infoPanel ||= layoutPanel($("lay-api"), { onClose: () => dlg.close() });
  const builtin = key.startsWith("builtin:");
  const l = builtin ? null : doc().layouts?.[+key.slice(5)];
  if (!builtin && !l) return;
  const name = builtin ? key.slice(8) : l.name;
  const fields = builtin ? BUILTIN[name].fields : l.fields || [];
  const sample = builtin ? fields.map(defaultSample) : samplesOf(l);
  let what = [];
  if (l?._template) {
    const t = (await templateById(l._template))?.t;
    if (t) what = fields.map((f) => t.fields?.[f] || "");
  }
  const cmp = !builtin && ap.connected && state.model in ap.files ? compareLayouts(doc(), ap.files[state.model]) : null;
  const st = builtin ? "ap" : cmp?.states[name];
  const pill = builtin ? "Built in" : !ap.connected ? "In this file"
    : st === "ap" ? "On the AP" : st === "changed" ? "Changed, not on the AP" : "Not on the AP";
  const host = onAp ? location.host : "openesl.local";
  const tags = ap.connected ? ap.tags.filter((t) => layoutModel(t.model) === state.model)
    .map((t) => ({ mac: t.mac, shows: ap.tagLayouts[t.mac] === name, shown: t.content_id ? ap.tagLayouts[t.mac] || "?" : "" })) : null;
  infoPanel.show({
    key: state.model + "/" + name, name, state: pill, ok: st === "ap", desc: l?.description || "", fields, sample, what, host, tags,
    send: ap.connected && st === "ap" ? async (mac, c) => {
      const r = await apClient().putContent(mac, c);
      ap.tagLayouts[mac] = name;
      refreshLayoutList();
      showLayoutInfo();
      return r;
    } : null,
    why: !ap.connected ? "Connect to your AP to send to a tag from here."
      : `The AP does not have this version of "${name}" yet. ${onAp ? "Save the file to the AP" : "Send it to the AP"} first.`,
    first: { label: !ap.connected ? "Connect to your AP…" : onAp ? "Save to AP" : "Send to your AP…", run: () => {
      // The Send window and Save to AP work on the selected layout: select this one first
      $("lay-api-dlg").close();
      if (state.sel !== key) {
        state.sel = key;
        state.el = -1;
        refreshAll();
      }
      $(onAp && ap.connected ? "ap-save" : "send-open").click();
    } },
  });
  if (!dlg.open) dlg.showModal();
}

// The small pictures of the layouts, by model: the last image data drawn for each
const thumbs = {};
let thumbTimer = 0;

function paintThumb(c, img) {
  if (!img) return;
  [c.width, c.height] = [img.width, img.height];
  c.getContext("2d").putImageData(img, 0, 0);
}

// Draws every layout of the file with its own sample text, a moment after the editing pauses.
// The renderer has the file loaded (compileAndDraw), so this only renders.
function scheduleThumbs() {
  clearTimeout(thumbTimer);
  thumbTimer = setTimeout(drawThumbs, 250);
}

function drawThumbs() {
  const r = state.renderer;
  if (!r || !doc()) return;
  applyLimits();
  const res = r.loadLayouts(JSON.stringify(doc()));
  if (!res.ok) return;
  const list = (thumbs[state.model] = []);
  (doc().layouts || []).forEach((l, i) => {
    const out = r.render(LAYOUT_FILE + i, 0, samplesOf(l));
    if (!out.ok) return;
    const img = new ImageData(r.width, r.height);
    for (let p = 0; p < r.width * r.height; p++) {
      const v = out.pixels[p];
      img.data.set(v === 2 ? [200, 30, 30, 255] : v ? [17, 17, 17, 255] : [255, 255, 255, 255], p * 4);
    }
    list[i] = img;
  });
  document.querySelectorAll("#layout-list canvas.thumb").forEach((c) => paintThumb(c, list[+c.dataset.i]));
}

// The sample text of any layout of the file, as samples() keeps it for the one shown
function samplesOf(l) {
  const s = [...(state.samples[state.model + "/" + l.name] || l._sample || [])];
  const names = l.fields || [];
  while (s.length < names.length) s.push(defaultSample(names[s.length]));
  s.length = names.length;
  return s;
}

function refreshLayoutForm() {
  const lay = layout();
  const bi = isBuiltin();
  $("lay-name").value = layoutName() || "";
  $("lay-name").disabled = bi;
  $("lay-sub").textContent = bi ? `built-in · ${state.model}"` : `${state.model}"`;
  $("builtin-note").hidden = !bi;
  $("desc-sec").hidden = bi;
  $("add-group").hidden = bi;
  $("field-add").hidden = bi;
  if (lay) $("lay-desc").value = lay.description || "";
}

// The layout's fields, each with its sample value: the name (what the client sends it as) and
// what the preview shows and Send to your AP sends.
function refreshSample() {
  const box = $("sample");
  box.replaceChildren();
  const names = fieldNames();
  const vals = samples();
  const lay = layout();
  const e = element();
  const selField = e && usesField(e) ? fieldLabel(e.field) : null;
  names.forEach((n, i) => {
    const row = document.createElement("div");
    row.className = "row";
    const name = document.createElement("input");
    name.className = "fname" + (n === selField ? " sel" : "");
    name.value = n;
    name.spellcheck = false;
    name.disabled = !lay;
    name.setAttribute("aria-label", `Name of field ${i + 1}`);
    name.title = lay ? "The field's name; the client sends the fields in this order" : "";
    name.addEventListener("change", () => renameField(i, name.value.trim()));
    const inp = document.createElement("input");
    inp.value = vals[i];
    inp.setAttribute("aria-label", `Sample text for ${n}`);
    inp.addEventListener("input", () => {
      vals[i] = inp.value;
      countBytes();
      compileAndDraw();
      save();
    });
    const x = document.createElement("button");
    x.className = "x";
    x.textContent = "✕";
    const used = lay?.elements.some((el) => usesField(el) && fieldLabel(el.field) === n);
    x.disabled = !lay || used || names.length <= 1;
    x.title = x.ariaLabel = used ? `${n} is shown by an element; change or delete that first` : `Remove the field ${n}`;
    if (!lay) x.style.visibility = "hidden";   // keeps its cell in the grid
    x.addEventListener("click", () => removeField(i));
    row.append(name, inp, x);
    box.append(row);
  });
  if (lay) $("field-add").disabled = lay.fields.length >= state.renderer.limits.fields;
  countBytes();
}

function renameField(i, name) {
  const lay = layout();
  const old = lay.fields[i];
  if (!name || name === old) return refreshSample();
  if (lay.fields.includes(name)) {
    setStatus(`There is already a field called ${name}`, "error");
    return refreshSample();
  }
  lay.fields[i] = name;
  for (const e of lay.elements) if (e.field === old) e.field = name;
  changed();
}

function addField() {
  const lay = layout();
  let n = lay.fields.length + 1;
  while (lay.fields.includes("field" + n)) n++;
  lay.fields.push("field" + n);
  changed();
  $("sample").querySelectorAll("input.fname").item(lay.fields.length - 1)?.select();
}

function removeField(i) {
  const lay = layout();
  samples().splice(i, 1);
  lay.fields.splice(i, 1);
  changed();
}

// Bytes of sample text, and the most the AP takes with this many fields: each field
// sent costs a length byte, and empty fields at the end are not sent.
function textBudget() {
  const r = state.renderer;
  const s = samples();
  let sent = s.length;
  while (sent && !s[sent - 1]) sent--;
  return { n: s.reduce((a, x) => a + r.textBytes(x), 0), max: r.fieldSpace - sent };
}

function countBytes() {
  const { n, max } = textBudget();
  const c = $("sample-count");
  const lay = layout();
  c.textContent = (lay ? `${lay.fields.length} of ${state.renderer.limits.fields} fields · ` : "") +
    `${n} of ${max} bytes of text · the client sends the fields in this order`;
  c.classList.toggle("error", n > max);
}

// The selected element in the right column: where it is and what it shows. How it looks is
// in the bar by the element on the canvas (refreshBar).
function refreshElement() {
  const box = $("el-form");
  box.replaceChildren();
  const e = element();
  $("sel-sec").hidden = !e;
  drawOverlay();
  refreshSample();
  refreshBar();
  if (!e) return;
  const lay = layout();
  $("sel-title").textContent = "Selected: " + elementName(e);
  const sel = (label, options, val, onSet) => {
    const l = document.createElement("label");
    l.textContent = label;
    const s = document.createElement("select");
    options.forEach(([v, name]) => s.add(new Option(name, v)));
    s.value = val;
    s.addEventListener("change", () => { onSet(s.value); changed({}); });
    l.append(s);
    box.append(l);
    return s;
  };
  if (e.type === "barcode" || e.type === "qr") {
    sel("Shows", lay.fields.map((f) => [f, "field " + f]), fieldLabel(e.field), (v) => (e.field = v));
  } else if (e.type === "text") {
    // A field filled in by each update, or a fixed text that is part of the layout.
    const FIXED = "\u0000fixed";
    sel("Shows", [...lay.fields.map((f) => [f, "field " + f]), [FIXED, "a fixed text"]], e.text !== undefined ? FIXED : fieldLabel(e.field), (v) => {
      if (v === FIXED) {
        delete e.field;
        e.text ??= "inkl. mva";
      } else {
        delete e.text;
        e.field = v;
      }
    });
    if (e.text !== undefined) {
      const l = document.createElement("label");
      l.textContent = "Text";
      const inp = document.createElement("input");
      inp.value = e.text;
      inp.maxLength = state.renderer.limits.fixedTextBytes;
      inp.title = `The same on every tag with this layout, at most ${inp.maxLength} characters`;
      inp.addEventListener("input", () => {
        e.text = inp.value;
        changed({ keepForm: true });
        $("sel-title").textContent = "Selected: " + elementName(e);
      });
      l.append(inp);
      box.append(l, dateFieldButtons(inp));
    }
  }
  // position and size
  const xywh = document.createElement("div");
  xywh.className = "xywh";
  for (const k of ["x", "y", "w", "h"]) {
    const l = document.createElement("label");
    const s = document.createElement("span");
    s.textContent = k.toUpperCase();
    const inp = document.createElement("input");
    inp.type = "number";
    inp.min = k === "w" || k === "h" ? 1 : 0;
    inp.value = e[k];
    inp.dataset.k = k;
    if (e.type === "icon" && !iconByField(e) && (k === "w" || k === "h")) {
      inp.disabled = true;
      inp.title = "An icon is drawn at its own size";
    }
    inp.setAttribute("aria-label", { x: "Left", y: "Top", w: "Width", h: "Height" }[k] + " in pixels");
    inp.addEventListener("input", () => {
      if (inp.value === "") return;
      e[k] = Math.round(+inp.value);
      changed({ keepForm: true });
    });
    l.append(s, inp);
    xywh.append(l);
  }
  box.append(xywh);
}

// Date fields the AP fills in when it draws a fixed text (esl_clock.h on the AP): a button for
// each puts it in at the cursor, with today's value on hover as the preview draws it
const DATE_FIELDS = [
  ["{weekday}", "the weekday, Thursday"],
  ["{date}", "the date, 29 October 2026"],
  ["{date_short}", "the date, 29.10.2026"],
  ["{week}", "the week number, 44"],
];

function dateFieldButtons(inp) {
  const div = document.createElement("div");
  div.className = "date-fields";
  const p = document.createElement("p");
  p.className = "small muted";
  p.textContent = "Date fields, filled in by the AP in its time zone (Settings, General; UTC until set): " +
    "the tag is redrawn once a day ({week} alone: once a week). An AP from before 0.9 shows them as written.";
  const chips = document.createElement("div");
  chips.className = "chips";
  for (const [f, what] of DATE_FIELDS) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "chip";
    b.textContent = f;
    b.title = "Put in " + what;
    b.addEventListener("click", () => {
      const at = inp.selectionStart ?? inp.value.length, end = inp.selectionEnd ?? at;
      const v = inp.value.slice(0, at) + f + inp.value.slice(end);
      if (v.length > inp.maxLength) {
        setStatus(`${f} does not fit: a fixed text is at most ${inp.maxLength} characters`, "error");
        return;
      }
      inp.value = v;
      inp.focus();
      inp.setSelectionRange(at + f.length, at + f.length);
      inp.dispatchEvent(new Event("input"));
    });
    chips.append(b);
  }
  div.append(chips, p);
  return div;
}

function updatePositionInputs() {
  const e = element();
  if (!e) return;
  document.querySelectorAll("#el-form .xywh input").forEach((inp) => {
    if (document.activeElement !== inp) inp.value = e[inp.dataset.k];
  });
}

// ---------- the bar by the selected element ----------

function refreshBar() {
  const bar = $("el-bar");
  const e = element();
  const openMore = bar.querySelector("details.menu")?.open;
  bar.replaceChildren();
  bar.hidden = !e;
  if (!e) return;
  const sep = () => {
    const s = document.createElement("span");
    s.className = "bar-sep";
    bar.append(s);
  };
  const btn = (parent, label, title, act, checked) => {
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = label;
    b.title = title;
    b.setAttribute("aria-label", title);
    if (checked !== undefined) {
      b.setAttribute("role", "radio");
      b.setAttribute("aria-checked", String(checked));
    }
    b.addEventListener("click", () => { act(); changed({}); });
    parent.append(b);
    return b;
  };
  const select = (parent, label, options, val, onSet) => {
    const s = document.createElement("select");
    s.setAttribute("aria-label", label);
    s.title = label;
    options.forEach(([v, name]) => s.add(new Option(name, v)));
    s.value = val;
    s.addEventListener("change", () => { onSet(s.value); changed({}); });
    parent.append(s);
    return s;
  };
  const alignRow = (parent) => {
    for (const [v, icon, name] of [["left", "⯇", "Left"], ["center", "≡", "Centre"], ["right", "⯈", "Right"]])
      btn(parent, icon, "Align " + name.toLowerCase(), () => (v === "left" ? delete e.align : (e.align = v)), (e.align || "left") === v);
  };
  if (e.type === "text") {
    const fonts = e.fonts || ["small"];
    select(bar, "Font", FONTS.map((f) => [f, `${f[0].toUpperCase() + f.slice(1)} ${FONT_PX[f]} px`]), fonts[0], (v) => {
      e.fonts = [v, ...fonts.slice(1).filter((f) => f !== v)];
    });
    sep();
    alignRow(bar);
  } else if (e.type === "barcode") {
    select(bar, "Code", [["code128", "Code 128"], ["ean13", "EAN-13"]], e.symbology, (v) => (e.symbology = v));
    sep();
    alignRow(bar);
  } else if (e.type === "icon") {
    // the picture by name, or the one a field names (e.g. a weather symbol: "sun", "rain")
    const pics = Object.keys(doc().icons || {});
    const opts = [...pics.map((n) => ["i:" + n, n]), ...layout().fields.map((f) => ["f:" + f, "Named by " + f])];
    select(bar, "Icon", opts, iconByField(e) ? "f:" + e.field : "i:" + e.icon, (v) => {
      if (v.startsWith("f:")) {
        delete e.icon;
        e.field = v.slice(2);
      } else {
        const pic = doc().icons[v.slice(2)];
        delete e.field;
        e.icon = v.slice(2);
        if (pic) Object.assign(e, { w: pic.w, h: pic.h });
      }
    });
  } else if (e.type === "qr") {
    select(bar, "Error correction", [["L", "L 7 %"], ["M", "M 15 %"], ["Q", "Q 25 %"], ["H", "H 30 %"]], e.ecc || "M",
      (v) => (v === "M" ? delete e.ecc : (e.ecc = v)));
    sep();
    alignRow(bar);
  }
  if (e.type !== "barcode" && e.type !== "qr") {
    if (e.type === "text") sep();
    const red = MODELS[state.model].colors?.includes("red");
    const colors = e.type === "text" ? (red ? TEXT_COLORS_RED : TEXT_COLORS)
      : e.type === "rect" ? (red ? RECT_COLORS_RED : RECT_COLORS) : red ? LINE_COLORS_RED : LINE_COLORS;
    const sw = document.createElement("span");
    sw.className = "swatch";
    sw.style.background = SWATCH[e.color || "black"];
    bar.append(sw);
    const cs = select(bar, "Colour", colors.map((c) => [c, COLOR_NAMES[c]]), e.color || "black", (v) => (v === "black" ? delete e.color : (e.color = v)));
    cs.title = "Colour" + (COLOR_HINTS[e.color] ? ": " + COLOR_HINTS[e.color] : "");
  }
  sep();
  bar.append(moreMenu(e, btn, select, openMore));
  btn(bar, "⧉", "Duplicate (Ctrl+D)", duplicateElement);
  const del = btn(bar, "🗑", "Delete (Delete)", deleteElement);
  del.className = "del";
  del.disabled = layout().elements.length <= 1;
  placeBar();
}

// The number format of a text element as checkboxes; they write "decimals" (0, 2, "auto",
// "dash", "0-dash", "keep") and "thousands".
function numberRows(e, row) {
  const d = e.decimals;
  const on = d !== undefined || !!e.thousands;
  const keep = on && (d === "keep" || d === undefined);
  const dec = keep || d === 2 || d === "auto" || d === "dash";
  const dash = d === "dash" || d === "0-dash";
  const hide = d === "auto" || d === "dash";
  const set = (f) => {
    const o = { on, dec, keep, dash, hide, thousands: !!e.thousands, ...f };
    delete e.decimals;
    delete e.thousands;
    if (!o.on) return;
    e.decimals = o.dec && o.keep ? "keep" : o.dec ? (o.dash ? "dash" : o.hide ? "auto" : 2) : o.dash ? "0-dash" : 0;
    if (o.thousands) e.thousands = true;
  };
  const check = (r, label, title, checked, act, disabled, sub) => {
    const l = document.createElement("label");
    l.className = "check" + (sub ? " sub" : "");
    l.title = title;
    const c = document.createElement("input");
    c.type = "checkbox";
    c.checked = checked;
    c.disabled = !!disabled;
    c.addEventListener("change", () => { act(c.checked); changed({}); });
    l.append(c, " " + label);
    r.append(l);
  };
  check(row("Number"), "Format as number", "Draw a price or other number the same way whatever form it is sent in",
    on, (v) => set(v ? { on: true, dec: true, keep: false } : { on: false }));
  if (!on) return false;
  check(row(""), "With decimals", "On: 199,90. Off: rounded to whole kroner, 199", dec, (v) => set({ dec: v, keep: false }));
  if (keep)
    check(row(""), "As sent", "The decimals as sent, with a decimal comma: 199,5 stays 199,5", true, () => set({ keep: false }), false, true);
  else if (dec)
    check(row(""), "Hide ,00", "199 instead of 199,00 (199,90 keeps its decimals)", hide || dash, (v) => set({ hide: v }), dash, true);
  check(row(""), "With ,-", dec ? "Whole amounts end in ,-: 199,- or 199,90" : "Rounded, with ,-: 199,-", dash, (v) => set({ dash: v, hide: v && hide }), keep);
  check(row(""), "Thousands separator", "A space between every three digits: 1 299", !!e.thousands, (v) => set({ thousands: v }));
  return true;
}

// What is used less: vertical alignment, lines, smaller fonts, placing on the screen, order.
function moreMenu(e, btn, select, open) {
  const { width, height } = MODELS[state.model];
  const d = document.createElement("details");
  d.className = "menu";
  d.open = !!open;
  const s = document.createElement("summary");
  s.textContent = "More";
  const list = document.createElement("div");
  list.className = "menu-list";
  d.append(s, list);
  const row = (label) => {
    const r = document.createElement("div");
    r.className = "row";
    const l = document.createElement("span");
    l.textContent = label;
    r.append(l);
    list.append(r);
    return r;
  };
  if (e.type === "text") {
    const v = row("Vertical");
    for (const [val, icon, name] of [["top", "⯅", "Top"], ["middle", "≡", "Middle"], ["bottom", "⯆", "Bottom"]])
      btn(v, icon, "Vertical: " + name.toLowerCase(), () => (val === "top" ? delete e.valign : (e.valign = val)), (e.valign || "top") === val);
    select(row("Lines"), "Lines", [["0", "auto"], ["1", "1"], ["2", "2"], ["3", "3"]], String(e.lines || 0), (val) => {
      if (+val) e.lines = +val;
      else delete e.lines;
    });
    // Up to two smaller fonts, tried in order when the text does not fit the first
    const fonts = e.fonts || ["small"];
    const f = row("If too long");
    for (let i = 1; i < 3; i++) {
      const sel = select(f, i === 1 ? "Next font if the text does not fit" : "Last font if the text does not fit",
        [["", "–"], ...FONTS.map((x) => [x, x])], fonts[i] || "", (val) => {
          const a = fonts.slice();
          if (!val) a.length = Math.min(a.length, i);
          else a[i] = val;
          e.fonts = a.filter(Boolean);
        });
      sel.disabled = i > fonts.length;
    }
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = "The tag uses the first font the text fits in, otherwise the last one with “…”.";
    list.append(p);
    // A price or other number: decimals, ",-" and thousands separator (the AP formats it)
    if (numberRows(e, row)) {
      const pn = document.createElement("p");
      pn.className = "hint";
      pn.textContent = "Sent as 1299.9, 1299,90 or 1 299,90, the number is drawn as chosen, on one line; text that is not a number is drawn as sent. A single dot is the decimal sign: 1.299 is 1,299, so send 1299 for a thousand.";
      list.append(pn);
    }
  }
  if (e.type === "barcode") {
    const lim = state.renderer.limits;
    select(row("Module"), "Module width", [["0", "auto"], ...Array.from({ length: lim.barcodeModuleMax }, (_, i) => [String(i + 1), i + 1 + " px"])],
      String(e.module || 0), (val) => {
        if (+val) e.module = +val;
        else delete e.module;
      });
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = (e.symbology === "ean13"
      ? "EAN-13 takes 12 digits (the tag adds the check digit) or 13. "
      : "Code 128 takes letters, digits and signs; digits only give the shortest code. ") +
      `The bars fill the box, which must be at least ${lim.barcodeMinH} px high; "auto" uses the widest module that fits. ` +
      "On the 2.9\", 2 px modules scan reliably.";
    list.append(p);
  }
  if (e.type === "qr") {
    const lim = state.renderer.limits;
    const v = row("Vertical");
    for (const [val, icon, name] of [["top", "⯅", "Top"], ["middle", "≡", "Middle"], ["bottom", "⯆", "Bottom"]])
      btn(v, icon, "Vertical: " + name.toLowerCase(), () => (val === "top" ? delete e.valign : (e.valign = val)), (e.valign || "top") === val);
    select(row("Module"), "Module size", [["0", "auto"], ...Array.from({ length: lim.qrModuleMax }, (_, i) => [String(i + 1), i + 1 + " px"])],
      String(e.module || 0), (val) => {
        if (+val) e.module = +val;
        else delete e.module;
      });
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = "The code grows with the text (up to about 90 characters fit in a field). The box includes a white " +
      "border of 2 modules for phones to read it; on the 2.9\", 2 px modules scan well.";
    list.append(p);
  }
  if (e.type === "rect" || e.type === "line") {
    const half = Math.floor(Math.min(e.w, e.h) / 2);
    const num = (r, label, key, min, max, def, hint) => {
      const inp = document.createElement("input");
      inp.type = "number";
      inp.min = min;
      inp.max = max;
      inp.value = e[key] ?? def;
      inp.title = hint;
      inp.setAttribute("aria-label", label);
      inp.addEventListener("change", () => {
        const v = clamp(Math.round(+inp.value || 0), min, max);
        // the bar is drawn again on a change, which blurs this input and fires change once more
        if (v === (e[key] ?? def)) return;
        if (v === def) delete e[key];
        else e[key] = v;
        changed({});
      });
      r.append(inp);
    };
    if (e.type === "rect") {
      num(row("Corners"), "Corner radius in pixels", "radius", 0, half, 0, `Round corners, 0 to ${half} px (half the shorter side)`);
      num(row("Border"), "Border width in pixels", "border", 0, half, 0, "0 fills the box, more draws only a border this wide");
    } else {
      const d = row("Direction");
      for (const [val, icon, name] of [["down", "╲", "Down: top-left to bottom-right"], ["up", "╱", "Up: bottom-left to top-right"]])
        btn(d, icon, name, () => (val === "down" ? delete e.direction : (e.direction = val)), (e.direction || "down") === val);
      num(row("Width"), "Line width in pixels", "width", 1, state.renderer.limits.lineWidthMax, 1, "Line width; a straight line is exactly this thick, a slanted one reaches half its width past the box, with round ends");
      const p = document.createElement("p");
      p.className = "hint";
      p.textContent = "The line runs corner to corner in its box: 1 px high is a straight line across, 1 px wide a straight line down.";
      list.append(p);
    }
  }
  const place = row("Place");
  for (const [label, title, act] of [
    ["⇤", "Against the left edge", () => (e.x = 0)],
    ["↔", "Centre across", () => (e.x = Math.floor((width - e.w) / 2))],
    ["⇥", "Against the right edge", () => (e.x = width - e.w)],
    ["⤒", "Against the top", () => (e.y = 0)],
    ["↕", "Centre up and down", () => (e.y = Math.floor((height - e.h) / 2))],
    ["⤓", "Against the bottom", () => (e.y = height - e.h)],
  ]) btn(place, label, title, act);
  // an icon is the size of its picture
  const fill = e.type === "icon" ? null : row("Fill");
  if (fill) btn(fill, "Width", "Stretch across the whole screen", () => ((e.x = 0), (e.w = width)));
  if (fill) btn(fill, "Height", "Stretch over the whole height", () => ((e.y = 0), (e.h = height)));
  if (e.type === "rect" || e.type === "line") btn(fill, "Turn", "Swap width and height (a line becomes a vertical line)", () => {
    [e.w, e.h] = [Math.min(e.h, width), Math.min(e.w, height)];
    e.x = clamp(e.x, 0, width - e.w);
    e.y = clamp(e.y, 0, height - e.h);
  });
  const order = row("Order");
  const n = layout().elements.length;
  btn(order, "Forward", "Draw later (further in front)", () => moveElement(1)).disabled = state.el >= n - 1;
  btn(order, "Back", "Draw earlier (further back)", () => moveElement(-1)).disabled = state.el <= 0;
  return d;
}

const narrow = matchMedia("(max-width: 1000px)");

// Under the selected box, or over it when there is no room under it in the view.
function placeBar() {
  const bar = $("el-bar");
  if (bar.hidden) return;
  if (narrow.matches) {
    // On a narrow screen the bar sits at the bottom of the window (designer.css), More opens upwards
    Object.assign(bar.style, { top: "", left: "", visibility: "visible" });
    bar.querySelector("details.menu")?.classList.add("up");
    return;
  }
  const box = document.querySelector("#overlay .box.sel");
  if (!box || state.dragging) {
    bar.style.visibility = "hidden";
    return;
  }
  const host = $("dz-canvas");
  const h = host.getBoundingClientRect();
  const b = box.getBoundingClientRect();
  const bh = bar.offsetHeight;
  const gap = 8;
  let top = b.bottom - h.top + host.scrollTop + gap;
  if (b.bottom + gap + bh > h.bottom && b.top - h.top - gap - bh - 20 > 0) top = b.top - h.top + host.scrollTop - gap - bh - 18;
  const left = clamp(b.left - h.left + host.scrollLeft, 8, Math.max(8, host.scrollWidth - bar.offsetWidth - 8));
  Object.assign(bar.style, { top: top + "px", left: left + "px", visibility: "visible" });
  // The More list opens upwards when the bar is in the lower half of the view
  bar.querySelector("details.menu")?.classList.toggle("up", b.top + b.height / 2 > (h.top + Math.min(h.bottom, innerHeight)) / 2);
}

function refreshJson() {
  const t = $("json");
  if (document.activeElement !== t) t.value = JSON.stringify(doc(), null, 2);
  $("upload-cmd").textContent =
    `curl -X PUT http://openesl.local/api/layouts/${state.model} \\\n  -H "Authorization: Bearer $TOKEN" \\\n  --data-binary @${state.model}.json`;
}

function refreshAll() {
  refreshLayoutList();
  refreshLayoutForm();
  refreshElement();
  refreshJson();
  refreshUndo();
  refreshFileState();
  compileAndDraw();
}

// Something in the document changed. form: only the numbers moved (drag/keys).
function changed({ form = false, keepForm = false } = {}) {
  fitCorners(element());
  if (form) {
    drawOverlay();
    updatePositionInputs();
  } else if (!keepForm) {
    refreshElement();
  } else {
    drawOverlay();
  }
  refreshJson();
  compileAndDraw();
  save();
}

// A box made smaller keeps its corner radius and border within half its shorter side, as
// the compiler wants
function fitCorners(e) {
  if (e?.type !== "rect") return;
  const half = Math.floor(Math.min(e.w, e.h) / 2);
  for (const k of ["radius", "border"]) if (e[k] > half) e[k] = half;
}

// ---------- actions ----------

function addElement(kind) {
  const lay = layout();
  const { width, height } = MODELS[state.model];
  const e = {
    text: { type: "text", field: lay.fields[0], x: 4, y: 4, w: Math.min(120, width - 4), h: 24, fonts: ["medium", "small"] },
    fixed: { type: "text", text: "inkl. mva", x: 4, y: height - 22, w: Math.min(100, width - 4), h: 18, fonts: ["small"] },
    rect: { type: "rect", x: 0, y: 0, w: Math.min(80, width), h: 30 },
    hline: { type: "line", x: 4, y: Math.floor(height / 2), w: width - 8, h: 1, width: 2 },
    barcode: { type: "barcode", field: lay.fields.at(-1), symbology: "code128", x: 4, y: height - 40, w: Math.min(240, width - 8), h: 36 },
    qr: { type: "qr", field: lay.fields.at(-1), x: width - Math.min(100, height - 8) - 4, y: 4, w: Math.min(100, height - 8), h: Math.min(100, height - 8), align: "center", valign: "middle" },
  }[kind];
  lay.elements.push(e);
  state.el = lay.elements.length - 1;
  changed();
}

// Size an SVG is drawn at, on its longer side: an SVG scales, and the icon sets of the web give
// theirs at 16 or 24 px, which is too small on a tag (an icon cannot be made bigger later)
const SVG_SIZE = 64;

// An SVG length in px: a number with px, pt, pc, mm, cm, in or no unit; 0 for anything else (%)
function svgLength(v) {
  const m = /^\s*([+]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?)\s*(px|pt|pc|mm|cm|in)?\s*$/i.exec(v ?? "");
  const k = { px: 1, pt: 4 / 3, pc: 16, mm: 96 / 25.4, cm: 96 / 2.54, in: 96 }[(m?.[2] ?? "px").toLowerCase()];
  return m ? parseFloat(m[1]) * k : 0;
}

// A picture file as something drawImage takes, with its width and height. Chromium's
// createImageBitmap cannot decode an SVG, so an SVG goes through an Image, drawn SVG_SIZE on its
// longer side with the shape of its viewBox (or of its width and height, or square). linked: it
// had pictures that are not in the file (<image> with a link), which are left out.
async function readPicture(file) {
  const svg = file.type === "image/svg+xml" || /\.svg$/i.test(file.name);
  if (!svg) return createImageBitmap(file);
  const xml = new DOMParser().parseFromString(await file.text(), "image/svg+xml");
  const root = xml.documentElement;
  if (root.localName !== "svg") throw new Error("not an SVG");
  const ow = svgLength(root.getAttribute("width")), oh = svgLength(root.getAttribute("height"));
  const vb = (root.getAttribute("viewBox") ?? "").trim().split(/[\s,]+/).map(Number);
  let [vw, vh] = vb.length === 4 && vb[2] > 0 && vb[3] > 0 ? [vb[2], vb[3]] : [0, 0];
  if (!vw && ow && oh) {
    // without a viewBox its content is in px of its own size: keep that, scaled
    [vw, vh] = [ow, oh];
    root.setAttribute("viewBox", `0 0 ${ow} ${oh}`);
  }
  const r = vw ? vw / vh : 1;
  const [w, h] = r >= 1 ? [SVG_SIZE, SVG_SIZE / r] : [SVG_SIZE * r, SVG_SIZE];
  root.setAttribute("width", String(w));
  root.setAttribute("height", String(h));
  if (!root.hasAttribute("xmlns")) root.setAttribute("xmlns", "http://www.w3.org/2000/svg");
  // a linked picture is not loaded from a blob, and would be drawn as a broken-image sign
  let linked = false;
  for (const im of [...xml.getElementsByTagNameNS("http://www.w3.org/2000/svg", "image")]) {
    const href = im.getAttribute("href") ?? im.getAttributeNS("http://www.w3.org/1999/xlink", "href") ?? "";
    if (!href.trim().startsWith("data:")) {
      im.remove();
      linked = true;
    }
  }
  const url = URL.createObjectURL(new Blob([new XMLSerializer().serializeToString(xml)], { type: "image/svg+xml" }));
  try {
    const img = new Image();
    img.src = url;
    await img.decode();
    return { img, width: w, height: h, linked };
  } finally {
    URL.revokeObjectURL(url);
  }
}

// An icon from a picture file: drawn at its own size (scaled down to fit ICON_MAX and the
// screen), dark pixels drawn, light and transparent ones left.
async function addIcon(file) {
  const lay = layout();
  if (!lay || !file) return;
  if (Object.keys(doc().icons ?? {}).length >= ICON_FILE_MAX) {
    setStatus(`A layout file holds at most ${ICON_FILE_MAX} icons`, "error");
    return;
  }
  const { width, height } = MODELS[state.model];
  let bmp;
  try {
    bmp = await readPicture(file);
  } catch {
    setStatus("Could not read " + file.name + " as a picture (PNG, JPEG, GIF or SVG)", "error");
    return;
  }
  const max = Math.min(ICON_MAX, width, height);
  const k = Math.min(1, max / bmp.width, max / bmp.height);
  const w = Math.max(1, Math.round(bmp.width * k)), h = Math.max(1, Math.round(bmp.height * k));
  const c = document.createElement("canvas");
  [c.width, c.height] = [w, h];
  const ctx = c.getContext("2d");
  ctx.drawImage(bmp.img ?? bmp, 0, 0, w, h);
  const px = ctx.getImageData(0, 0, w, h).data;
  const row = Math.ceil(w / 8);
  const bits = new Uint8Array(row * h);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const i = 4 * (y * w + x);
      // over white, then the AP's picture rule (esl_image.c): dark if 3R + 4G + 2B is below grey 128
      const a = px[i + 3], over = (c) => (c * a + 255 * (255 - a)) / 255;
      if (3 * over(px[i]) + 4 * over(px[i + 1]) + 2 * over(px[i + 2]) < 9 * 128) bits[y * row + (x >> 3)] |= 0x80 >> (x & 7);
    }
  const d = doc();
  d.icons ??= {};
  let base = file.name.replace(/\.[^.]*$/, "").toLowerCase().replace(/[^a-z0-9_]+/g, "_").replace(/^[^a-z]+/, "").slice(0, 12) || "icon";
  let name = base;
  for (let n = 2; d.icons[name]; n++) name = base + n;
  d.icons[name] = { w, h, data: btoa(String.fromCharCode(...bits)) };
  lay.elements.push({ type: "icon", icon: name, x: Math.floor((width - w) / 2), y: Math.floor((height - h) / 2), w, h });
  state.el = lay.elements.length - 1;
  changed();  if (bmp.linked) setStatus(file.name + " links pictures that are not in the file; they were left out", "error");
}

// Icons no element in any layout draws are dropped, so they do not take room in the file. An icon
// named by a field may be any of them, so with one of those every icon stays.
function pruneIcons() {
  const d = doc();
  if (!d.icons || d.layouts.some((l) => l.elements.some(iconByField))) return;
  const used = new Set(d.layouts.flatMap((l) => l.elements.filter((e) => e.type === "icon").map((e) => e.icon)));
  for (const k of Object.keys(d.icons)) if (!used.has(k)) delete d.icons[k];
  if (!Object.keys(d.icons).length) delete d.icons;
}

function deleteElement() {
  const lay = layout();
  if (!element() || lay.elements.length <= 1) return;
  lay.elements.splice(state.el, 1);
  state.el = -1;
  pruneIcons();
  changed();
}

function duplicateElement() {
  const lay = layout();
  const e = element();
  if (!e) return;
  const { width, height } = MODELS[state.model];
  const c = structuredClone(e);
  c.x = clamp(c.x + 4, 0, width - c.w);
  c.y = clamp(c.y + 4, 0, height - c.h);
  lay.elements.splice(state.el + 1, 0, c);
  state.el++;
  changed();
}

// Paste the copied element into the current layout (it may be another layout or model). A
// text whose field this layout lacks takes the layout's first field.
function pasteElement() {
  const lay = layout();
  const { width, height } = MODELS[state.model];
  const { _icon: pic, ...c } = structuredClone(state.clip);
  if (usesField(c) && !lay.fields.includes(c.field)) c.field = lay.fields[0];
  if (c.type === "icon" && !iconByField(c)) {
    // the picture comes along; under another name if this file has a different one by that name
    if (!pic || pic.w > width || pic.h > height) return;
    const d = doc();
    d.icons ??= {};
    let name = c.icon;
    for (let n = 2; d.icons[name] && JSON.stringify(d.icons[name]) !== JSON.stringify(pic); n++) name = c.icon.slice(0, 16 - String(n).length) + n;
    if (!d.icons[name] && Object.keys(d.icons).length >= ICON_FILE_MAX) {
      setStatus(`A layout file holds at most ${ICON_FILE_MAX} icons`, "error");
      return;
    }
    d.icons[name] = pic;
    c.icon = name;
  }
  c.w = Math.min(c.w, width);
  c.h = Math.min(c.h, height);
  const same = lay.elements.some((o) => o.x === c.x && o.y === c.y && o.w === c.w && o.h === c.h);
  c.x = clamp(c.x + (same ? 4 : 0), 0, width - c.w);
  c.y = clamp(c.y + (same ? 4 : 0), 0, height - c.h);
  lay.elements.push(c);
  state.el = lay.elements.length - 1;
  changed();
}

function moveElement(d) {
  const lay = layout();
  const j = state.el + d;
  if (!element() || j < 0 || j >= lay.elements.length) return;
  [lay.elements[state.el], lay.elements[j]] = [lay.elements[j], lay.elements[state.el]];
  state.el = j;
}

function uniqueName(base) {
  return freeName(base, new Set(doc().layouts.map((l) => l.name)));
}

function addLayout(l) {
  doc().layouts.push(l);
  state.sel = "file:" + (doc().layouts.length - 1);
  state.el = -1;
  refreshAll();
  save();
}

function download(name, text, type = "application/json") {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([text], { type }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

function setDoc(d) {
  if (d.model && MODELS[d.model] && d.model !== state.model) {
    state.docs[d.model] = d;
    loadModel(d.model).then(save);
    return;
  }
  state.docs[state.model] = d;
  if (!isBuiltin() && !d.layouts?.[+state.sel.slice(5)]) state.sel = "file:0";
  if (!layout()?.elements?.[state.el]) state.el = -1;
}

// Fit: the whole tag in the canvas area. Otherwise tag pixels.
function applyZoom() {
  const z = $("zoom").value;
  const { width, height } = MODELS[state.model];
  $("stage").style.width = z === "fit" ? `min(100%, 900px, calc((100vh - 330px) * ${width / height}))` : `${z * width}px`;
  requestAnimationFrame(placeBar);
}

// The three columns fill the window under the page's own header.
function fitHeight() {
  const dz = document.querySelector("#tab-design .dz");
  if (!dz || $("tab-design").hidden) return;
  dz.style.setProperty("--top", "0px");
  dz.style.setProperty("--head", Math.round(dz.getBoundingClientRect().top + window.scrollY) + "px");
  placeBar();
}

function initActions() {
  for (const [name, m] of Object.entries(MODELS)) {
    if (layoutModel(name) !== name) continue; // it has the tab of the model whose file it uses
    const users = layoutUsers(name);
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = users.join('" + ') + '"';
    b.title = `${m.width} × ${m.height}` + (users.length > 1 ? `, one layout file for ${users.join('", ')}"` : "");
    b.dataset.model = name;
    b.setAttribute("role", "radio");
    b.addEventListener("click", () => { if (name !== state.model) loadModel(name).then(save); });
    $("model-tabs").append(b);
  }
  $("zoom").value = store.get("zoom") || "fit";
  $("zoom").addEventListener("change", () => {
    store.set("zoom", $("zoom").value);
    applyZoom();
  });
  $("layout-list").addEventListener("click", (ev) => {
    const li = ev.target.closest("li");
    if (!li) return;
    const act = ev.target.closest("button")?.dataset.act;
    if (act === "info") {
      showLayoutInfo(li.dataset.sel);
    } else if (act === "copy") {
      const c = structuredClone(layout());
      c.name = uniqueName(c.name);
      addLayout(c);
    } else if (act === "delete") {
      if (doc().layouts.length <= 1 || !confirm(`Delete the layout "${layout().name}"?`)) return;
      doc().layouts.splice(+state.sel.slice(5), 1);
      pruneIcons();
      state.sel = "file:0";
      state.el = -1;
      refreshAll();
      save();
    } else if (li.dataset.sel !== state.sel) {
      state.sel = li.dataset.sel;
      state.el = -1;
      refreshAll();
    }
  });
  $("builtin-list").addEventListener("click", (ev) => {
    const li = ev.target.closest("li");
    if (li && ev.target.closest("button")?.dataset.act === "info") return showLayoutInfo(li.dataset.sel);
    if (!li || li.dataset.sel === state.sel) return;
    state.sel = li.dataset.sel;
    state.el = -1;
    refreshAll();
  });
  $("layout-from-tpl").addEventListener("click", openTemplatePicker);
  $("tpl-offer-no").addEventListener("click", () => ($("tpl-offer").hidden = true));
  initTemplatePicker({
    model: () => state.model,
    doc,
    renderer: () => state.renderer,
    onAp,
    exampleDoc,
    add: addFromTemplate,
    redraw: () => { if (state.renderer && doc()) compileAndDraw(); },
  });
  $("layout-add").addEventListener("click", () => {
    const d = emptyDoc(state.model).layouts[0];
    d.name = uniqueName("layout");
    addLayout(d);
  });
  $("file-example").addEventListener("click", async () => {
    if (!confirm(`Replace the ${state.model}" layout file in the editor with the example from the repository?`)) return;
    try {
      setDoc(await exampleDoc(state.model));
      state.sel = "file:0";
      refreshAll();
      save();
      if (!onAp) markClean();
    } catch (e) {
      setStatus(e.message, "error");
    }
  });
  $("file-open").addEventListener("change", async (ev) => {
    const f = ev.target.files[0];
    ev.target.value = "";
    closeMenus();
    if (!f) return;
    try {
      setDoc(JSON.parse(await f.text()));
      state.sel = "file:0";
      refreshAll();
      save();
      if (!onAp) markClean();
    } catch (e) {
      setStatus("Could not read " + f.name + ": " + e.message, "error");
    }
  });
  $("file-save").addEventListener("click", () => {
    download(state.model + ".json", JSON.stringify(doc(), null, 2) + "\n");
    if (!onAp) markClean();
  });
  $("design-save").addEventListener("click", () => {
    const name = layoutName();
    const fields = {};
    fieldNames().forEach((n) => (fields[n] = `{${n}}`));
    const design = {
      format: "esl-label", version: 1, id: name.replace(/[^a-z0-9-]/g, "-").slice(0, 40),
      layout: name, fields,
      style: { price_highlight: $("opt-highlight").checked, frame: $("opt-frame").checked },
    };
    download(design.id + ".json", JSON.stringify(design, null, 2) + "\n");
  });
  $("json-open").addEventListener("click", () => {
    refreshJson();
    $("json-dlg").showModal();
  });
  $("lay-name").addEventListener("input", (ev) => {
    const old = sampleKey();
    layout().name = ev.target.value.trim();
    state.samples[sampleKey()] = state.samples[old];
    refreshLayoutList();
    refreshJson();
    compileAndDraw();
    save();
  });
  $("lay-desc").addEventListener("input", (ev) => {
    if (ev.target.value) layout().description = ev.target.value;
    else delete layout().description;
    refreshJson();
    save();
  });
  $("field-add").addEventListener("click", addField);
  $("json").addEventListener("input", (ev) => {
    try {
      setDoc(JSON.parse(ev.target.value));
    } catch (e) {
      setStatus("JSON: " + e.message, "error");
      return;
    }
    refreshLayoutList();
    refreshLayoutForm();
    refreshElement();
    compileAndDraw();
    save();
  });
  $("json").addEventListener("blur", refreshJson);
  for (const id of ["opt-highlight", "opt-frame"]) $(id).addEventListener("change", compileAndDraw);
  $("opt-boxes").addEventListener("change", drawOverlay);
  for (const k of ["text", "fixed", "rect", "hline", "barcode", "qr"]) $("el-add-" + k).addEventListener("click", () => addElement(k));
  $("el-add-icon").addEventListener("click", () => $("icon-file").click());
  $("icon-file").addEventListener("change", (ev) => {
    addIcon(ev.target.files[0]);
    ev.target.value = "";
  });
  $("undo").addEventListener("click", () => undoRedo(false));
  $("redo").addEventListener("click", () => undoRedo(true));
  $("dz-canvas").addEventListener("scroll", placeBar);
  window.addEventListener("resize", fitHeight);
  // The tab becomes visible (main.js unhides it): size the columns then
  new MutationObserver(() => requestAnimationFrame(fitHeight)).observe($("tab-design"), { attributes: true, attributeFilter: ["hidden"] });
}

// ---------- send to the AP ----------

// files: on the AP's own page, model -> the layout file the AP has (null: none), once read
// board and limits: what GET /api/layouts says about the AP (null limits from an older AP)
// via: on the web page, how the AP is reached: "usb" (its port, js/apport.js) or "wifi" (its
// own page in a new tab, js/handoff.js, or the bridge in tools/esl-client under Advanced)
const ap = { tags: [], fileVersion: null, connected: false, mac: "", info: "", files: {}, board: "", limits: null, via: "", usb: false,
             tagLayouts: {} };   // tagLayouts: the layout each tag shows, by MAC (layoutinfo.js)
const DEFAULT_BOARD = "c6-supermini";   // the board whose limits the renderer has built in (web/build.py)

// The version of the model's layout file on the AP (GET /api/layouts), null when it has none,
// and the AP's board and limits
async function apFileVersion() {
  const r = await apClient().layouts();
  ap.board = r.board || "";
  ap.limits = r.limits || null;
  return r.layouts?.find((x) => x.model === state.model)?.version ?? null;
}

// Upload, send and check only work once Connect has reached the AP (over USB or the bridge)
function apButtons(busy = false) {
  $("ap-connect").disabled = $("ap-usb-connect").disabled = $("ap-handoff").disabled = busy;
  $("ap-upload").disabled = busy || !ap.connected || isBuiltin();
  if (onAp) $("ap-save").disabled = $("ap-reload").disabled = busy || !ap.connected;
  for (const id of ["ap-send", "ap-refresh"]) $(id).disabled = busy || !ap.connected || !ap.mac;
}

function apShow() {
  if (!doc()) return;    // before the first model is loaded
  $("ap-setup").hidden = ap.connected;
  $("ap-conn").hidden = $("ap-pick").hidden = $("ap-buttons").hidden = !ap.connected;
  $("ap-conn-text").textContent = ap.info;
  $("send-h").textContent = `Send "${layoutName()}" to your AP`;
  if (!onAp) {
    for (const r of document.querySelectorAll('input[name="ap-via"]')) r.checked = r.value === ap.via;
    $("ap-usb").hidden = ap.via !== "usb";
    $("ap-wifi").hidden = ap.via !== "wifi";
    $("ap-usb-missing").hidden = hasSerial();
  }
  refreshApTags();
  refreshSends();
  apButtons();
  refreshLayoutList();
}

function apDisconnected() {
  ap.connected = false;
  ap.usb = false;
  ap.tags = [];
  ap.tagLayouts = {};
  if (!onAp) ap.files = {};
  ap.limits = null;
  ap.board = "";
  limitsChanged();
  apShow();
  apStatus("");
}

function apClient() {
  if (onAp) return new ApClient("", $("ap-token").value.trim());
  if (ap.usb) {
    if (!serialAp()) throw new Error("The USB connection to the AP is closed. Connect again.");
    return new PortClient(serialAp());
  }
  const { base, error } = apBase($("ap-url").value);
  if (error) throw new Error(error);
  return new ApClient(base, $("ap-token").value.trim());
}

function apStatus(text, cls = "") {
  const s = $("ap-status");
  s.textContent = text;
  s.className = "hint " + cls;
}

function ago(s) {
  if (s === null || s === undefined) return "not seen";
  if (s < 90) return s + " s ago";
  if (s < 5400) return Math.round(s / 60) + " min ago";
  return Math.round(s / 3600) + " h ago";
}

// The AP's tags of this model, one to pick; the last six digits of the MAC tell them apart
function refreshApTags() {
  const box = $("ap-tags");
  const mine = ap.tags.filter((t) => layoutModel(t.model) === state.model || t.model === null);
  $("ap-tags-h").textContent = `Tag (${state.model}" tags on this AP)`;
  if (!mine.some((t) => t.mac === ap.mac)) ap.mac = mine.length === 1 ? mine[0].mac : "";
  if (!mine.length) {
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = ap.tags.length ? `The AP has no ${state.model}" tags.` : "The AP has no tags yet.";
    box.replaceChildren(p);
    return;
  }
  box.replaceChildren(...mine.map((t) => {
    const l = document.createElement("label");
    const r = document.createElement("input");
    r.type = "radio";
    r.name = "ap-tag";
    r.value = t.mac;
    r.checked = t.mac === ap.mac;
    r.addEventListener("change", () => { ap.mac = t.mac; apButtons(); });
    const mac = document.createElement("span");
    mac.className = "mac";
    mac.title = t.mac;
    const b = document.createElement("b");
    b.textContent = t.mac.slice(-6);
    mac.append("…", b);
    const when = document.createElement("span");
    when.className = "when";
    when.textContent = "seen " + ago(t.seen_s_ago) + (t.pending ? " · waiting for its poll" : "");
    l.append(r, mac, when);
    return l;
  }));
}

function refreshSends() {
  const p = $("ap-sends");
  p.replaceChildren();
  fieldNames().forEach((n, i) => {
    if (i) p.append(" · ");
    const b = document.createElement("b");
    b.textContent = samples()[i] || "(empty)";
    p.append(n + " ", b);
  });
  const extra = [$("opt-highlight").checked && "highlighted price", $("opt-frame").checked && "frame"].filter(Boolean);
  p.append(document.createElement("br"), Object.assign(document.createElement("span"), {
    className: "muted", textContent: "The sample text" + (extra.length ? ", with " + extra.join(" and ") : "") + ".",
  }));
}

async function apRun(what, fn) {
  apStatus(what + "…");
  apButtons(true);
  try {
    apStatus(...(await fn()));
  } catch (e) {
    apStatus(e.message, "error");
  } finally {
    apShow();
  }
}

async function apConnect() {
  ap.connected = false;
  const r = await apClient().tags();
  ap.connected = true;
  ap.tags = r.tags || [];
  ap.fileVersion = await apFileVersion();
  limitsChanged();
  const where = onAp ? location.host || "this AP" : ap.usb ? "the AP over USB" : apBase($("ap-url").value).base.replace(/^https?:\/\//, "");
  ap.info = `Connected to ${where} · AP ${r.ap?.fw || "?"}, ${ap.tags.length} tags`;
  ap.tagLayouts = {};
  if (onAp) {
    try { localStorage.setItem(DASH_TOKEN, $("ap-token").value.trim()); } catch { /* private mode */ }
    const r = state.model in ap.files ? ["", ""] : await apOpen(state.model);
    await apReadFile(state.model);
    return r;
  }
  await apReadFile(state.model);
  return ["", ""];
}

// What the layout column shows for a connected AP: the AP's own file of a model (to tell
// which layouts it has as they are) and, in the background, which layout each tag shows.
// Off the AP's page the editor's file stays as it is.
async function apReadFile(model) {
  if (!onAp) {
    try {
      ap.files[model] = await apClient().call("GET", "/api/layouts/" + encodeURIComponent(model));
    } catch (e) {
      if (!/^404\b/.test(e.message)) throw e;
      ap.files[model] = null;
    }
  }
  const mine = ap.tags.filter((t) => layoutModel(t.model) === model);
  if (mine.some((t) => !(t.mac in ap.tagLayouts)))
    tagLayouts(apClient(), mine, ap.tagLayouts).then((m) => {
      if (!ap.connected) return;
      ap.tagLayouts = m;
      refreshLayoutList();
    }, () => { /* the column shows no counts */ });
}

// On the AP's own page: open the layout file the AP has for this model. Changes made here
// and not saved to the AP yet stay (the head says so); otherwise the AP's file replaces
// what the editor had, and a model without a file on the AP keeps the editor's.
async function apOpen(model, discard = false) {
  let file = null;
  try {
    file = await apClient().call("GET", "/api/layouts/" + encodeURIComponent(model));
  } catch (e) {
    if (!/^404\b/.test(e.message)) throw e;
  }
  ap.files[model] = structuredClone(file);   // the editor changes its own copy
  if (model !== state.model) return ["", ""];
  const wasClean = store.get("clean." + model) === JSON.stringify(doc());
  if (!file) {
    // nothing on the AP: whatever the editor has is not saved there
    store.set("clean." + model, "");
    refreshFileState();
    return [`The AP has no ${model}" layout file yet. Save to AP stores this one.`, ""];
  }
  if (!discard && !wasClean && JSON.stringify(file) !== JSON.stringify(doc())) {
    store.set("clean." + model, JSON.stringify(file));
    refreshFileState();
    return ["Your changes from before are kept. Reload from the AP drops them.", ""];
  }
  setDoc(file);
  state.history[model].reset(JSON.stringify(file));
  if (!state.sel.startsWith("builtin:") && !layout()) state.sel = file.layouts?.length ? "file:0" : "builtin:price";
  refreshAll();
  save();
  markClean();
  refreshUndo();
  return [`Opened the ${model}" layout file stored on the AP.`, "ok"];
}

async function apUpload(skip) {
  applyLimits();
  const res = state.renderer.loadLayouts(JSON.stringify(doc()));
  if (!res.ok) throw new Error("Fix the layout file first: " + res.error);
  const r = await apClient().putLayouts(state.model, doc());
  ap.fileVersion = res.version;
  ap.files[state.model] = structuredClone(doc());
  markClean();
  const unfit = unfitNote(r, skip);
  if (unfit) return [`Stored on the AP, but ${unfit}`, "error"];
  return [r.changed ? `Stored on the AP. It draws the ${state.model}" tags with it, and they get the new screen at their next poll.` : "The AP already has this file.", "ok"];
}

// The tags whose content no longer fits the uploaded file (their content is held back), "" for none.
// skip: a tag that gets new content right after, so it is not listed.
function unfitNote(r, skip) {
  const errs = (r?.content_errors || []).filter((e) => e.mac !== skip);
  const n = (r?.content_errors_total || 0) - ((r?.content_errors || []).length - errs.length);
  if (n <= 0) return "";
  const list = errs.map((e) => `${e.mac}: ${e.error}`).join("; ");
  const more = n > errs.length ? ` and ${n - errs.length} more` : "";
  return `${n === 1 ? "1 tag has" : n + " tags have"} content that does not fit it, and keep${n === 1 ? "s" : ""} what ${n === 1 ? "it shows" : "they show"} until it gets content that fits: ${list}${more}.`;
}

async function apSend() {
  const mac = ap.mac;
  if (!mac) throw new Error("Pick a tag first.");
  const name = layoutName();
  const content = { layout: name, fields: samples().slice() };
  if ($("opt-highlight").checked) content.price_highlight = true;
  if ($("opt-frame").checked) content.frame = true;
  let r, uploaded = false;
  // An AP without this version of the file gets it first, so the tag shows what you see
  applyLimits();
  const res = state.renderer.loadLayouts(JSON.stringify(doc()));
  let unfit = "";
  if (!isBuiltin() && res.ok && ap.fileVersion !== res.version) {
    const [msg, kind] = await apUpload(mac);
    if (kind === "error") unfit = " " + msg;
    uploaded = true;
  }
  try {
    r = await apClient().putContent(mac, content);
  } catch (e) {
    // The AP does not have this layout (no layout file for the model yet, or an older one
    // without it): upload the editor's file, then send again
    if (isBuiltin() || uploaded || !/^422: layout .* is not in (the|any) layout file/.test(e.message)) throw e;
    const [msg, kind] = await apUpload(mac);
    if (kind === "error") unfit = " " + msg;
    r = await apClient().putContent(mac, content);
    uploaded = true;
  }
  ap.tagLayouts[mac] = name;
  const note = uploaded ? " The layout file was uploaded first." : "";
  return [(r.changed ? "Sent. The tag shows it after its next poll." : "The tag already has this content.") + note + unfit,
          unfit ? "error" : "ok"];
}

async function apCheck() {
  const mac = ap.mac;
  if (!mac) throw new Error("Pick a tag first.");
  const t = await apClient().tag(mac);
  const i = ap.tags.findIndex((x) => x.mac === mac);
  if (i >= 0) ap.tags[i] = t;
  if (t.content_error) return [`Cannot be shown: ${t.content_error}.`, "error"];
  if (t.draw_failed) return ["The tag says its last drawing failed.", "error"];
  if (t.pending) return [`Waiting for the tag's next poll (seen ${ago(t.seen_s_ago)}).`, ""];
  return [`The tag shows it (seen ${ago(t.seen_s_ago)}).`, "ok"];
}

// On the AP's own page the token is all it needs; the Dashboard tab keeps it under the same
// key, so one entry serves both. Save to AP and Reload from the AP stand for the file menu's
// file on disk there: the AP's own file is the one the editor opens.
function initOnAp() {
  $("ap-web").hidden = true;
  $("ap-connect").classList.add("primary");
  $("ap-url").closest("label").hidden = true;
  document.querySelector("#ap-direct > p.hint").textContent =
    "Enter the AP's API token once; this browser keeps it for this address, and the Dashboard tab uses it too.";
  try { $("ap-token").value = localStorage.getItem(DASH_TOKEN) || ""; } catch { /* private mode */ }
  $("ap-token").placeholder = "the AP's API token";
  $("ap-token").addEventListener("change", apDisconnected);
  $("ap-save").hidden = $("ap-reload").hidden = false;
  new MutationObserver(() => { if (!$("tab-design").hidden) apStart(); })
    .observe($("tab-design"), { attributes: true, attributeFilter: ["hidden"] });
  // the outcome of Save and Reload goes in the status bar too, since the dialog is closed
  const toStatus = () => setStatus($("ap-status").textContent, $("ap-status").classList.contains("error") ? "error" : "ok");
  $("ap-save").addEventListener("click", async () => {
    await apRun("Saving", apUpload);
    toStatus();
  });
  $("ap-reload").addEventListener("click", async () => {
    closeMenus();
    if (store.get("clean." + state.model) !== JSON.stringify(doc()) &&
      !confirm(`Drop the changes to the ${state.model}" layout file and open the one stored on the AP?`)) return;
    await apRun("Reading the AP's layout file", () => apOpen(state.model, true));
    toStatus();
  });
  return () => $("ap-token").value;
}

// On the AP's own page, once the first model is loaded and whenever the tab is shown while not
// connected: connect with the kept token (the Dashboard may have stored it meanwhile) and open
// the AP's file, or ask for the token where Send to your AP does. Not while the tab is hidden: its
// dialog is modal, and would leave the Dashboard tab unclickable behind it.
let apStarting = false;
let apReady = false;      // the first model is loaded (initEditor)
async function apStart() {
  if (!apReady || $("tab-design").hidden || ap.connected || apStarting || $("send-dlg").open) return;
  apStarting = true;
  try {
    try { $("ap-token").value ||= localStorage.getItem(DASH_TOKEN) || ""; } catch { /* private mode */ }
    if ($("ap-token").value) await apRun("Connecting", apConnect);
    if (!ap.connected) return $("send-open").click();
    if ($("ap-status").textContent) setStatus($("ap-status").textContent, $("ap-status").classList.contains("error") ? "error" : "ok");
  } finally {
    apStarting = false;
  }
}

// The web page's Send to your AP: over USB (the port the Dashboard tab shares, no token), or
// over WiFi by handing the file to the AP's own page (js/handoff.js). The bridge in
// tools/esl-client, at the Dashboard's address and with a token for this session, is under
// Advanced.
function initOnWeb() {
  const sess = {
    get(k) { try { return sessionStorage.getItem(STORE + k) || ""; } catch { return ""; } },
    set(k, v) { try { sessionStorage.setItem(STORE + k, v); } catch { /* private mode */ } },
  };
  let url = "";
  try { url = localStorage.getItem(DASH_URL) || store.get("ap.url") || ""; } catch { /* private mode */ }
  $("ap-url").value = url;
  $("ap-token").value = sess.get("ap.token");
  $("ap-addr").value = store.get("ap.addr") || "";
  $("ap-bridge").append($("ap-direct"));
  $("ap-bridge").open = store.get("ap.bridge") === "1";
  ap.via = store.get("ap.via") || (hasSerial() ? "usb" : "wifi");
  $("ap-url").addEventListener("change", () => {
    try { localStorage.setItem(DASH_URL, $("ap-url").value.trim()); } catch { /* private mode */ }
    apDisconnected();
  });
  $("ap-token").addEventListener("change", () => { sess.set("ap.token", $("ap-token").value.trim()); apDisconnected(); });
  $("ap-addr").addEventListener("change", () => store.set("ap.addr", $("ap-addr").value.trim()));
  $("ap-bridge").addEventListener("toggle", () => store.set("ap.bridge", $("ap-bridge").open ? "1" : ""));
  for (const r of document.querySelectorAll('input[name="ap-via"]'))
    r.addEventListener("change", () => {
      ap.via = r.value;
      store.set("ap.via", ap.via);
      apShow();
      apStatus("");
    });
  $("ap-usb-connect").addEventListener("click", () => apRun("Connecting over USB", async () => {
    try {
      await connectSerial();
    } catch (e) {
      if (e.name === "NotFoundError") return ["", ""];   // no port picked: nothing to say
      throw e;
    }
    ap.usb = true;
    return apConnect();
  }));
  $("ap-handoff").addEventListener("click", () => apRun("Opening the AP's page", apHandoff));
  window.addEventListener("message", handoffMessage);
  // The port closed (unplugged, or the Dashboard tab disconnected it)
  onSerialChange((port) => { if (!port && ap.usb) apDisconnected(); });
  return () => {
    if (ap.via === "usb") return serialAp() ? "usb" : "";
    if (!$("ap-bridge").open) return "";
    // The Dashboard's address may have changed since
    try {
      const u = localStorage.getItem(DASH_URL);
      if (u && u !== $("ap-url").value && !ap.connected) $("ap-url").value = u;
    } catch { /* private mode */ }
    return $("ap-url").value;
  };
}

// ---------- over WiFi: the AP's own page takes the file (js/handoff.js) ----------

let handoff = null;   // { win, msg, host, timer } while the AP's page is opened

async function apHandoff() {
  const { url, error } = apPageUrl($("ap-addr").value);
  if (error) throw new Error(error);
  store.set("ap.addr", $("ap-addr").value.trim());
  applyLimits();
  const res = state.renderer.loadLayouts(JSON.stringify(doc()));
  if (!res.ok) throw new Error("Fix the layout file first: " + res.error);
  const content = { layout: layoutName(), fields: samples().slice() };
  if ($("opt-highlight").checked) content.price_highlight = true;
  if ($("opt-frame").checked) content.frame = true;
  const msg = layoutsMessage({ model: state.model, file: structuredClone(doc()), layout: layoutName(), content, preview: $("screen").toDataURL("image/png") });
  clearTimeout(handoff?.timer);
  // A name, so a second press uses the same tab
  const win = window.open(url, "openesl-ap");
  const host = new URL(url).host;
  if (!win) throw new Error(`The browser blocked the new tab. Allow pop-ups for this page, or open ${url.replace(/\?.*/, "")} yourself.`);
  handoff = { win, msg, host };
  handoff.timer = setTimeout(() => {
    if (handoff?.win === win)
      apStatus(`No answer from the page at ${host}. Either that is not the AP's address (its IP address works too), or the AP's ` +
        "firmware is 0.8.2 or older, which cannot take layouts from here: update it on the Dashboard tab, or use the USB cable.", "error");
  }, 10000);
  return [`Opening ${host} in a new tab. Its page shows the layout file at the top when the AP's firmware is new enough.`, ""];
}

function handoffMessage(ev) {
  if (!handoff || ev.source !== handoff.win) return;
  const type = ev.data?.type;
  if (type === READY) {
    clearTimeout(handoff.timer);
    ev.source.postMessage(handoff.msg, ev.origin);
    apStatus(`Sent to the page on ${handoff.host}. Check it there and press Save.`, "ok");
  } else if (type === SAVED) {
    const text = typeof ev.data.text === "string" ? ev.data.text : "";
    apStatus(text || (ev.data.ok ? "Saved on the AP." : "The AP did not save it."), ev.data.ok ? "ok" : "error");
    if (ev.data.ok && ev.data.model === handoff.msg.model) {
      store.set("clean." + handoff.msg.model, JSON.stringify(handoff.msg.file));
      refreshFileState();
    }
  }
}

function initAp() {
  // what Connect needs (the token on the AP, the port or the bridge's address on the web), "" before there is one
  const canConnect = onAp ? initOnAp() : initOnWeb();
  $("ap-connect").addEventListener("click", () => {
    ap.usb = false;
    apRun("Connecting", apConnect);
  });
  $("ap-change").addEventListener("click", apDisconnected);
  $("ap-upload").addEventListener("click", () => apRun("Uploading", apUpload));
  $("ap-send").addEventListener("click", () => apRun("Sending", apSend));
  $("ap-refresh").addEventListener("click", () => apRun("Asking the AP", apCheck));
  $("send-open").addEventListener("click", () => {
    apStatus("");
    apShow();
    $("send-dlg").showModal();
    const how = canConnect();
    if (how && !ap.connected) {
      ap.usb = how === "usb";
      apRun("Connecting", apConnect);
    } else if (ap.connected) apRun("Reading the tags", async () => {
      ap.tags = (await apClient().tags()).tags || [];
      return ["", ""];
    });
  });
  apDisconnected();
}

// sameOrigin: on the AP's own page (js/apdesign.js)
// Edit on the dashboard's Layouts card (js/aplayouts.js): the AP's file of that model, with the
// layout picked. Changes the editor has to its file and nowhere else are only dropped when the
// user says so.
async function openFromAp({ model, file, name }) {
  if (!MODELS[model] || !file?.layouts) return;
  location.hash = "#design";
  // Load the model first: its file (with unsaved changes) may only be in the browser's storage yet
  if (model !== state.model) await loadModel(model);
  if (state.docs[model] && JSON.stringify(state.docs[model]) !== JSON.stringify(file) &&
      store.get("clean." + model) !== JSON.stringify(state.docs[model]) &&
      !confirm(`The designer has changes to its ${model}" layout file that are not saved anywhere. Open the AP's file instead?`)) return;
  setDoc(structuredClone(file));
  state.history[model].reset(JSON.stringify(file));
  ap.files[model] = structuredClone(file);
  const i = file.layouts.findIndex((l) => l.name === name);
  state.sel = "file:" + Math.max(0, i);
  state.el = -1;
  refreshAll();
  save();
  markClean();
  refreshUndo();
}

export async function initEditor({ sameOrigin = false } = {}) {
  onAp = sameOrigin;
  try {
    state.samples = JSON.parse(store.get("samples") || "{}");
  } catch {
    state.samples = {};
  }
  initActions();
  initMenus();
  initDrag();
  initList();
  initAp();
  window.addEventListener("openesl-edit-layout", (ev) => openFromAp(ev.detail));
  const m = store.get("model");
  await loadModel(MODELS[m] ? m : Object.keys(MODELS)[0]);
  fitHeight();
  if (onAp) {
    apReady = true;
    await apStart();
  }
}
