// catalog.js - the template catalog: ready-made layouts in templates/ of the repository, one file
// per template with a layout for each tag model it suits (wiki/labels/Label-templates.md).
// web/templates.mjs checks them and publishes them next to the page as templates/, with an
// index.json the designer's template picker (tplpicker.js) lists them from. Only plain functions
// here, so the check in CI (web/test/templates.test.mjs) uses the same ones as the picker.

// Where the published catalog is: next to the page (GitHub Pages, a local build), and on
// GitHub Pages for a page that has none (the AP's own designer, ap/main/webui.py)
export const PAGES = "https://mathiasbk.github.io/openESL/";

// The variant of template t for a model, with "same_as" followed: { layout, icons, note }, or null
export function variantFor(t, model) {
  const v = t?.variants?.[model];
  if (!v) return null;
  if (v.same_as) {
    const base = t.variants[v.same_as];
    return base?.layout ? { ...base, note: v.note ?? base.note } : null;
  }
  return v.layout ? v : null;
}

// A layout file holding only the template's layout for this model (and its icons), to compile and
// draw it on its own; null when the template has no variant for the model
export function soloFile(t, model) {
  const v = variantFor(t, model);
  if (!v) return null;
  const d = { format: "esl-layouts", version: 1, model };
  if (v.icons && Object.keys(v.icons).length) d.icons = structuredClone(v.icons);
  d.layouts = [structuredClone(v.layout)];
  return d;
}

// name, or name-2, name-3 ... when the set has it (at most `max` characters)
export function freeName(name, taken, max = 31) {
  let n = 2;
  let out = name;
  while (taken.has(out)) out = `${name.slice(0, max - String(n).length - 1)}-${n++}`;
  return out;
}

// The layout file `doc` with the template's layout for `model` added at the end, as a new object:
// the layout gets a name the file does not have yet and "_template" (the template's id, which the
// AP ignores), and its icons are added to the file's, renamed when the file has another picture by
// that name. { doc, index, name }, or null when the template has no variant for the model.
export function addTemplate(doc, t, model) {
  const v = variantFor(t, model);
  if (!v) return null;
  const out = structuredClone(doc);
  out.layouts ||= [];
  const layout = structuredClone(v.layout);
  layout.name = freeName(layout.name, new Set(out.layouts.map((l) => l.name)));
  if (!t.example) layout._template = t.id;      // t.example: an example layout, not from the library
  const rename = {};
  for (const [name, icon] of Object.entries(v.icons || {})) {
    out.icons ||= {};
    const same = (n) => out.icons[n] && JSON.stringify(out.icons[n]) === JSON.stringify(icon);
    // icon names are a-z 0-9 _, at most 16 characters
    let n = name;
    for (let i = 2; out.icons[n] && !same(n); i++) n = `${name.slice(0, 15 - String(i).length)}_${i}`;
    out.icons[n] = structuredClone(icon);
    rename[name] = n;
  }
  for (const e of layout.elements || []) if (e.type === "icon" && rename[e.icon]) e.icon = rename[e.icon];
  out.layouts.push(layout);
  return { doc: out, index: out.layouts.length - 1, name: layout.name };
}

// The templates of the index that match a search text (title, description, tags, field names)
export function search(templates, text) {
  const words = String(text || "").toLowerCase().split(/\s+/).filter(Boolean);
  if (!words.length) return templates;
  return templates.filter((t) => {
    const hay = [t.id, t.title, t.description, ...(t.tags || []), ...(t.fields || [])].join(" ").toLowerCase();
    return words.every((w) => hay.includes(w));
  });
}
