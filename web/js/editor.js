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
import { ApClient, apBase } from "./apclient.js";
import { loadRenderer, MODELS, LAYOUT_PRICE, LAYOUT_TEXT, LAYOUT_FILE, FLAG_PRICE_HIGHLIGHT, FLAG_FRAME } from "./renderer.js";

const FONTS = ["small", "medium", "mid", "large"];
const FONT_PX = { small: 17, medium: 24, mid: 36, large: 52 };
const TEXT_COLORS = ["black", "white", "invert", "highlight", "frame"];
const RECT_COLORS = ["black", "white", "frame"];
// On a model with red (colors in shared/models.json) the AP also takes these
const TEXT_COLORS_RED = [...TEXT_COLORS, "red", "red_invert"];
const RECT_COLORS_RED = [...RECT_COLORS, "red"];
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
const DASH_URL = "openesl.dash.url";   // the Dashboard tab's AP address, shared with Send to tag

const $ = (id) => document.getElementById(id);
const store = {
  get(k) { try { return localStorage.getItem(STORE + k); } catch { return null; } },
  set(k, v) { try { localStorage.setItem(STORE + k, v); } catch { /* private mode */ } },
};

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
  refreshFileState();
}

// The file as it was last saved to a file, opened, or stored on the AP, so the head can say
// when the editor has changes that are only in this browser.
function markClean() {
  store.set("clean." + state.model, JSON.stringify(doc()));
  refreshFileState();
}

function refreshFileState() {
  const clean = store.get("clean." + state.model);
  const fs = $("file-state");
  fs.replaceChildren(`Layout file for ${state.model}"`);
  if (clean !== JSON.stringify(doc())) {
    const d = document.createElement("span");
    d.className = "dirty";
    d.textContent = " · ● changes only in this browser";
    d.title = "Kept in this browser. Save the layout file or send it to the AP to keep it elsewhere.";
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

// The layouts of the example file, offered as templates under "+ New". Their "_sample"
// values (ignored by the AP) fill the preview.
const templates = {};

async function refreshTemplates(model) {
  try {
    templates[model] ||= (await exampleDoc(model)).layouts || [];
  } catch {
    templates[model] = [];
  }
  if (model !== state.model) return;
  const list = $("layout-new-list");
  list.querySelectorAll(".tpl").forEach((b) => b.remove());
  templates[model].forEach((l, i) => {
    const b = document.createElement("button");
    b.className = "item tpl";
    b.setAttribute("role", "menuitem");
    b.textContent = l.name;
    b.title = l.description || "";
    b.addEventListener("click", () => {
      closeMenus();
      const c = structuredClone(templates[state.model][i]);
      c.name = uniqueName(c.name);
      addLayout(c);
    });
    list.append(b);
  });
}

async function loadModel(model) {
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
  document.querySelectorAll("#model-tabs button").forEach((b) => b.setAttribute("aria-checked", String(b.dataset.model === model)));
  applyZoom();
  document.querySelectorAll(".model-name").forEach((e) => (e.textContent = model));
  state.sel = doc().layouts?.length ? "file:0" : "builtin:price";
  state.el = -1;
  ap.mac = "";
  refreshAll();
  refreshTemplates(model);
}

// ---------- drawing ----------

function compileAndDraw() {
  const r = state.renderer;
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
  if (res.ok) {
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
  if (lay) {
    const u = layoutUsage(lay, lim);
    meters.push(["Text lines", u.runs, lim.runs, "Lines of text the AP draws at most for one screen of this layout (each text box counts the most lines its fonts can fit)"]);
    meters.push(["Boxes", u.rects, lim.rects, "Filled boxes, lines and inverted or highlighted text in this layout"]);
    if (u.barcodes) meters.push(["Barcodes", u.barcodes, lim.barcodes, "Barcodes in this layout"]);
    $("el-add-barcode").disabled = u.barcodes >= lim.barcodes;
    $("el-add-barcode").title = u.barcodes >= lim.barcodes ? "A layout has at most one barcode" : "A field as a Code 128 or EAN-13 barcode (one per layout)";
  }
  meters.push(["Compiled file", res.ok ? res.size : null, lim.fileBytes, "Size of the whole compiled layout file the AP draws with, in bytes"]);
  meters.push(["JSON", new TextEncoder().encode(JSON.stringify(doc())).length, lim.jsonBytes, "Size of the layout file as Send to tag uploads it (compact JSON), in bytes; it usually fills before the compiled file. Save layout file writes it indented, and esl-client layout-put compacts it again"]);
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
  $("limits-text").textContent = worst > 1 ? "Over the AP's limits" : `${pct} % of limits`;
  if (worst > 1) l.open = true;
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
    b.className = "box" + (e.type === "rect" ? " rect" : e.type === "barcode" ? " barcode" : "") + (i === state.el ? " sel" : "");
    Object.assign(b.style, {
      left: (100 * e.x) / width + "%", top: (100 * e.y) / height + "%",
      width: (100 * e.w) / width + "%", height: (100 * e.h) / height + "%",
    });
    b.dataset.i = i;
    const tag = document.createElement("span");
    tag.className = "tag";
    tag.textContent = e.type === "rect" ? rectName(e) : e.type === "barcode" ? "▥ " + fieldLabel(e.field)
      : e.text !== undefined ? `"${e.text}"` : fieldLabel(e.field);
    b.append(tag);
    for (const h of HANDLES) {
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
    ico.className = "ico" + (e.type === "rect" ? " rect" : inverted ? " inv" : "");
    ico.textContent = e.type === "rect" ? "▬" : e.type === "barcode" ? "▥" : e.text !== undefined ? "“”" : "T";
    const name = document.createElement("span");
    name.className = "name";
    name.textContent = e.type === "rect" ? rectName(e) : e.type === "barcode" ? fieldLabel(e.field)
      : e.text !== undefined ? `"${e.text}"` : fieldLabel(e.field);
    const dim = document.createElement("span");
    dim.className = "dim";
    dim.textContent = e.type === "text" ? [(e.fonts || ["small"])[0], e.color && e.color !== "black" ? COLOR_NAMES[e.color]?.split(" ")[0].toLowerCase() : ""].filter(Boolean).join(", ")
      : e.type === "barcode" ? (e.symbology === "ean13" ? "EAN-13" : "Code 128") : e.color && e.color !== "black" ? e.color : "";
    li.title = `x ${e.x}, y ${e.y}, ${e.w} × ${e.h} px`;
    li.append(ico, name, dim);
    return li;
  }).reverse());
}

function rectName(e) {
  const kind = e.h <= 2 && e.w > e.h ? "line" : e.w <= 2 && e.h > e.w ? "vertical line" : "box";
  return kind + (e.color && e.color !== "black" ? ` (${e.color})` : "");
}

function elementName(e) {
  if (e.type === "barcode") return `barcode (${fieldLabel(e.field)})`;
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
      resize: ev.target.dataset.h || null,
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
  else if (mod && key === "c" && e && !window.getSelection()?.toString()) state.clip = structuredClone(e);
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
    if (ev.target.closest(".dz-head .menu-list .item, .dz-left .menu-list .item")) closeMenus();
  });
}

// ---------- the layouts and the right column ----------

function refreshLayoutList() {
  const ol = $("layout-list");
  const items = (doc().layouts || []).map((l, i) => ["file:" + i, l.name || "(no name)", false]);
  items.push(["builtin:price", "price", true], ["builtin:text", "text", true]);
  ol.replaceChildren(...items.map(([key, name, builtin]) => {
    const li = document.createElement("li");
    li.className = (key === state.sel ? "sel" : "") + (builtin ? " builtin" : "");
    li.dataset.sel = key;
    li.title = builtin ? "Built in to the firmware; it cannot be edited" : doc().layouts[+key.slice(5)]?.description || "";
    const n = document.createElement("span");
    n.className = "name";
    n.textContent = name;
    const acts = document.createElement("span");
    if (builtin) {
      acts.className = "dim";
      acts.textContent = "built-in";
    } else {
      acts.className = "acts";
      const copy = document.createElement("button");
      copy.textContent = "⧉";
      copy.title = copy.ariaLabel = "Copy this layout";
      copy.dataset.act = "copy";
      const del = document.createElement("button");
      del.textContent = "🗑";
      del.title = del.ariaLabel = doc().layouts.length <= 1 ? "The file needs at least one layout" : "Delete this layout";
      del.disabled = doc().layouts.length <= 1;
      del.dataset.act = "delete";
      acts.append(copy, del);
    }
    li.append(n, acts);
    return li;
  }));
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
// what the preview shows and Send to tag sends.
function refreshSample() {
  const box = $("sample");
  box.replaceChildren();
  const names = fieldNames();
  const vals = samples();
  const lay = layout();
  const e = element();
  const selField = e && e.text === undefined && e.type !== "rect" ? fieldLabel(e.field) : null;
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
    const used = lay?.elements.some((el) => el.type !== "rect" && el.text === undefined && fieldLabel(el.field) === n);
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
  if (e.type === "barcode") {
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
      box.append(l);
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
  }
  if (e.type !== "barcode") {
    if (e.type === "text") sep();
    const red = MODELS[state.model].colors?.includes("red");
    const colors = e.type === "text" ? (red ? TEXT_COLORS_RED : TEXT_COLORS) : red ? RECT_COLORS_RED : RECT_COLORS;
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
  const place = row("Place");
  for (const [label, title, act] of [
    ["⇤", "Against the left edge", () => (e.x = 0)],
    ["↔", "Centre across", () => (e.x = Math.floor((width - e.w) / 2))],
    ["⇥", "Against the right edge", () => (e.x = width - e.w)],
    ["⤒", "Against the top", () => (e.y = 0)],
    ["↕", "Centre up and down", () => (e.y = Math.floor((height - e.h) / 2))],
    ["⤓", "Against the bottom", () => (e.y = height - e.h)],
  ]) btn(place, label, title, act);
  const fill = row("Fill");
  btn(fill, "Width", "Stretch across the whole screen", () => ((e.x = 0), (e.w = width)));
  btn(fill, "Height", "Stretch over the whole height", () => ((e.y = 0), (e.h = height)));
  if (e.type === "rect") btn(fill, "Turn", "Swap width and height (a line becomes a vertical line)", () => {
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

// ---------- actions ----------

function addElement(kind) {
  const lay = layout();
  const { width, height } = MODELS[state.model];
  const e = {
    text: { type: "text", field: lay.fields[0], x: 4, y: 4, w: Math.min(120, width - 4), h: 24, fonts: ["medium", "small"] },
    fixed: { type: "text", text: "inkl. mva", x: 4, y: height - 22, w: Math.min(100, width - 4), h: 18, fonts: ["small"] },
    rect: { type: "rect", x: 0, y: 0, w: Math.min(80, width), h: 30 },
    hline: { type: "rect", x: 4, y: Math.floor(height / 2), w: width - 8, h: 2 },
    barcode: { type: "barcode", field: lay.fields.at(-1), symbology: "code128", x: 4, y: height - 40, w: Math.min(240, width - 8), h: 36 },
  }[kind];
  lay.elements.push(e);
  state.el = lay.elements.length - 1;
  changed();
}

function deleteElement() {
  const lay = layout();
  if (!element() || lay.elements.length <= 1) return;
  lay.elements.splice(state.el, 1);
  state.el = -1;
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
  const c = structuredClone(state.clip);
  if (c.type !== "rect" && c.text === undefined && !lay.fields.includes(c.field)) c.field = lay.fields[0];
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
  const names = new Set(doc().layouts.map((l) => l.name));
  let n = 2;
  let name = base;
  while (names.has(name)) name = `${base}-${n++}`.slice(0, 31);
  return name;
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
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = name + '"';
    b.title = `${m.width} × ${m.height}`;
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
    if (act === "copy") {
      const c = structuredClone(layout());
      c.name = uniqueName(c.name);
      addLayout(c);
    } else if (act === "delete") {
      if (doc().layouts.length <= 1 || !confirm(`Delete the layout "${layout().name}"?`)) return;
      doc().layouts.splice(+state.sel.slice(5), 1);
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
      markClean();
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
      markClean();
    } catch (e) {
      setStatus("Could not read " + f.name + ": " + e.message, "error");
    }
  });
  $("file-save").addEventListener("click", () => {
    download(state.model + ".json", JSON.stringify(doc(), null, 2) + "\n");
    markClean();
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
  for (const k of ["text", "fixed", "rect", "hline", "barcode"]) $("el-add-" + k).addEventListener("click", () => addElement(k));
  $("undo").addEventListener("click", () => undoRedo(false));
  $("redo").addEventListener("click", () => undoRedo(true));
  $("dz-canvas").addEventListener("scroll", placeBar);
  window.addEventListener("resize", fitHeight);
  // The tab becomes visible (main.js unhides it): size the columns then
  new MutationObserver(() => requestAnimationFrame(fitHeight)).observe($("tab-design"), { attributes: true, attributeFilter: ["hidden"] });
}

// ---------- send to the AP ----------

const ap = { tags: [], fileVersion: null, connected: false, mac: "", info: "" };

// The version of the model's layout file on the AP (GET /api/layouts), null when it has none
async function apFileVersion() {
  const r = await apClient().layouts();
  return r.layouts?.find((x) => x.model === state.model)?.version ?? null;
}

// Upload, send and check only work once Connect has reached the AP (through the bridge)
function apButtons(busy = false) {
  $("ap-connect").disabled = busy;
  $("ap-upload").disabled = busy || !ap.connected || isBuiltin();
  for (const id of ["ap-send", "ap-refresh"]) $(id).disabled = busy || !ap.connected || !ap.mac;
}

function apShow() {
  if (!doc()) return;    // before the first model is loaded
  $("ap-setup").hidden = ap.connected;
  $("ap-conn").hidden = $("ap-pick").hidden = !ap.connected;
  $("ap-conn-text").textContent = ap.info;
  $("send-h").textContent = `Send "${layoutName()}" to a tag`;
  refreshApTags();
  refreshSends();
  apButtons();
}

function apDisconnected() {
  ap.connected = false;
  ap.tags = [];
  apShow();
  apStatus("");
}

function apClient() {
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
  const mine = ap.tags.filter((t) => t.model === state.model || t.model === null);
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
  const where = apBase($("ap-url").value).base.replace(/^https?:\/\//, "");
  ap.info = `Connected to ${where} · AP ${r.ap?.fw || "?"}, ${ap.tags.length} tags`;
  return ["", ""];
}

async function apUpload() {
  const res = state.renderer.loadLayouts(JSON.stringify(doc()));
  if (!res.ok) throw new Error("Fix the layout file first: " + res.error);
  const r = await apClient().putLayouts(state.model, doc());
  ap.fileVersion = res.version;
  markClean();
  return [r.changed ? `Stored on the AP. It draws the ${state.model}" tags with it, and they get the new screen at their next poll.` : "The AP already has this file.", "ok"];
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
  const res = state.renderer.loadLayouts(JSON.stringify(doc()));
  if (!isBuiltin() && res.ok && ap.fileVersion !== res.version) {
    await apUpload();
    uploaded = true;
  }
  try {
    r = await apClient().putContent(mac, content);
  } catch (e) {
    // The AP does not have this layout (no layout file for the model yet, or an older one
    // without it): upload the editor's file, then send again
    if (isBuiltin() || uploaded || !/^422: layout .* is not in (the|any) layout file/.test(e.message)) throw e;
    await apUpload();
    r = await apClient().putContent(mac, content);
    uploaded = true;
  }
  const note = uploaded ? " The layout file was uploaded first." : "";
  return [(r.changed ? "Sent. The tag shows it after its next poll." : "The tag already has this content.") + note, "ok"];
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

function initAp() {
  const sess = {
    get(k) { try { return sessionStorage.getItem(STORE + k) || ""; } catch { return ""; } },
    set(k, v) { try { sessionStorage.setItem(STORE + k, v); } catch { /* private mode */ } },
  };
  let url = "";
  try { url = localStorage.getItem(DASH_URL) || store.get("ap.url") || ""; } catch { /* private mode */ }
  $("ap-url").value = url;
  $("ap-token").value = sess.get("ap.token");
  $("ap-url").addEventListener("change", () => {
    try { localStorage.setItem(DASH_URL, $("ap-url").value.trim()); } catch { /* private mode */ }
    apDisconnected();
  });
  $("ap-token").addEventListener("change", () => { sess.set("ap.token", $("ap-token").value.trim()); apDisconnected(); });
  $("ap-connect").addEventListener("click", () => apRun("Connecting", apConnect));
  $("ap-change").addEventListener("click", apDisconnected);
  $("ap-upload").addEventListener("click", () => apRun("Uploading", apUpload));
  $("ap-send").addEventListener("click", () => apRun("Sending", apSend));
  $("ap-refresh").addEventListener("click", () => apRun("Asking the AP", apCheck));
  $("send-open").addEventListener("click", () => {
    apStatus("");
    apShow();
    $("send-dlg").showModal();
    // The Dashboard's address may have changed since
    try {
      const u = localStorage.getItem(DASH_URL);
      if (u && u !== $("ap-url").value && !ap.connected) $("ap-url").value = u;
    } catch { /* private mode */ }
    if ($("ap-url").value && !ap.connected) apRun("Connecting", apConnect);
    else if (ap.connected) apRun("Reading the tags", async () => {
      ap.tags = (await apClient().tags()).tags || [];
      return ["", ""];
    });
  });
  apDisconnected();
}

export async function initEditor() {
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
  const m = store.get("model");
  await loadModel(MODELS[m] ? m : Object.keys(MODELS)[0]);
  fitHeight();
}
