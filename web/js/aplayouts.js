// aplayouts.js - the Layouts card of the dashboard: the layouts the AP has for each tag model,
// drawn as the AP draws them, with how many tags show each one, and the ready-made layouts of
// the template library that can be sent to the AP with one press. Picking a layout shows what
// a client has to send for it (its fields, in order) with examples to copy.
//
// The same on the web page (over USB or the bridge) and on the AP's own page, where the
// renderer and the templates come from the published web page (firmwareBase).

import { MODELS } from "./models.js";
import { tagLayouts, countByLayout } from "./layoutinfo.js";
import { variantFor, addTemplate, PAGES } from "./catalog.js";

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

// What a client sends for a layout, in the forms the panel offers: { curl, json, csv, ha }
export function examples({ layout, fields, sample, mac, host }) {
  const body = { layout, fields: sample.slice(0, fields.length) };
  const json = JSON.stringify(body, null, 2);
  const target = mac || "<MAC>";
  const q = (s) => `"${String(s).replace(/"/g, '""')}"`;
  return {
    curl: `curl -X PUT http://${host}/api/tags/${target} \\\n  -H "Authorization: Bearer $TOKEN" \\\n` +
          `  -H "Content-Type: application/json" \\\n  -d '${JSON.stringify(body, null, 2).replace(/\n/g, "\n     ").replace(/'/g, "'\\''")}'`,
    json: `PUT /api/tags/${target}\n\n${json}`,
    csv: `# tags.csv\nmac,layout,${fields.join(",")}\n${target},${layout},${body.fields.map(q).join(",")}\n\n` +
         `python3 tools/esl-client/esl_client.py --url http://${host} --token $TOKEN sync tags.csv`,
    ha: `# rest_command from docs/home-assistant.md\naction: rest_command.esl_tag_text\ndata:\n` +
        `  mac: "${target}"\n  layout: ${layout}\n  fields: ${JSON.stringify(body.fields)}`,
  };
}

const TABS = [["curl", "curl"], ["json", "JSON"], ["csv", "esl-client"], ["ha", "Home Assistant"]];

export function initLayouts({ client, refresh, firmwareBase = "", sameOrigin = false }) {
  let last = null;
  let model = "";
  const files = {};        // by model: { version, file } of the AP's layout file (file null: none)
  const shown = {};        // by MAC: { cid, layout }, the layout each tag's content uses
  let catalog;             // { index, base } of the template library, null when it cannot be read
  const tplFiles = {};     // the template files by id
  const pics = new Map();  // drawn pictures by key
  let sel = null;          // { kind: "ap" | "builtin" | "tpl", name, id }
  let tab = "curl";
  let busy = false;
  let drawnKey = "";
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
      if (last?.layouts?.limits) r.setLimits?.(last.layouts.limits);
      return r;
    } catch {
      return null;
    }
  }

  // Draws `layout` (an id: built-in or 0x10 + n) of `file` (or none) into a canvas
  function paint(r, file, layout, fields, flags = 0) {
    if (file) {
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

  // The models to pick from: those with tags first
  function models() {
    const n = {};
    for (const t of last.list.tags) if (t.model) n[t.model] = (n[t.model] || 0) + 1;
    const all = Object.keys(MODELS);
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
    if (!model || !MODELS[model]) model = ms[0][0];
    $("lay-models").replaceChildren(...ms.map(([m, n]) => {
      const b = el("button", m === model ? "on" : "", `${m}" · ${n} tag${n === 1 ? "" : "s"}`);
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
    $("lay-space").textContent = info?.version ? `Space on the AP for ${model}": ${(info.size / 1024).toFixed(1)} of ${(max / 1024).toFixed(0)} KB` : `No ${model}" layout file on the AP yet`;
    $("lay-meter").hidden = !info?.version;
    $("lay-meter").firstElementChild.style.width = info?.version ? Math.min(100, (100 * info.size) / max).toFixed(0) + "%" : "0";
    const tags = last.list.tags.filter((t) => t.model === model);
    let file;
    try {
      file = await apFile(model, info?.version || null);
      await readShown(tags);
    } catch (e) {
      say("Could not read the AP's layouts: " + e.message, "error");
      return;
    }
    const cat = await loadCatalog();
    const key = JSON.stringify([model, files[model]?.version, tags.map((t) => [t.mac, shown[t.mac]?.layout, t.content_error]), !!cat, sel, tab, busy]);
    if (key === drawnKey) return;
    drawnKey = key;
    await draw(file, tags, cat);
  }

  async function draw(file, tags, cat) {
    const m = model;
    const r = await renderer(m);
    const counts = countByLayout(tags, layoutsOf(), m);
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
        const rm = button("Remove", () => remove(l.name), file.layouts.length <= 1 ? "The AP needs at least one layout in the file" : "Take it out of the AP's file");
        rm.disabled ||= file.layouts.length <= 1;
        buttons.push(rm);
      }
      cards.push(card({
        key: "ap:" + l.name, name: l.name, pic: r && paint(r, file, 0x10 + i, sampleOf(l)),
        sub: [el("span", "pill " + (c ? "ok" : "off"), c ? `${c} tag${c === 1 ? "" : "s"}` : "Not used"),
              el("span", "", ` ${(l.fields || []).length} fields`)],
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
    if (!r) say("No pictures: the drawing code could not be loaded (it comes from the web page).", "");
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
      render(last);
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
    let fields, sample, what = [], desc = "", state;
    if (sel.kind === "builtin") {
      const b = BUILTIN[sel.name];
      [fields, sample, what, state] = [b.fields, b.sample, b.what, "Built in"];
      desc = sel.name === "price" ? "A product name, a large price and a small text under it." : "Up to four lines of text.";
    } else if (sel.kind === "ap") {
      const l = file?.layouts?.find((x) => x.name === sel.name);
      if (!l) return (p.hidden = true);
      [fields, sample, desc, state] = [l.fields || [], sampleOf(l), l.description || "", "On the AP"];
      const entry = cat?.index.templates.find((t) => t.id === l._template || t.layout === l.name);
      if (entry) try { const t = await templateFile(entry); what = fields.map((f) => t.fields?.[f] || ""); } catch { /* none */ }
    } else {
      const entry = cat?.index.templates.find((t) => t.id === sel.id);
      if (!entry) return (p.hidden = true);
      const t = await templateFile(entry);
      const v = variantFor(t, model);
      [fields, sample, desc, state] = [v.layout.fields || [], sampleOf(v.layout), t.description || "", "Not on the AP"];
      what = fields.map((f) => t.fields?.[f] || "");
    }
    const users = tags.filter((t) => shown[t.mac]?.layout === sel.name);
    p.hidden = false;
    $("lay-p-name").textContent = sel.name;
    $("lay-p-state").textContent = state;
    $("lay-p-state").className = "pill " + (state === "Not on the AP" ? "off" : "ok");
    $("lay-p-desc").textContent = desc;
    $("lay-p-desc").hidden = !desc;
    $("lay-p-fields").replaceChildren(...fields.map((f, i) => {
      const tr = el("tr");
      const code = el("code", "", f);
      const td = el("td");
      td.append(code);
      tr.append(el("td", "muted", String(i)), td, el("td", "", what[i] || (sample[i] ? `e.g. ${sample[i]}` : "")));
      return tr;
    }));
    const ex = examples({ layout: sel.name, fields, sample, mac: users[0]?.mac, host });
    $("lay-p-tabs").replaceChildren(...TABS.map(([k, label]) => {
      const b = el("button", k === tab ? "on" : "", label);
      b.type = "button";
      b.setAttribute("aria-pressed", String(k === tab));
      b.onclick = () => {
        tab = k;
        drawnKey = "";
        render(last);
      };
      return b;
    }));
    $("lay-p-code").textContent = ex[tab];
    $("lay-p-users-h").textContent = `Tags using it (${users.length})`;
    $("lay-p-users").textContent = users.length ? users.map((t) => "…" + t.mac.slice(-6)).join(" · ")
      : sel.kind === "tpl" ? "None: send it to the AP first, then to a tag." : "None yet.";
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
      if (!added) throw new Error(`"${entry.title}" has no layout for the ${m}".`);
      if (added.doc.layouts.length > MAX_LAYOUTS) throw new Error(`The AP's file has ${MAX_LAYOUTS} layouts already. Remove one first.`);
      const r = await client().call("PUT", "/api/layouts/" + encodeURIComponent(m), added.doc);
      sel = { kind: "ap", name: added.name };
      return [`"${added.name}" is on the AP. Send content with it to a tag (see the examples).` + unfit(r), unfit(r) ? "error" : "ok"];
    });
  }

  function remove(name) {
    const m = model;
    if (!confirm(`Take the layout "${name}" out of the AP's ${m}" layout file?`)) return;
    return run(`Removing "${name}"`, async () => {
      const cur = await fresh(m);
      if (!cur?.layouts?.some((l) => l.name === name)) return [`The AP has no "${name}" any more.`, ""];
      const next = structuredClone(cur);
      next.layouts = next.layouts.filter((l) => l.name !== name);
      if (!next.layouts.length) throw new Error("The AP needs at least one layout in the file.");
      await client().call("PUT", "/api/layouts/" + encodeURIComponent(m), pruneIcons(next));
      if (sel?.name === name) sel = null;
      return [`Removed "${name}" from the AP.`, "ok"];
    });
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
    say("");
  }

  $("lay-p-close")?.addEventListener("click", () => {
    sel = null;
    drawnKey = "";
    render(last);
  });
  $("lay-p-copy")?.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText($("lay-p-code").textContent);
      $("lay-p-copy").textContent = "Copied";
    } catch {
      $("lay-p-copy").textContent = "Select and copy";
    }
    setTimeout(() => ($("lay-p-copy").textContent = "Copy"), 1500);
  });

  return { render, reset };
}
