// aplayouts.js - the Layouts card of the dashboard: the layouts the AP has for each tag model,
// drawn as the AP draws them, with how many tags show each one, and the ready-made layouts of
// the template library that can be sent to the AP with one press. Picking a layout shows what
// a client has to send for it (its fields, in order), with a form to send it to a tag from here
// and examples to copy (js/layoutapi.js). The card folds away; its head says what it holds.
//
// The same on the web page (over USB or the bridge) and on the AP's own page, where the
// renderer and the templates come from the published web page (firmwareBase).

import { MODELS, layoutModel, layoutUsers, modelName, modelLabel } from "./models.js";
import { tagLayouts, countByLayout } from "./layoutinfo.js";
import { variantFor, addTemplate, PAGES } from "./catalog.js";
import { layoutPanel } from "./layoutapi.js";
import { apFonts, checkFontFile } from "./apfonts.js";
import { makeFont, loadFontFile, previewFont, hasFont, MAX_FILE } from "./fontmaker.js";

const $ = (id) => document.getElementById(id);
const MAX_LAYOUTS = 16;
const BUILTIN = {
  price: { id: 1, fields: ["name", "price", "subtitle"], sample: ["Semi-skimmed milk 1 L", "24,90", "kr 24,90/L"],
           what: ["Product name", "Price, e.g. 24,90", "Small text under it"] },
  text: { id: 2, fields: ["line1", "line2", "line3", "line4"], sample: ["Welcome", "Open 9-21", "", ""],
          what: ["Line 1", "Line 2", "Line 3", "Line 4"] },
};
const MISSING = "the layout is not in the layout file for this tag's model";

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

// The layout's sample text, or its field names for one without
const sampleOf = (l) => (l._sample?.length ? l._sample : (l.fields || []).map((f) => f.charAt(0).toUpperCase() + f.slice(1).replace(/_/g, " ")));

// The icons a file's layouts still use, the rest left out
function pruneIcons(file) {
  if (!file.icons) return file;
  const used = new Set(file.layouts.flatMap((l) => (l.elements || []).filter((e) => e.type === "icon").map((e) => e.icon)));
  for (const n of Object.keys(file.icons)) if (!used.has(n)) delete file.icons[n];
  if (!Object.keys(file.icons).length) delete file.icons;
  return file;
}

export function initLayouts({ client, refresh, firmwareBase = "", sameOrigin = false }) {
  let last = null;
  let model = "";
  const files = {};        // by model: { version, file } of the AP's layout file (file null: none)
  const shown = {};        // by MAC: { cid, layout }, the layout each tag's content uses
  let catalog;             // { index, base } of the template library, null when it cannot be read
  const tplFiles = {};     // the template files by id
  const pics = new Map();  // drawn pictures by key
  let sel = null;          // { kind: "ap" | "builtin" | "tpl", name, id }
  let busy = false;
  let warned = false;      // the missing-layout warning was shown (the card is opened once for it)
  let drawnKey = "";
  const fonts = apFonts();
  let fontState = { supported: false, info: null, files: {}, slots: {} };   // the AP's font files (js/apfonts.js)
  const host = sameOrigin ? location.host : "openesl.local";
  const canEdit = !!$("tab-design");

  const say = (text, cls = "") => {
    $("lay-msg").textContent = text;
    $("lay-msg").className = "hint" + (cls ? " " + cls : "");
  };

  // The renderer (from the published page on the AP's own page), or null without one
  let rendererMod;
  async function renderer(m) {
    try {
      rendererMod ||= await import(firmwareBase ? new URL("js/renderer.js", firmwareBase).href : "./renderer.js");
      const r = await rendererMod.loadRenderer(m, firmwareBase ? new URL("wasm/", firmwareBase) : undefined);
      return r;
    } catch {
      return null;
    }
  }

  // Draws `layout` (an id: built-in or 0x10 + n) of `file` (or none) into a canvas
  function paint(r, file, layout, fields, flags = 0) {
    if (file) {
      // The renderer is shared with the designer: check with this AP's limits each time
      r.setLimits?.(last?.layouts?.limits || null);
      r.setFonts?.(fontState.files, fontState.slots);   // and with its font files, which the file may name
      const res = r.loadLayouts(JSON.stringify(file));
      if (!res.ok) return null;
    }
    const out = r.render(layout, flags, fields);
    if (!out.ok) return null;
    const c = el("canvas", "lay-pic");
    [c.width, c.height] = [r.width, r.height];
    const ctx = c.getContext("2d");
    const img = ctx.createImageData(r.width, r.height);
    for (let i = 0; i < r.width * r.height; i++) {
      const v = out.pixels[i];
      img.data.set(v === 2 ? [200, 30, 30, 255] : v ? [17, 17, 17, 255] : [255, 255, 255, 255], i * 4);
    }
    ctx.putImageData(img, 0, 0);
    return c;
  }

  async function loadCatalog() {
    if (catalog !== undefined) return catalog;
    const bases = sameOrigin ? [] : [new URL("../templates/", import.meta.url).href];
    bases.push(PAGES + "templates/");
    for (const base of bases) {
      try {
        const r = await fetch(base + "index.json", { cache: "no-cache" });
        const index = r.ok ? await r.json() : null;
        if (index?.format === "esl-template-index" && Array.isArray(index.templates)) return (catalog = { index, base });
      } catch { /* the next one */ }
    }
    return (catalog = null);
  }

  async function templateFile(entry) {
    if (!tplFiles[entry.id]) {
      const r = await fetch(catalog.base + entry.file, { cache: "no-cache" });
      if (!r.ok) throw new Error(`${entry.file}: ${r.status}`);
      tplFiles[entry.id] = await r.json();
    }
    return tplFiles[entry.id];
  }

  // The AP's file of a model, read again when its version changed
  async function apFile(m, version) {
    if (files[m] && files[m].version === version) return files[m].file;
    let file = null;
    if (version) {
      try {
        file = await client().call("GET", "/api/layouts/" + encodeURIComponent(m));
      } catch (e) {
        if (!/^404\b/.test(e.message)) throw e;
      }
    }
    files[m] = { version, file };
    return file;
  }

  // The layout each tag of the model shows, asked again for a tag whose content changed
  async function readShown(tags) {
    const known = {};
    for (const t of tags) if (shown[t.mac] && shown[t.mac].cid === t.content_id) known[t.mac] = shown[t.mac].layout;
    const next = await tagLayouts(client(), tags.filter((t) => t.content_id), known);
    for (const t of tags) {
      if (!t.content_id) shown[t.mac] = { cid: 0, layout: "" };
      else if (t.mac in next) shown[t.mac] = { cid: t.content_id, layout: next[t.mac] };
    }
  }

  const layoutsOf = () => Object.fromEntries(Object.entries(shown).map(([mac, s]) => [mac, s.layout]));

  // Tags with content whose layout is not known (their answer did not come): while there are
  // any, a layout's count is a lower bound and no layout can be called unused (#302)
  const unchecked = (tags) => tags.filter((t) => t.content_id && shown[t.mac]?.cid !== t.content_id).length;

  // The models to pick from, those with tags first: one per layout file, so a model whose
  // sameAs names another (same screen, #181) is counted with that one
  function models() {
    const n = {};
    for (const t of last.list.tags) if (t.model) n[layoutModel(t.model)] = (n[layoutModel(t.model)] || 0) + 1;
    const all = Object.keys(MODELS).filter((m) => layoutModel(m) === m);
    return all.map((m) => [m, n[m] || 0]).sort((a, b) => b[1] - a[1]);
  }

  // Called with what the dashboard read at each refresh; one at a time
  let chain = Promise.resolve();
  function render(data) {
    last = data;
    chain = chain.then(update, update);
    return chain;
  }

  async function update() {
    if (!last?.list || !$("lay-card")) return;
    $("lay-card").hidden = false;
    const ms = models();
    if (!model || !ms.some(([m]) => m === model)) model = ms[0][0];
    $("lay-models").replaceChildren(...ms.map(([m, n]) => {
      const b = el("button", m === model ? "on" : "", `${modelLabel(m)} · ${n} tag${n === 1 ? "" : "s"}`);
      if (layoutUsers(m).length > 1) b.title = `One layout file for ${layoutUsers(m).map(modelName).join(", ")}`;
      b.type = "button";
      b.setAttribute("aria-pressed", String(m === model));
      b.onclick = () => {
        if (m === model) return;
        model = m;
        sel = null;
        drawnKey = "";
        render(last);
      };
      return b;
    }));
    const info = (last.layouts?.layouts || []).find((l) => l.model === model);
    const max = last.layouts?.max_size || 8192;
    $("lay-space").textContent = info?.version ? `Space on the AP for ${modelName(model)}: ${(info.size / 1024).toFixed(1)} of ${(max / 1024).toFixed(0)} KB` : `No ${modelName(model)} layout file on the AP yet`;
    $("lay-meter").hidden = !info?.version;
    $("lay-meter").firstElementChild.style.width = info?.version ? Math.min(100, (100 * info.size) / max).toFixed(0) + "%" : "0";
    const tags = last.list.tags.filter((t) => layoutModel(t.model) === model);
    let file;
    try {
      file = await apFile(model, info?.version || null);
      await readShown(tags);
    } catch (e) {
      say("Could not read the AP's layouts: " + e.message, "error");
      return;
    }
    try {
      fontState = await fonts.read(client());
    } catch (e) {
      say("Could not read the AP's font files: " + e.message, "error");
    }
    drawFonts();
    const cat = await loadCatalog();
    const key = JSON.stringify([model, files[model]?.version, (fontState.info?.fonts || []).map((f) => f.version), tags.map((t) => [t.mac, shown[t.mac]?.layout, shown[t.mac]?.cid, t.content_error]), !!cat, sel, busy]);
    if (key === drawnKey) return;
    drawnKey = key;
    await draw(file, tags, cat);
  }

  async function draw(file, tags, cat) {
    const m = model;
    const r = await renderer(m);
    const counts = countByLayout(tags, layoutsOf(), m);
    const unk = unchecked(tags);
    const onAp = (file?.layouts || []).map((l) => l.name);

    // Tags that show a layout the AP does not have
    const missing = {};
    for (const t of tags) if (t.content_error === MISSING) {
      const n = shown[t.mac]?.layout || "?";
      (missing[n] ||= []).push(t.mac);
    }
    const warn = $("lay-warn");
    warn.replaceChildren();
    for (const [name, macs] of Object.entries(missing)) {
      const p = el("p");
      p.append(`⚠ ${macs.length} tag${macs.length === 1 ? " uses" : "s use"} `, el("b", "", name),
        ", but it is not on the AP. " + (macs.length === 1 ? "It keeps its" : "They keep their") + " old picture.");
      const entry = cat?.index.templates.find((t) => t.layout === name && t.models?.[m]);
      if (entry) {
        const b = el("button", "small primary", "Send it");
        b.type = "button";
        b.disabled = busy;
        b.onclick = () => sendTemplate(entry);
        p.append(" ", b);
      }
      warn.append(p);
    }
    warn.hidden = !warn.childElementCount;
    // Tags without their layout: show the warning even when the card was folded away
    if (!warn.hidden && !warned) $("lay-card").open = true;
    warned = !warn.hidden;

    // On the AP: the built-in layouts, then the file's
    const cards = [];
    for (const [name, b] of Object.entries(BUILTIN)) {
      const c = counts[name] || 0;
      cards.push(card({
        key: "builtin:" + name, name, pic: r && paint(r, null, b.id, b.sample),
        sub: [el("span", "", "Built in" + (c ? ` · ${c} tag${c === 1 ? "" : "s"}` : ""))],
        sel: { kind: "builtin", name },
      }));
    }
    (file?.layouts || []).forEach((l, i) => {
      const c = counts[l.name] || 0;
      const buttons = [];
      if (canEdit) buttons.push(button("Edit", () => edit(l.name), "Open it in the designer"));
      if (!c) {
        const rm = button("Remove", () => remove(l.name), file.layouts.length <= 1 ? "The AP needs at least one layout in the file"
          : unk ? `${unk} tag${unk === 1 ? "" : "s"} could not be checked, so this layout may be in use. Try again in a moment.`
          : "Take it out of the AP's file");
        rm.disabled ||= file.layouts.length <= 1 || unk > 0;
        buttons.push(rm);
      }
      const uses = unk ? (c ? `at least ${c} tag${c === 1 ? "" : "s"}` : "Not checked") : c ? `${c} tag${c === 1 ? "" : "s"}` : "Not used";
      cards.push(card({
        key: "ap:" + l.name, name: l.name, pic: r && paint(r, file, 0x10 + i, sampleOf(l)),
        sub: [el("span", "pill " + (c ? "ok" : "off"), uses),
              el("span", "", ` ${(l.fields || []).length} field${(l.fields || []).length === 1 ? "" : "s"}`)],
        buttons, sel: { kind: "ap", name: l.name },
      }));
    });
    $("lay-on").replaceChildren(...cards);

    // Can be sent: the library's templates for this model whose layout the AP does not have
    const can = [];
    if (cat) {
      const titles = Object.fromEntries((cat.index.categories || []).map((c) => [c.id, c.title]));
      const entries = cat.index.templates.filter((t) => t.models?.[m] && !onAp.includes(t.layout));
      for (const entry of entries) {
        let pic = null;
        try {
          const t = await templateFile(entry);
          const v = variantFor(t, m);
          if (r && v) pic = paint(r, { format: "esl-layouts", version: 1, model: m, icons: v.icons, layouts: [v.layout] }, 0x10, sampleOf(v.layout));
        } catch { /* drawn without a picture */ }
        const send = button("Send to AP", () => sendTemplate(entry), "Add it to the AP's layout file");
        send.classList.add("primary");
        send.disabled ||= onAp.length >= MAX_LAYOUTS;
        if (onAp.length >= MAX_LAYOUTS) send.title = `The file has ${MAX_LAYOUTS} layouts, the most it can hold. Remove one first.`;
        can.push(card({
          key: "tpl:" + entry.id, name: entry.layout, pic,
          sub: [el("span", "", `${titles[entry.category] || entry.category} · ${entry.title}`)],
          buttons: [send], sel: { kind: "tpl", name: entry.layout, id: entry.id },
        }));
      }
    }
    $("lay-can").replaceChildren(...can);
    $("lay-can-none").hidden = !!can.length;
    $("lay-can-none").textContent = cat ? "Every layout of the template library is on the AP already."
      : "The template library is on the web page; this browser could not reach it.";
    const nMissing = Object.values(missing).flat().length;
    $("lay-sum").textContent = `${modelName(m)}: ${onAp.length + Object.keys(BUILTIN).length} on the AP` +
      (can.length ? ` · ${can.length} more can be sent` : "") +
      (nMissing ? ` · ⚠ ${nMissing} tag${nMissing === 1 ? " uses a layout" : "s use layouts"} the AP lacks` : "");
    if (!r && !$("lay-msg").textContent) say("No pictures: the drawing code could not be loaded (it comes from the web page).", "");
    await drawPanel(file, tags, cat);
  }

  function button(text, fn, title = "") {
    const b = el("button", "small", text);
    b.type = "button";
    b.title = title;
    b.disabled = busy;
    b.onclick = (ev) => {
      ev.stopPropagation();
      fn();
    };
    return b;
  }

  function card({ key, name, pic, sub, buttons = [], sel: s }) {
    const d = el("div", "lay-card" + (sel && sel.kind === s.kind && sel.name === s.name ? " sel" : ""));
    d.dataset.key = key;
    d.tabIndex = 0;
    d.setAttribute("role", "button");
    d.setAttribute("aria-label", `${name}: show its fields`);
    const box = el("div", "lay-thumb");
    if (pic) box.append(pic);
    d.append(box, el("div", "lay-n", name), Object.assign(el("div", "lay-u"), {}));
    d.lastChild.append(...sub);
    if (buttons.length) {
      const row = el("div", "lay-b");
      row.append(...buttons);
      d.append(row);
    }
    const pick = () => {
      sel = s;
      // On a phone the panel is below the cards: take the reader there
      render(last).then(() => matchMedia("(max-width: 900px)").matches &&
        $("lay-panel").scrollIntoView({ behavior: matchMedia("(prefers-reduced-motion: reduce)").matches ? "auto" : "smooth", block: "start" }));
    };
    d.onclick = pick;
    d.onkeydown = (ev) => {
      if (ev.target === d && (ev.key === "Enter" || ev.key === " ")) {
        ev.preventDefault();
        pick();
      }
    };
    return d;
  }

  // The panel of the picked layout: what to send for it, and the tags showing it
  async function drawPanel(file, tags, cat) {
    const p = $("lay-panel");
    if (!sel) return (p.hidden = true);
    let fields, sample, what = [], desc = "", state, entry;
    if (sel.kind === "builtin") {
      const b = BUILTIN[sel.name];
      [fields, sample, what, state] = [b.fields, b.sample, b.what, "Built in"];
      desc = sel.name === "price" ? "A product name, a large price and a small text under it." : "Up to four lines of text.";
    } else if (sel.kind === "ap") {
      const l = file?.layouts?.find((x) => x.name === sel.name);
      if (!l) return (p.hidden = true);
      [fields, sample, desc, state] = [l.fields || [], sampleOf(l), l.description || "", "On the AP"];
      const e = cat?.index.templates.find((t) => t.id === l._template || t.layout === l.name);
      if (e) try { const t = await templateFile(e); what = fields.map((f) => t.fields?.[f] || ""); } catch { /* none */ }
    } else {
      entry = cat?.index.templates.find((t) => t.id === sel.id);
      if (!entry) return (p.hidden = true);
      const t = await templateFile(entry);
      const v = variantFor(t, model);
      [fields, sample, desc, state] = [v.layout.fields || [], sampleOf(v.layout), t.description || "", "Not on the AP"];
      what = fields.map((f) => t.fields?.[f] || "");
    }
    p.hidden = false;
    const onAp = sel.kind !== "tpl";
    panel.show({
      key: model + "/" + sel.name, name: sel.name, state, ok: onAp, desc, fields, sample, what, host,
      tags: tags.map((t) => ({ mac: t.mac, shows: shown[t.mac]?.layout === sel.name, shown: t.content_id ? shown[t.mac]?.layout || "?" : "" })),
      send: onAp ? async (mac, c) => {
        const r = await client().call("PUT", "/api/tags/" + encodeURIComponent(mac), c);
        refresh();
        return r;
      } : null,
      why: "It is not on the AP yet. Send it there first; then it can go to a tag.",
      first: entry && !busy ? { label: "Send to AP", run: () => sendTemplate(entry) } : null,
    });
  }

  async function run(what, fn) {
    if (busy) return;
    busy = true;
    drawnKey = "";
    say(what + "…");
    try {
      say(...(await fn()));
    } catch (e) {
      say(e.message, "error");
    } finally {
      busy = false;
      drawnKey = "";
      files[model] = undefined;
      await refresh();
    }
  }

  // The current file of the model from the AP (null: none), read fresh before a change
  async function fresh(m) {
    try {
      return await client().call("GET", "/api/layouts/" + encodeURIComponent(m));
    } catch (e) {
      if (/^404\b/.test(e.message)) return null;
      throw e;
    }
  }

  function unfit(r) {
    const n = r?.content_errors_total || 0;
    return n ? ` ${n === 1 ? "1 tag has" : n + " tags have"} content that does not fit it and keep${n === 1 ? "s" : ""} the old picture.` : "";
  }

  function sendTemplate(entry) {
    const m = model;
    return run(`Sending "${entry.layout}" to the AP`, async () => {
      const t = await templateFile(entry);
      const cur = await fresh(m);
      const added = addTemplate(cur || { format: "esl-layouts", version: 1, model: m, layouts: [] }, t, m);
      if (!added) throw new Error(`"${entry.title}" has no layout for the ${modelName(m)}.`);
      if (added.doc.layouts.length > MAX_LAYOUTS) throw new Error(`The AP's file has ${MAX_LAYOUTS} layouts already. Remove one first.`);
      const r = await client().call("PUT", "/api/layouts/" + encodeURIComponent(m), added.doc);
      sel = { kind: "ap", name: added.name };
      return [`"${added.name}" is on the AP. Send content with it to a tag (see the examples).` + unfit(r), unfit(r) ? "error" : "ok"];
    });
  }

  function remove(name) {
    const m = model;
    if (!confirm(`Take the layout "${name}" out of the AP's ${modelName(m)} layout file?`)) return;
    return run(`Removing "${name}"`, async () => {
      const cur = await fresh(m);
      if (!cur?.layouts?.some((l) => l.name === name)) return [`The AP has no "${name}" any more.`, ""];
      // Ask the tags again right before: a tag may have got content with it since the cards
      // were drawn, and one that does not answer may use it (#302)
      const tags = last.list.tags.filter((t) => layoutModel(t.model) === m);
      await readShown(tags);
      const unk = unchecked(tags);
      if (unk) throw new Error(`${unk} tag${unk === 1 ? "" : "s"} could not be checked, so "${name}" may be in use. Try again in a moment.`);
      const n = countByLayout(tags, layoutsOf(), m)[name] || 0;
      if (n) throw new Error(`${n} tag${n === 1 ? " shows" : "s show"} "${name}". Give ${n === 1 ? "it" : "them"} other content first.`);
      const next = structuredClone(cur);
      next.layouts = next.layouts.filter((l) => l.name !== name);
      if (!next.layouts.length) throw new Error("The AP needs at least one layout in the file.");
      await client().call("PUT", "/api/layouts/" + encodeURIComponent(m), pruneIcons(next));
      if (sel?.name === name) sel = null;
      return [`Removed "${name}" from the AP.`, "ok"];
    });
  }

  // The font files on the AP (#177): which layout files draw with each, and a way to add one
  function drawFonts() {
    const box = $("lay-fonts");
    if (!box) return;
    box.hidden = !fontState.supported;
    if (!fontState.supported) return;
    const i = fontState.info;
    $("lay-fonts-space").textContent = `${(i.fonts || []).length} of ${i.max_fonts} · ${(i.bytes / 1024).toFixed(1)} of ${(i.max_bytes / 1024).toFixed(0)} KB`;
    const rows = (i.fonts || []).map((f) => {
      const li = el("li");
      li.append(el("b", "", f.name), " ", el("span", "muted small",
        `${f.height} px · ${f.characters} characters` + (f.source ? " · " + f.source : "") +
        (f.used_by?.length ? ` · used by the ${f.used_by.map(modelName).join(", ")} layouts` : " · not used yet")));
      const del = button("Delete", () => removeFont(f.name),
        f.used_by?.length ? "A layout file draws with it: change those layouts first" : "Delete the font file from the AP");
      del.disabled = busy || !!f.used_by?.length;
      li.append(" ", del);
      return li;
    });
    if (!rows.length) rows.push(el("li", "muted", "None yet: the AP draws with its built-in fonts only."));
    $("lay-font-list").replaceChildren(...rows);
    $("lay-font-add").disabled = busy;
    $("lay-font-make").disabled = busy;
    if ($("fm-send")) $("fm-send").disabled = busy || !made;
  }

  function addFont(text) {
    return run("Sending the font file", async () => {
      let file = text;
      if (typeof text === "string") {
        try {
          file = JSON.parse(text);
        } catch {
          throw new Error("The file is not JSON: pick a font file made here (Make a font…) or by tag/tools/gen_font.py --file.");
        }
      }
      const bad = checkFontFile(file);
      if (bad) throw new Error("The file is " + bad + ".");
      const r = await fonts.put(client(), file);
      const again = r.redrawn?.length ? ` The ${r.redrawn.map(modelName).join(", ")} layouts that use it are drawn again with it.` : "";
      return [`"${file.name}" is on the AP${r.replaced ? " in place of the old one" : ""}: layouts can name it in their fonts.` +
        again + unfit(r), unfit(r) ? "error" : "ok"];
    });
  }

  function removeFont(name) {
    if (!confirm(`Delete the font file "${name}" from the AP?`)) return;
    return run(`Deleting "${name}"`, async () => {
      await fonts.remove(client(), name);
      return [`Deleted the font file "${name}".`, "ok"];
    });
  }

  if ($("lay-font-add")) {
    $("lay-font-add").onclick = () => $("lay-font-file").click();
    $("lay-font-file").onchange = async (ev) => {
      const f = ev.target.files?.[0];
      ev.target.value = "";
      if (f) await addFont(await f.text());
    };
  }

  // Make a font (js/fontmaker.js): the browser draws a font on this computer, or a font file,
  // into a font file, with a preview of what the tag shows, and sends it to the AP
  let made = null;          // the font file made from the form, null when the form has an error
  let picked = null;        // { family, file } of a font file picked in the form
  let makeTimer = 0;
  const fm = (id) => $("fm-" + id);
  function makeNow() {
    made = null;
    const info = fm("info");
    try {
      const sel = fm("family").value;
      fm("other-l").hidden = sel !== "other";
      fm("file-l").hidden = sel !== "file";
      fm("custom-l").hidden = fm("chars").value !== "custom";
      let family = sel, source = "";
      if (sel === "file") {
        if (!picked) throw new Error("Pick a font file.");
        family = picked.family;
        source = picked.file;
      } else if (sel === "other") {
        family = fm("other").value.trim();
        if (!family) throw new Error("Write the name of a font on this computer.");
        if (!hasFont(family)) throw new Error(`This computer has no font called "${family}" (or the browser may not use it).`);
        source = family;
      } else source = "browser " + sel;
      const name = fm("name").value.trim();
      const bad = checkFontFile({ format: "esl-font", name });
      if (bad) throw new Error("The name: " + bad.replace(/^its name /, "") + ".");
      const weight = fm("weight").value;
      const file = makeFont({
        name, family, weight, height: +fm("height").value, size: +fm("size").value || 0,
        chars: fm("chars").value === "custom" ? fm("custom").value : fm("chars").value,
      });
      file.source = `${source}, ${weight}, ${file._made.size} px`.replace(/[^\x20-\x7e]/g, "?").slice(0, 63);
      const p = previewFont(fm("canvas"), file, fm("sample").value || "Aa", file.height > 36 ? 1 : 2);
      if (!fm("size").value) fm("size").placeholder = `auto (${file._made.size})`;
      const n = file.widths.filter((w) => w).length;
      const kb = (p.blob / 1024).toFixed(1);
      const list = (a) => a.slice(0, 12).join(" ") + (a.length > 12 ? " …" : "");
      const { clipped, accents } = file._made;
      const clip = (clipped.length ? ` The tops or bottoms of ${list(clipped)} are cut off: make it taller or the letters smaller.` : "") +
        (accents.length && !clipped.length ? ` Tall accents (${list(accents)}) reach a little out of it at the top, as in the built-in fonts.` : "");
      const room = fontState.info ? fontState.info.max_bytes - fontState.info.bytes + ((fontState.info.fonts || []).find((f) => f.name === name)?.bytes || 0) : Infinity;
      const full = p.json > MAX_FILE ? ` The file is ${(p.json / 1024).toFixed(1)} KB, more than the ${MAX_FILE / 1024} KB the AP takes in one font file: fewer characters or a smaller height.`
        : p.blob > room ? ` That is more than the ${(room / 1024).toFixed(1)} KB the AP has left: fewer characters or a smaller height.` : "";
      info.textContent = `"${name}": ${file.height} px high, ${n} characters, ${kb} KB.` + clip + full;
      info.className = "hint" + (full ? " error" : "");
      made = full ? null : file;
    } catch (e) {
      info.textContent = e.message;
      info.className = "hint error";
      const c = fm("canvas");
      c.width = c.height = 0;
    }
    fm("send").disabled = busy || !made;
    fm("save").disabled = !made;
  }
  const makeSoon = () => {
    clearTimeout(makeTimer);
    makeTimer = setTimeout(makeNow, 150);
  };
  if ($("lay-fm")) {
    $("lay-font-make").onclick = () => {
      $("lay-fm").hidden = !$("lay-fm").hidden;
      if (!$("lay-fm").hidden) {
        makeNow();
        fm("name").focus();
      }
    };
    for (const id of ["name", "other", "height", "size", "custom", "sample"]) fm(id).oninput = makeSoon;
    for (const id of ["weight", "chars"]) fm(id).onchange = makeNow;
    fm("family").onchange = () => {
      if (fm("family").value === "file" && !picked) fm("file").click();
      makeNow();
    };
    fm("file").onchange = async (ev) => {
      const f = ev.target.files?.[0];
      ev.target.value = "";
      if (!f) return;
      try {
        picked = { family: await loadFontFile(f), file: f.name };
        fm("family").value = "file";
        fm("pick").textContent = f.name;
      } catch (e) {
        picked = null;
        say(e.message, "error");
      }
      makeNow();
    };
    fm("pick").onclick = () => fm("file").click();
    fm("send").onclick = async () => {
      makeNow();
      if (made) await addFont(made);
    };
    fm("save").onclick = () => {
      makeNow();
      if (!made) return;
      const a = document.createElement("a");
      a.href = URL.createObjectURL(new Blob([JSON.stringify(made)], { type: "application/json" }));
      a.download = made.name + ".json";
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    };
    fm("close").onclick = () => ($("lay-fm").hidden = true);
  }

  // The designer of this page opens the AP's file with this layout picked (js/editor.js)
  function edit(name) {
    const file = files[model]?.file;
    if (!file) return;
    window.dispatchEvent(new CustomEvent("openesl-edit-layout", { detail: { model, file: structuredClone(file), name } }));
  }

  function reset() {
    last = null;
    sel = null;
    drawnKey = "";
    for (const k of Object.keys(files)) delete files[k];
    for (const k of Object.keys(shown)) delete shown[k];
    if ($("lay-card")) $("lay-card").hidden = true;
    fontState = { supported: false, info: null, files: {}, slots: {} };
    if ($("lay-fonts")) $("lay-fonts").hidden = true;
    say("");
  }

  const panel = $("lay-panel") && layoutPanel($("lay-panel"), {
    onClose: () => {
      sel = null;
      drawnKey = "";
      render(last);
    },
  });

  return { render, reset };
}
