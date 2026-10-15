// editor.js - the layout designer. Edits a layout file (docs/protokoll.md part 3) for one tag
// model and draws it live with the WebAssembly renderer, which is the tag's own drawing code
// and the AP's own compiler: what the preview shows is what the tag draws, and an error here
// is the 422 the AP would answer when the file is uploaded.

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
  return name.charAt(0).toUpperCase() + name.slice(1).replace(/_/g, " ");
}

function samples() {
  const k = sampleKey();
  const names = fieldNames();
  const s = state.samples[k] || [];
  while (s.length < names.length) s.push(defaultSample(names[s.length]));
  state.samples[k] = s.slice(0, names.length);
  return state.samples[k];
}

// ---------- persistence ----------

function save() {
  store.set("doc." + state.model, JSON.stringify(doc()));
  store.set("model", state.model);
  store.set("samples", JSON.stringify(state.samples));
}

async function exampleDoc(model) {
  const r = await fetch(new URL("../designs/layouts/" + model + ".json", import.meta.url));
  if (!r.ok) throw new Error("could not load the example (" + r.status + ")");
  return r.json();
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
  state.renderer = await loadRenderer(model);
  $("model").value = model;
  document.querySelectorAll(".model-name").forEach((e) => (e.textContent = model));
  state.sel = doc().layouts?.length ? "file:0" : "builtin:price";
  state.el = -1;
  refreshAll();
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
  if (res.ok) setStatus(`OK: ${res.size} of 768 bytes for the tag, version ${res.version.toString(16).toUpperCase().padStart(8, "0")}`, "ok");
  else setStatus("Built-in layout (the layout file has an error: " + res.error + ")", "error");
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
    b.className = "box" + (e.type === "rect" ? " rect" : "") + (i === state.el ? " sel" : "");
    Object.assign(b.style, {
      left: (100 * e.x) / width + "%", top: (100 * e.y) / height + "%",
      width: (100 * e.w) / width + "%", height: (100 * e.h) / height + "%",
    });
    b.dataset.i = i;
    const tag = document.createElement("span");
    tag.className = "tag";
    tag.textContent = e.type === "rect" ? "rect" : fieldLabel(e.field);
    const grip = document.createElement("span");
    grip.className = "grip";
    b.append(tag, grip);
    ov.append(b);
  });
}

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
      resize: ev.target.classList.contains("grip"),
      sx: ev.clientX, sy: ev.clientY,
      scale: rect.width / MODELS[state.model].width,
      start: { ...e },
    };
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
    if (drag.resize) {
      e.w = clamp(s.w + dx, 1, width - s.x);
      e.h = clamp(s.h + dy, 1, height - s.y);
    } else {
      e.x = clamp(s.x + dx, 0, width - s.w);
      e.y = clamp(s.y + dy, 0, height - s.h);
    }
    changed({ form: true });
  });
  const end = () => {
    if (drag) save();
    drag = null;
  };
  ov.addEventListener("pointerup", end);
  ov.addEventListener("pointercancel", end);

  document.addEventListener("keydown", (ev) => {
    const e = element();
    if (!e || ev.target.closest("input, textarea, select")) return;
    const step = ev.shiftKey ? 10 : 1;
    const { width, height } = MODELS[state.model];
    const moves = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] };
    if (moves[ev.key]) {
      e.x = clamp(e.x + moves[ev.key][0], 0, width - e.w);
      e.y = clamp(e.y + moves[ev.key][1], 0, height - e.h);
      changed({ form: true });
      ev.preventDefault();
    } else if (ev.key === "Delete") {
      deleteElement();
    }
  });
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
  $("add-row").hidden = !lay;
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

function countBytes() {
  const r = state.renderer;
  const n = samples().reduce((a, s) => a + r.textBytes(s), 0);
  const c = $("sample-count");
  c.textContent = `${n} of ${r.maxText} bytes of text`;
  c.classList.toggle("error", n > r.maxText);
}

function refreshElement() {
  const box = $("el-form");
  box.replaceChildren();
  const e = element();
  $("el-panel").hidden = !layout();
  $("el-del").disabled = $("el-up").disabled = $("el-down").disabled = !e;
  drawOverlay();
  if (!e) {
    const p = document.createElement("p");
    p.className = "hint";
    p.textContent = layout() ? "Click a box in the preview to edit it." : "Built-in layouts are part of the firmware and cannot be edited.";
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
  if (e.type === "text") {
    sel("Field", "field", lay.fields, fieldLabel(e.field), (v) => (e.field = v));
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
  if (e.type === "text") {
    const fonts = e.fonts || ["small"];
    for (let i = 0; i < 3; i++) {
      sel(i ? `then font` : "Font", "font" + i, i ? ["(none)", ...FONTS] : FONTS, fonts[i] || "(none)", (v) => {
        const f = (e.fonts || ["small"]).slice();
        if (v === "(none)") f.length = Math.min(f.length, i);
        else f[i] = v;
        e.fonts = f.filter(Boolean);
      });
    }
    sel("Align", "align", ALIGN, e.align || "left", (v) => (v === "left" ? delete e.align : (e.align = v)));
    sel("Vertical", "valign", VALIGN, e.valign || "top", (v) => (v === "top" ? delete e.valign : (e.valign = v)));
    sel("Lines", "lines", ["0 (auto)", "1", "2", "3"], e.lines ? String(e.lines) : "0 (auto)", (v) => {
      const n = parseInt(v, 10);
      if (n) e.lines = n;
      else delete e.lines;
    });
  }
  sel("Colour", "color", e.type === "text" ? TEXT_COLORS : RECT_COLORS, e.color || "black",
    (v) => (v === "black" ? delete e.color : (e.color = v)));
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
    rect: { type: "rect", x: 0, y: 0, w: Math.min(80, width), h: 30 },
    hline: { type: "rect", x: 4, y: Math.floor(height / 2), w: width - 8, h: 2 },
    vline: { type: "rect", x: Math.floor(width / 2), y: 4, w: 2, h: height - 8 },
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

function initActions() {
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
  for (const k of ["invert", "rect", "hline", "vline"]) $("el-add-" + k).addEventListener("click", () => addElement(k));
  $("el-del").addEventListener("click", deleteElement);
  $("el-up").addEventListener("click", () => moveElement(-1));
  $("el-down").addEventListener("click", () => moveElement(1));
}

export async function initEditor() {
  try {
    state.samples = JSON.parse(store.get("samples") || "{}");
  } catch {
    state.samples = {};
  }
  initActions();
  initDrag();
  const m = store.get("model");
  await loadModel(MODELS[m] ? m : "2.9");
}
