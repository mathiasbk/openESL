// editor.js - the layout designer. Edits a layout file (docs/protokoll.md part 3) for one tag
// model and draws it live with the WebAssembly renderer, which is the tag's own drawing code
// and the AP's own compiler: what the preview shows is what the tag draws, and an error here
// is the 422 the AP would answer when the file is uploaded.

import { History } from "./history.js";
import { layoutUsage } from "./limits.js";
import { snapMove, resizeBox } from "./snap.js";
import { ApClient, apBase } from "./apclient.js";
import { loadRenderer, MODELS, LAYOUT_PRICE, LAYOUT_TEXT, LAYOUT_FILE, FLAG_PRICE_HIGHLIGHT, FLAG_FRAME } from "./renderer.js";

const FONTS = ["small", "medium", "mid", "large"];
const ALIGN = ["left", "center", "right"];
const VALIGN = ["top", "middle", "bottom"];
const TEXT_COLORS = ["black", "white", "invert", "highlight", "frame"];
const RECT_COLORS = ["black", "white", "frame"];
const BUILTIN = {
  price: { id: LAYOUT_PRICE, fields: ["name", "price", "subtitle"] },
  text: { id: LAYOUT_TEXT, fields: ["line1", "line2", "line3", "line4"] },
};
const STORE = "openesl.editor.";

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

// The layouts of the example file, offered as templates. Their "_sample" values (ignored by
// the AP) fill the preview.
const templates = {};

async function refreshTemplates(model) {
  try {
    templates[model] ||= (await exampleDoc(model)).layouts || [];
  } catch {
    templates[model] = [];
  }
  if (model !== state.model) return;
  const sel = $("layout-template");
  sel.length = 1;
  templates[model].forEach((l, i) => {
    const o = document.createElement("option");
    o.value = i;
    o.textContent = l.name + (l.description ? " - " + l.description : "");
    sel.append(o);
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
  }
  if (!state.history[model]) {
    state.history[model] = new History();
    state.history[model].reset(JSON.stringify(state.docs[model]));
  }
  state.renderer = await loadRenderer(model);
  $("model").value = model;
  applyZoom();
  document.querySelectorAll(".model-name").forEach((e) => (e.textContent = model));
  state.sel = doc().layouts?.length ? "file:0" : "builtin:price";
  state.el = -1;
  refreshAll();
  refreshApTags();
  refreshTemplates(model);
}

// ---------- drawing ----------

function compileAndDraw() {
  const r = state.renderer;
  const status = $("status");
  const res = r.loadLayouts(JSON.stringify(doc()));
  let layoutId;
  if (isBuiltin()) {
    layoutId = BUILTIN[state.sel.slice(8)].id;
  } else if (!res.ok) {
    setStatus("AP would reject the file: " + res.error, "error");
    refreshMeters(res);
    dimCanvas();
    return;
  } else {
    layoutId = LAYOUT_FILE + +state.sel.slice(5);
  }
  const flags = ($("opt-highlight").checked ? FLAG_PRICE_HIGHLIGHT : 0) | ($("opt-frame").checked ? FLAG_FRAME : 0);
  const out = r.render(layoutId, flags, samples());
  if (!out.ok) {
    setStatus(out.error, "error");
    dimCanvas();
    return;
  }
  paint(out.pixels, r.width, r.height);
  refreshMeters(res);
  if (res.ok) setStatus(`✓ The AP accepts this file (version ${res.version.toString(16).toUpperCase().padStart(8, "0")})`, "ok");
  else setStatus("Built-in layout (the layout file has an error: " + res.error + ")", "error");
}

// What the layout uses of the tag's limits, so you see a limit coming before the AP says no.
function refreshMeters(res) {
  const box = $("meters");
  const r = state.renderer;
  const lim = r.limits;
  const lay = layout();
  const meters = [];
  $("el-add-barcode").disabled = false;
  if (lay) {
    const u = layoutUsage(lay, lim);
    meters.push(["Text lines", u.runs, lim.runs, "Lines of text the tag may draw for this layout (each text box counts the most lines its fonts can fit)"]);
    meters.push(["Boxes", u.rects, lim.rects, "Filled boxes, lines and inverted or highlighted text in this layout"]);
    if (u.barcodes) meters.push(["Barcodes", u.barcodes, lim.barcodes, "Barcodes in this layout"]);
    $("el-add-barcode").disabled = u.barcodes >= lim.barcodes;
  }
  meters.push(["File", res.ok ? res.size : null, lim.fileBytes, "Size of the whole layout file the tags download, in bytes"]);
  if (lay) meters.push(["Fields", lay.fields.length, lim.fields, "Text fields the client sends for this layout, e.g. price and price_ex"]);
  const t = textBudget();
  meters.push(["Sample text", t.n, t.max, "Bytes of text in one update from the AP (the fields share 96 bytes, one per field plus the text)"]);
  box.replaceChildren(...meters.map(([name, n, max, title]) => {
    const m = document.createElement("span");
    m.className = "meter" + (n === null ? "" : n > max ? " over" : n >= max ? " near" : "");
    m.title = title;
    const bar = document.createElement("i");
    bar.style.setProperty("--p", n === null ? "0%" : Math.min(100, (100 * n) / max) + "%");
    const b = document.createElement("b");
    b.textContent = (n === null ? "?" : n) + " / " + max;
    m.append(name + " ", b, bar);
    return m;
  }));
}

function setStatus(text, cls) {
  const s = $("status");
  s.textContent = text;
  s.className = "status " + (cls || "");
}

function paint(pixels, w, h) {
  const c = $("screen");
  if (c.width !== w || c.height !== h) [c.width, c.height] = [w, h];
  const ctx = c.getContext("2d");
  const img = ctx.createImageData(w, h);
  for (let i = 0; i < w * h; i++) {
    const v = pixels[i] ? 17 : 255;
    img.data.set([v, v, v, 255], i * 4);
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
  if (!lay) return;
  lay.elements.forEach((e, i) => {
    const b = document.createElement("div");
    b.className = "box" + (e.type === "rect" ? " rect" : e.type === "barcode" ? " barcode" : "") + (i === state.el ? " sel" : "");
    Object.assign(b.style, {
      left: (100 * e.x) / width + "%", top: (100 * e.y) / height + "%",
      width: (100 * e.w) / width + "%", height: (100 * e.h) / height + "%",
    });
    b.dataset.i = i;
    const tag = document.createElement("span");
    tag.className = "tag";
    tag.textContent = e.type === "rect" ? "rect" : e.type === "barcode" ? "▥ " + fieldLabel(e.field)
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
}

// The list of elements in drawing order, so a box hidden under another can still be picked.
function refreshList() {
  const ol = $("el-list");
  const lay = layout();
  $("list-panel").hidden = !lay;
  if (!lay) return;
  const usage = layoutUsage(lay, state.renderer.limits).elements;
  ol.replaceChildren(...lay.elements.map((e, i) => {
    const li = document.createElement("li");
    li.className = i === state.el ? "sel" : "";
    li.dataset.i = i;
    const ico = document.createElement("span");
    const inverted = e.type === "text" && (e.color === "invert" || e.color === "highlight");
    ico.className = "ico" + (e.type === "rect" ? " rect" : e.type === "barcode" ? " barcode" : inverted ? " inv" : "");
    const name = document.createElement("span");
    name.className = "name";
    name.textContent = elementName(e);
    const dim = document.createElement("span");
    dim.className = "dim";
    const u = usage[i];
    dim.textContent = `${e.x},${e.y} ${e.w}×${e.h}` + (u.runs ? ` · ${u.runs} ln` : "");
    dim.title = `x ${e.x}, y ${e.y}, ${e.w} × ${e.h} px` + (u.runs ? `, up to ${u.runs} lines of text` : "") + (u.rects ? ", uses a box" : "");
    li.append(ico, name, dim);
    return li;
  }));
}

function elementName(e) {
  if (e.type === "barcode") return `barcode (${fieldLabel(e.field)}, ${e.symbology === "ean13" ? "EAN-13" : "Code 128"})`;
  if (e.type !== "text") return rectName(e);
  const what = e.text !== undefined ? `"${e.text}"` : fieldLabel(e.field);
  return what + (e.color && e.color !== "black" ? ` (${e.color})` : "");
}

function rectName(e) {
  const kind = e.h <= 2 && e.w > e.h ? "line" : e.w <= 2 && e.h > e.w ? "vertical line" : "box";
  return kind + (e.color && e.color !== "black" ? ` (${e.color})` : "");
}

function initList() {
  const ol = $("el-list");
  ol.addEventListener("click", (ev) => {
    const li = ev.target.closest("li");
    if (!li) return;
    state.el = +li.dataset.i;
    refreshElement();
  });
  const hover = (i) => document.querySelectorAll("#overlay .box").forEach((b) => b.classList.toggle("hover", b.dataset.i === i));
  ol.addEventListener("pointerover", (ev) => hover(ev.target.closest("li")?.dataset.i));
  ol.addEventListener("pointerleave", () => hover(null));
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
  };
  ov.addEventListener("pointerup", end);
  ov.addEventListener("pointercancel", end);

  document.addEventListener("keydown", onKey);
}

// Keyboard shortcuts, only while the Design tab is shown. In a text box the browser's own
// keys win (its undo, arrows, Delete).
function onKey(ev) {
  if ($("tab-design").hidden || ev.target.closest("input, textarea, select")) return;
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
  else if (key === "Escape" && state.el >= 0) {
    state.el = -1;
    refreshElement();
  } else if (key === "Tab" && layout() && ev.target === document.body) {
    const n = layout().elements.length;
    state.el = ((state.el < 0 ? (ev.shiftKey ? 0 : -1) : state.el) + (ev.shiftKey ? n - 1 : 1)) % n;
    refreshElement();
  } else return;
  ev.preventDefault();
}

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

// ---------- forms ----------

function refreshLayoutSelect() {
  const s = $("layout-select");
  s.replaceChildren();
  (doc().layouts || []).forEach((l, i) => s.add(new Option(l.name || "(no name)", "file:" + i)));
  s.add(new Option("price (built-in)", "builtin:price"));
  s.add(new Option("text (built-in)", "builtin:text"));
  s.value = state.sel;
  $("layout-dup").disabled = $("layout-del").disabled = isBuiltin();
  $("layout-del").disabled ||= doc().layouts.length <= 1;
}

function refreshLayoutForm() {
  const lay = layout();
  $("layout-panel").hidden = !lay;
  $("add-group").hidden = !lay;
  if (!lay) return;
  $("lay-name").value = lay.name || "";
  $("lay-fields").value = (lay.fields || []).join(", ");
  $("lay-desc").value = lay.description || "";
}

function refreshSample() {
  const box = $("sample");
  box.replaceChildren();
  const names = fieldNames();
  const vals = samples();
  names.forEach((n, i) => {
    const l = document.createElement("label");
    l.textContent = n;
    const inp = document.createElement("input");
    inp.value = vals[i];
    inp.addEventListener("input", () => {
      vals[i] = inp.value;
      countBytes();
      compileAndDraw();
      save();
    });
    l.append(inp);
    box.append(l);
  });
  const c = document.createElement("div");
  c.className = "count";
  c.id = "sample-count";
  box.append(c);
  countBytes();
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
  c.textContent = `${n} of ${max} bytes of text`;
  c.classList.toggle("error", n > max);
}

function refreshElement() {
  const box = $("el-form");
  box.replaceChildren();
  const e = element();
  $("el-panel").hidden = !layout();
  document.querySelector(".el-actions").hidden = !e;
  $("el-up").disabled = !e || state.el === 0;
  $("el-down").disabled = !e || state.el === layout().elements.length - 1;
  $("el-title").textContent = e ? "· " + elementName(e) : "";
  drawOverlay();
  if (!e) {
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = layout() ? "Click a box in the preview or in the list above to edit it, or add one with the buttons over the preview." : "Built-in layouts are part of the firmware and cannot be edited. Pick a layout from the file, or make a new one, to design your own.";
    box.append(p);
    return;
  }
  const lay = layout();
  const sel = (label, key, options, val, onSet) => {
    const l = document.createElement("label");
    l.textContent = label;
    const s = document.createElement("select");
    options.forEach((o) => s.add(new Option(o, o)));
    s.value = val;
    s.addEventListener("change", () => { onSet(s.value); changed({}); });
    l.append(s);
    box.append(l);
    return s;
  };
  // A row of buttons for a few choices, so the choice is visible without opening a list.
  const seg = (label, options, val, onSet) => {
    const l = document.createElement("label");
    l.textContent = label;
    const g = document.createElement("span");
    g.className = "seg";
    g.setAttribute("role", "radiogroup");
    g.setAttribute("aria-label", label);
    for (const [v, icon, name] of options) {
      const b = document.createElement("button");
      b.type = "button";
      b.textContent = icon + " " + name;
      b.setAttribute("role", "radio");
      b.setAttribute("aria-checked", String(v === val));
      b.addEventListener("click", () => { onSet(v); changed({}); });
      g.append(b);
    }
    l.append(g);
    box.append(l);
  };
  if (e.type === "barcode") {
    sel("Field", "field", lay.fields, fieldLabel(e.field), (v) => (e.field = v));
  } else if (e.type === "text") {
    // A field filled in by each update, or a fixed text that is part of the layout.
    const FIXED = "(fixed text)";
    sel("Shows", "field", [...lay.fields, FIXED], e.text !== undefined ? FIXED : fieldLabel(e.field), (v) => {
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
        refreshList();
        $("el-title").textContent = "· " + elementName(e);
      });
      l.append(inp);
      box.append(l);
    }
  }
  // position and size
  const pair = document.createElement("div");
  pair.className = "pair";
  for (const k of ["x", "y", "w", "h"]) {
    const l = document.createElement("label");
    l.textContent = k;
    const inp = document.createElement("input");
    inp.type = "number";
    inp.min = k === "w" || k === "h" ? 1 : 0;
    inp.value = e[k];
    inp.dataset.k = k;
    inp.addEventListener("input", () => {
      if (inp.value === "") return;
      e[k] = Math.round(+inp.value);
      changed({ keepForm: true });
    });
    l.append(inp);
    pair.append(l);
  }
  box.append(pair);
  box.append(placeButtons(e));
  if (e.type === "text") {
    // Up to three fonts on one row; the tag uses the first that fits the text.
    const fonts = e.fonts || ["small"];
    const fl = document.createElement("label");
    fl.textContent = "Fonts";
    fl.title = "Tried in order: the tag uses the first font the text fits in";
    const row = document.createElement("span");
    row.className = "fonts";
    for (let i = 0; i < 3; i++) {
      const s = document.createElement("select");
      s.setAttribute("aria-label", i ? `Font ${i + 1}, if the text does not fit` : "Font");
      (i ? ["–", ...FONTS] : FONTS).forEach((o) => s.add(new Option(o, o)));
      s.value = fonts[i] || "–";
      s.disabled = i > fonts.length;
      s.addEventListener("change", () => {
        const f = (e.fonts || ["small"]).slice();
        if (s.value === "–") f.length = Math.min(f.length, i);
        else f[i] = s.value;
        e.fonts = f.filter(Boolean);
        changed({});
      });
      row.append(s);
    }
    fl.append(row);
    box.append(fl);
    seg("Align", [["left", "⯇", "Left"], ["center", "≡", "Centre"], ["right", "⯈", "Right"]], e.align || "left",
      (v) => (v === "left" ? delete e.align : (e.align = v)));
    seg("Vertical", [["top", "⯅", "Top"], ["middle", "≡", "Middle"], ["bottom", "⯆", "Bottom"]], e.valign || "top",
      (v) => (v === "top" ? delete e.valign : (e.valign = v)));
    sel("Lines", "lines", ["0 (auto)", "1", "2", "3"], e.lines ? String(e.lines) : "0 (auto)", (v) => {
      const n = parseInt(v, 10);
      if (n) e.lines = n;
      else delete e.lines;
    });
  }
  if (e.type === "barcode") {
    seg("Code", [["code128", "", "Code 128"], ["ean13", "", "EAN-13"]], e.symbology,
      (v) => (e.symbology = v));
    const lim = state.renderer.limits;
    sel("Module", "module", ["auto", ...Array.from({ length: lim.barcodeModuleMax }, (_, i) => i + 1 + " px")],
      e.module ? e.module + " px" : "auto", (v) => {
        const n = parseInt(v, 10);
        if (n) e.module = n;
        else delete e.module;
      });
    seg("Align", [["left", "⯇", "Left"], ["center", "≡", "Centre"], ["right", "⯈", "Right"]], e.align || "left",
      (v) => (v === "left" ? delete e.align : (e.align = v)));
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = (e.symbology === "ean13"
      ? "EAN-13 takes 12 digits (the tag adds the check digit) or 13. "
      : "Code 128 takes letters, digits and signs; digits only give the shortest code. ") +
      `The bars fill the box, which must be at least ${lim.barcodeMinH} px high; "auto" uses the widest module that fits. ` +
      "On the 2.9\", 2 px modules scan reliably.";
    box.append(p);
    return;
  }
  sel("Colour", "color", e.type === "text" ? TEXT_COLORS : RECT_COLORS, e.color || "black",
    (v) => (v === "black" ? delete e.color : (e.color = v)));
}

// Buttons that put the element against an edge or in the middle of the screen, or stretch it.
function placeButtons(e) {
  const { width, height } = MODELS[state.model];
  const row = document.createElement("div");
  row.className = "place";
  const actions = [
    ["⇤", "Against the left edge", () => (e.x = 0)],
    ["↔", "Centre across", () => (e.x = Math.floor((width - e.w) / 2))],
    ["⇥", "Against the right edge", () => (e.x = width - e.w)],
    ["⤒", "Against the top", () => (e.y = 0)],
    ["↕", "Centre up and down", () => (e.y = Math.floor((height - e.h) / 2))],
    ["⤓", "Against the bottom", () => (e.y = height - e.h)],
    ["Width", "Stretch across the whole screen", () => ((e.x = 0), (e.w = width))],
    ["Height", "Stretch over the whole height", () => ((e.y = 0), (e.h = height))],
  ];
  for (const [label, title, act] of actions) {
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = label;
    b.title = title;
    b.setAttribute("aria-label", title);
    b.addEventListener("click", () => {
      act();
      changed({ form: true });
    });
    row.append(b);
  }
  return row;
}

function updatePositionInputs() {
  const e = element();
  if (!e) return;
  document.querySelectorAll("#el-form .pair input").forEach((inp) => {
    if (document.activeElement !== inp) inp.value = e[inp.dataset.k];
  });
}

function refreshJson() {
  const t = $("json");
  if (document.activeElement !== t) t.value = JSON.stringify(doc(), null, 2);
  $("upload-cmd").textContent =
    `curl -X PUT http://openesl.local/api/layouts/${state.model} \\\n  -H "Authorization: Bearer $TOKEN" \\\n  --data-binary @${state.model}.json`;
}

function refreshAll() {
  refreshLayoutSelect();
  refreshLayoutForm();
  refreshSample();
  refreshElement();
  refreshJson();
  refreshUndo();
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
  const text = { type: "text", field: lay.fields[0], x: 4, y: 4, w: Math.min(120, width - 4), h: 24, fonts: ["medium", "small"] };
  const e = {
    text,
    invert: { ...text, h: 32, color: "invert", lines: 1 },
    fixed: { type: "text", text: "inkl. mva", x: 4, y: height - 22, w: Math.min(100, width - 4), h: 18, fonts: ["small"] },
    rect: { type: "rect", x: 0, y: 0, w: Math.min(80, width), h: 30 },
    hline: { type: "rect", x: 4, y: Math.floor(height / 2), w: width - 8, h: 2 },
    vline: { type: "rect", x: Math.floor(width / 2), y: 4, w: 2, h: height - 8 },
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
  changed();
}

function uniqueName(base) {
  const names = new Set(doc().layouts.map((l) => l.name));
  let n = 2;
  let name = base;
  while (names.has(name)) name = `${base}-${n++}`.slice(0, 31);
  return name;
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

// Fit: as wide as the column, but never taller than about 60% of the window, so the whole
// tag and the buttons under it stay in view (the 1.54" is square). Otherwise tag pixels.
function applyZoom() {
  const z = $("zoom").value;
  const { width, height } = MODELS[state.model];
  $("stage").style.width = z === "fit" ? `min(100%, calc(60vh * ${width / height}))` : `${z * width}px`;
}

function initActions() {
  for (const [name, m] of Object.entries(MODELS))
    $("model").append(new Option(`${name}" (${m.width} × ${m.height})`, name));
  $("zoom").value = store.get("zoom") || "fit";
  $("zoom").addEventListener("change", () => {
    store.set("zoom", $("zoom").value);
    applyZoom();
  });
  $("model").addEventListener("change", (ev) => loadModel(ev.target.value).then(save));
  $("layout-select").addEventListener("change", (ev) => {
    state.sel = ev.target.value;
    state.el = -1;
    refreshAll();
  });
  $("layout-add").addEventListener("click", () => {
    const d = emptyDoc(state.model).layouts[0];
    d.name = uniqueName("layout");
    doc().layouts.push(d);
    state.sel = "file:" + (doc().layouts.length - 1);
    state.el = -1;
    refreshAll();
    save();
  });
  $("layout-template").addEventListener("change", (ev) => {
    const t = templates[state.model]?.[+ev.target.value];
    ev.target.value = "";
    if (!t) return;
    const c = structuredClone(t);
    c.name = uniqueName(c.name);
    doc().layouts.push(c);
    state.sel = "file:" + (doc().layouts.length - 1);
    state.el = -1;
    refreshAll();
    save();
  });
  $("layout-dup").addEventListener("click", () => {
    const c = structuredClone(layout());
    c.name = uniqueName(c.name);
    doc().layouts.push(c);
    state.sel = "file:" + (doc().layouts.length - 1);
    refreshAll();
    save();
  });
  $("layout-del").addEventListener("click", () => {
    if (doc().layouts.length <= 1 || !confirm(`Delete the layout "${layout().name}"?`)) return;
    doc().layouts.splice(+state.sel.slice(5), 1);
    state.sel = "file:0";
    state.el = -1;
    refreshAll();
    save();
  });
  $("file-example").addEventListener("click", async () => {
    if (!confirm(`Replace the ${state.model}" layout file in the editor with the example from the repository?`)) return;
    try {
      setDoc(await exampleDoc(state.model));
      state.sel = "file:0";
      refreshAll();
      save();
    } catch (e) {
      setStatus(e.message, "error");
    }
  });
  $("file-open").addEventListener("change", async (ev) => {
    const f = ev.target.files[0];
    ev.target.value = "";
    if (!f) return;
    try {
      setDoc(JSON.parse(await f.text()));
      state.sel = "file:0";
      refreshAll();
      save();
    } catch (e) {
      setStatus("Could not read " + f.name + ": " + e.message, "error");
    }
  });
  $("file-save").addEventListener("click", () => download(state.model + ".json", JSON.stringify(doc(), null, 2) + "\n"));
  $("design-save").addEventListener("click", () => {
    const name = isBuiltin() ? state.sel.slice(8) : layout().name;
    const fields = {};
    fieldNames().forEach((n) => (fields[n] = `{${n}}`));
    const design = {
      format: "esl-label", version: 1, id: name.replace(/[^a-z0-9-]/g, "-").slice(0, 40),
      layout: name, fields,
      style: { price_highlight: $("opt-highlight").checked, frame: $("opt-frame").checked },
    };
    download(design.id + ".json", JSON.stringify(design, null, 2) + "\n");
  });
  $("lay-name").addEventListener("input", (ev) => {
    const old = sampleKey();
    layout().name = ev.target.value.trim();
    state.samples[sampleKey()] = state.samples[old];
    refreshLayoutSelect();
    refreshJson();
    compileAndDraw();
    save();
  });
  $("lay-fields").addEventListener("change", (ev) => {
    layout().fields = ev.target.value.split(/[,\s]+/).filter(Boolean);
    refreshSample();
    changed();
  });
  $("lay-desc").addEventListener("input", (ev) => {
    if (ev.target.value) layout().description = ev.target.value;
    else delete layout().description;
    refreshJson();
    save();
  });
  $("json").addEventListener("input", (ev) => {
    try {
      setDoc(JSON.parse(ev.target.value));
    } catch (e) {
      setStatus("JSON: " + e.message, "error");
      return;
    }
    refreshLayoutSelect();
    refreshLayoutForm();
    refreshSample();
    refreshElement();
    compileAndDraw();
    save();
  });
  $("json").addEventListener("blur", refreshJson);
  for (const id of ["opt-highlight", "opt-frame"]) $(id).addEventListener("change", compileAndDraw);
  $("opt-boxes").addEventListener("change", drawOverlay);
  $("el-add-text").addEventListener("click", () => addElement("text"));
  for (const k of ["invert", "fixed", "rect", "hline", "vline", "barcode"]) $("el-add-" + k).addEventListener("click", () => addElement(k));
  $("el-del").addEventListener("click", deleteElement);
  $("el-dup").addEventListener("click", duplicateElement);
  $("undo").addEventListener("click", () => undoRedo(false));
  $("redo").addEventListener("click", () => undoRedo(true));
  $("el-up").addEventListener("click", () => moveElement(-1));
  $("el-down").addEventListener("click", () => moveElement(1));
}

// ---------- send to the AP ----------

const ap = { tags: [], fileVersion: null, connected: false };

// Upload, send and check only work once Connect has reached the AP (through the bridge)
function apButtons(busy = false) {
  $("ap-connect").disabled = busy;
  for (const id of ["ap-upload", "ap-send", "ap-refresh"]) $(id).disabled = busy || !ap.connected;
}

function apDisconnected() {
  ap.connected = false;
  ap.tags = [];
  refreshApTags();
  apButtons();
  apStatus("Not connected. Start the bridge (step 1), then press Connect.");
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

function refreshApTags() {
  const sel = $("ap-tag");
  const keep = sel.value;
  sel.replaceChildren();
  const mine = ap.tags.filter((t) => t.model === state.model || t.model === null);
  if (!mine.length) sel.add(new Option(ap.tags.length ? `(no ${state.model}" tags on the AP)` : "(connect first)", ""));
  for (const t of mine) sel.add(new Option(`${t.mac} · ${ago(t.seen_s_ago)}${t.pending ? " · pending" : ""}`, t.mac));
  if (mine.some((t) => t.mac === keep)) sel.value = keep;
}

async function apRun(what, fn) {
  apStatus(what + "…");
  apButtons(true);
  try {
    apStatus(...(await fn()));
  } catch (e) {
    apStatus(e.message, "error");
  } finally {
    apButtons();
  }
}

async function apConnect() {
  ap.connected = false;
  const r = await apClient().tags();
  ap.connected = true;
  ap.tags = r.tags || [];
  refreshApTags();
  const n = ap.tags.filter((t) => t.model === state.model).length;
  return [`AP ${r.ap?.fw || "?"}, channel ${r.ap?.channel ?? "?"}: ${ap.tags.length} tags, ${n} of them ${state.model}".`, "ok"];
}

async function apUpload() {
  const res = state.renderer.loadLayouts(JSON.stringify(doc()));
  if (!res.ok) throw new Error("Fix the layout file first: " + res.error);
  const r = await apClient().putLayouts(state.model, doc());
  ap.fileVersion = res.version;
  return [r.changed ? `Stored on the AP. The ${state.model}" tags download it at their next poll.` : "The AP already has this file.", "ok"];
}

async function apSend() {
  const mac = $("ap-tag").value;
  if (!mac) throw new Error("Connect and pick a tag first.");
  const name = isBuiltin() ? state.sel.slice(8) : layout().name;
  const content = { layout: name, fields: samples().slice() };
  if ($("opt-highlight").checked) content.price_highlight = true;
  if ($("opt-frame").checked) content.frame = true;
  let r, uploaded = false;
  try {
    r = await apClient().putContent(mac, content);
  } catch (e) {
    // The AP does not have this layout (no layout file for the model yet, or an older one
    // without it): upload the editor's file, then send again
    if (isBuiltin() || !/^422: layout .* is not in (the|any) layout file/.test(e.message)) throw e;
    await apUpload();
    r = await apClient().putContent(mac, content);
    uploaded = true;
  }
  let note = uploaded ? ` The AP did not have the layout "${name}", so the layout file was uploaded first.` : "";
  const t = ap.tags.find((x) => x.mac === mac);
  const res = state.renderer.loadLayouts(JSON.stringify(doc()));
  if (!uploaded && !isBuiltin() && res.ok && t && t.layout_version !== res.version && ap.fileVersion !== res.version) {
    note = " The tag has another version of the layout file than the editor; upload the file too.";
  }
  return [(r.changed ? "Sent. The tag shows it after its next poll." : "The tag already has this content.") + note, "ok"];
}

async function apCheck() {
  const mac = $("ap-tag").value;
  if (!mac) throw new Error("Connect and pick a tag first.");
  const t = await apClient().tag(mac);
  const i = ap.tags.findIndex((x) => x.mac === mac);
  if (i >= 0) ap.tags[i] = t;
  refreshApTags();
  if (t.content_error) return [`Cannot be sent: ${t.content_error}.`, "error"];
  if (t.layout_failed) return ["The tag rejected the layout file.", "error"];
  if (t.draw_failed) return ["The tag says its last drawing failed.", "error"];
  if (t.layout_pending) return [`The tag is downloading the layout file (seen ${ago(t.seen_s_ago)}).`, ""];
  if (t.pending) return [`Waiting for the tag's next poll (seen ${ago(t.seen_s_ago)}).`, ""];
  return [`The tag shows it (seen ${ago(t.seen_s_ago)}).`, "ok"];
}

function initAp() {
  const sess = {
    get(k) { try { return sessionStorage.getItem(STORE + k) || ""; } catch { return ""; } },
    set(k, v) { try { sessionStorage.setItem(STORE + k, v); } catch { /* private mode */ } },
  };
  $("ap-url").value = store.get("ap.url") || "";
  $("ap-token").value = sess.get("ap.token");
  $("ap-url").addEventListener("change", () => { store.set("ap.url", $("ap-url").value.trim()); apDisconnected(); });
  $("ap-token").addEventListener("change", () => { sess.set("ap.token", $("ap-token").value.trim()); apDisconnected(); });
  $("ap-connect").addEventListener("click", () => apRun("Connecting", apConnect));
  $("ap-upload").addEventListener("click", () => apRun("Uploading", apUpload));
  $("ap-send").addEventListener("click", () => apRun("Sending", apSend));
  $("ap-refresh").addEventListener("click", () => apRun("Asking the AP", apCheck));
  apDisconnected();
}

export async function initEditor() {
  try {
    state.samples = JSON.parse(store.get("samples") || "{}");
  } catch {
    state.samples = {};
  }
  initActions();
  initDrag();
  initList();
  initAp();
  const m = store.get("model");
  await loadModel(MODELS[m] ? m : Object.keys(MODELS)[0]);
}
