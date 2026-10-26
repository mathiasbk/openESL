#!/usr/bin/env node
// templates.mjs - checks the template catalog (templates/ in the repository) and publishes it
// for the web page (web/build.py runs it after building the renderer):
//
//   node web/templates.mjs [OUT]     OUT: default web/dist/templates
//
// OUT then holds categories.json, every template file under <category>/<id>.json, and
// index.json, which the designer's template picker (js/tplpicker.js) lists the catalog from:
// the categories in order, and per template its id, file, title, description, tags, fields and,
// per model, what its layout uses of the AP's limits (runs, rects, lines, icons, barcodes, QR
// codes, compiled bytes and JSON bytes).
//
// Each variant is compiled and drawn with its sample text by the renderer (dist/wasm/, the AP's
// own compiler) with the default AP board's limits, the smallest. Any error stops the build with
// the file and the model it is in. The format and the rules: wiki/labels/Label-templates.md.
// web/test/templates.test.mjs runs the same check, also on broken catalogs.
import { readFileSync, readdirSync, existsSync, mkdirSync, writeFileSync, rmSync, statSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { loadRenderer, MODELS, LAYOUT_FILE } from "./js/renderer.js";
import { MODELS as REGISTRY } from "./js/models.js";
import { layoutUsage } from "./js/limits.js";
import { soloFile } from "./js/catalog.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const ROOT = path.dirname(HERE);
const WASM = pathToFileURL(path.join(HERE, "dist", "wasm") + path.sep);

const ID = /^[a-z0-9][a-z0-9-]{0,30}$/;
const TEMPLATE_KEYS = ["format", "version", "id", "category", "title", "description", "tags", "docs", "fields", "variants"];
const VARIANT_KEYS = ["layout", "icons", "note", "same_as"];
const CATEGORY_KEYS = ["id", "title", "description", "order"];
const RED = new Set(["red", "red_invert"]);
const TEMPLATE_MAX = 1024;      // compiled bytes of one template: a quarter of a layout file

const str = (v) => typeof v === "string" && v.trim() !== "";
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

function readJson(file, errors) {
  try {
    return JSON.parse(readFileSync(file, "utf8"));
  } catch (e) {
    errors.push(`${file}: ${e.message}`);
    return null;
  }
}

// The catalog in dir (default templates/): { categories, templates: [{ file, t }] }, with
// errors for files that are not JSON
export function readCatalog(dir = path.join(ROOT, "templates"), errors = []) {
  const categories = readJson(path.join(dir, "categories.json"), errors);
  const templates = [];
  for (const sub of readdirSync(dir).sort()) {
    const d = path.join(dir, sub);
    if (!statSync(d).isDirectory()) continue;
    for (const f of readdirSync(d).sort()) {
      if (!f.endsWith(".json")) continue;
      const t = readJson(path.join(d, f), errors);
      if (t) templates.push({ file: `${sub}/${f}`, t });
    }
  }
  return { categories, templates };
}

// The layouts in the example files tag/designs/layouts/<model>.json, by model and name: a
// template with a layout of the same name must have the same fields and elements, so the
// designer's starting file and the catalog do not drift apart
function starterLayouts() {
  const out = {};
  for (const model of Object.keys(REGISTRY)) {
    const f = path.join(ROOT, "tag", "designs", "layouts", model + ".json");
    if (!existsSync(f)) continue;
    out[model] = Object.fromEntries(JSON.parse(readFileSync(f, "utf8")).layouts.map((l) => [l.name, l]));
  }
  return out;
}

// Checks a catalog ({ categories, templates } as readCatalog gives it) and makes its index.
// { errors: [text], index }
export async function checkCatalog({ categories, templates }, { root = ROOT, wasm = WASM, starter = starterLayouts() } = {}) {
  const errors = [];
  const cats = new Map();
  if (!categories || categories.format !== "esl-template-categories" || categories.version !== 1 || !Array.isArray(categories.categories)) {
    errors.push(`categories.json: needs "format": "esl-template-categories", "version": 1 and a list "categories"`);
  } else {
    for (const c of categories.categories) {
      const at = `categories.json: ${c?.id ?? "?"}`;
      for (const k of Object.keys(c || {})) if (!CATEGORY_KEYS.includes(k)) errors.push(`${at}: unknown key "${k}"`);
      if (!ID.test(c?.id || "")) errors.push(`${at}: id must be a-z 0-9 -`);
      else if (cats.has(c.id)) errors.push(`${at}: the id is there twice`);
      if (!str(c?.title) || !str(c?.description)) errors.push(`${at}: needs a title and a description`);
      if (typeof c?.order !== "number") errors.push(`${at}: needs a number "order"`);
      if (c?.id) cats.set(c.id, c);
    }
  }

  const renderers = {};
  for (const model of Object.keys(MODELS)) {
    renderers[model] = await loadRenderer(model, wasm);
    renderers[model].setLimits(null);         // the default board's: the smallest AP
  }

  const ids = new Map();
  const index = [];
  for (const { file, t } of templates) {
    const at = `templates/${file}`;
    const err = (s) => errors.push(`${at}: ${s}`);
    for (const k of Object.keys(t)) if (!TEMPLATE_KEYS.includes(k)) err(`unknown key "${k}"`);
    if (t.format !== "esl-template" || t.version !== 1) err(`needs "format": "esl-template" and "version": 1`);
    if (!ID.test(t.id || "")) err(`id must be 1-31 of a-z 0-9 -, starting with a letter or digit`);
    else if (ids.has(t.id)) err(`id "${t.id}" is also in templates/${ids.get(t.id)}`);
    else ids.set(t.id, file);
    if (file !== `${t.category}/${t.id}.json`) err(`must be templates/${t.category}/${t.id}.json (category/id.json)`);
    if (!cats.has(t.category)) err(`category "${t.category}" is not in categories.json`);
    if (!str(t.title) || !str(t.description)) err("needs a title and a description");
    if (t.tags !== undefined && !(Array.isArray(t.tags) && t.tags.every(str))) err("tags must be a list of words");
    if (t.docs !== undefined && !(str(t.docs) && existsSync(path.join(root, t.docs)))) err(`docs: no file ${t.docs} in the repository`);
    const fields = t.fields && typeof t.fields === "object" && !Array.isArray(t.fields) ? Object.keys(t.fields) : null;
    if (!fields?.length) err(`needs "fields": what each field means, by its name`);
    else for (const f of fields) if (!str(t.fields[f])) err(`fields.${f} needs a description`);
    const variants = t.variants && typeof t.variants === "object" ? Object.keys(t.variants) : [];
    if (!variants.length) err(`needs "variants", one per tag model`);

    const models = {};
    let layoutName = null;
    for (const model of variants) {
      const vat = `variant ${model}`;
      const v = t.variants[model];
      const m = REGISTRY[model];
      if (!m) {
        err(`${vat}: no tag model "${model}" in shared/models.json`);
        continue;
      }
      for (const k of Object.keys(v || {})) if (!VARIANT_KEYS.includes(k)) err(`${vat}: unknown key "${k}"`);
      if (v?.note !== undefined && !str(v.note)) err(`${vat}: note must be a text`);
      if (v?.same_as !== undefined) {
        const base = REGISTRY[v.same_as];
        if (v.layout || v.icons) err(`${vat}: same_as takes no layout or icons of its own`);
        if (!t.variants[v.same_as]?.layout) err(`${vat}: same_as "${v.same_as}" is not a variant with a layout here`);
        else if (!base || base.width !== m.width || base.height !== m.height || !same(base.colors, m.colors))
          err(`${vat}: same_as "${v.same_as}" needs the same screen (${m.width}x${m.height}, ${m.colors.join("/")})`);
        else models[model] = { ...models[v.same_as], same_as: v.same_as };
        continue;
      }
      const l = v?.layout;
      if (!l || typeof l !== "object") {
        err(`${vat}: needs a layout`);
        continue;
      }
      if (!same(l.fields, fields)) err(`${vat}: the layout's fields must be the template's, in the same order: ${JSON.stringify(fields)}`);
      if (layoutName === null) layoutName = l.name;
      else if (l.name !== layoutName) err(`${vat}: the layout must have the same name in every variant ("${layoutName}")`);
      if (!Array.isArray(l._sample) || l._sample.length !== (l.fields || []).length || !l._sample.every((s) => typeof s === "string"))
        err(`${vat}: _sample needs one sample text per field`);
      for (const [i, e] of (l.elements || []).entries())
        if (RED.has(e.color) && !m.colors.includes("red")) err(`${vat}: element ${i} is ${e.color}, which the ${model}" has not`);
      const s = starter[model]?.[l.name];
      if (s && !(same(s.fields, l.fields) && same(s.elements, l.elements)))
        err(`${vat}: tag/designs/layouts/${model}.json has a layout "${l.name}" with other fields or elements; keep them the same`);

      const r = renderers[model];
      const solo = soloFile(t, model);
      const res = r.loadLayouts(JSON.stringify(solo));
      if (!res.ok) {
        err(`${vat}: the AP would refuse it: ${res.error}`);
        continue;
      }
      if (res.size > TEMPLATE_MAX) err(`${vat}: ${res.size} bytes compiled; keep a template within ${TEMPLATE_MAX}, so it fits in a file with others`);
      const shown = r.render(LAYOUT_FILE, 0, Array.isArray(l._sample) ? l._sample : []);
      if (!shown.ok || shown.refused) err(`${vat}: the AP would refuse the sample text: ${shown.error || shown.refused}`);
      const u = layoutUsage(l, r.limits);
      models[model] = {
        runs: u.runs, rects: u.rects, lines: u.lines, icons: u.icons, barcodes: u.barcodes, qrs: u.qrs,
        bytes: res.size,
        json: new TextEncoder().encode(JSON.stringify(solo.layouts[0]) + JSON.stringify(solo.icons || {})).length,
      };
    }
    // a model with same_as listed before the variant it points at
    for (const model of variants) {
      const v = t.variants[model];
      if (v?.same_as && models[v.same_as] && !models[model]?.bytes) models[model] = { ...models[v.same_as], same_as: v.same_as };
    }
    index.push({
      id: t.id, file, category: t.category, title: t.title, description: t.description,
      tags: t.tags || [], docs: t.docs, fields: fields || [], layout: layoutName, models,
    });
  }
  const order = [...cats.values()].sort((a, b) => a.order - b.order);
  return {
    errors,
    index: {
      format: "esl-template-index", version: 1,
      categories: order.map(({ id, title, description }) => ({ id, title, description })),
      templates: index.sort((a, b) => order.findIndex((c) => c.id === a.category) - order.findIndex((c) => c.id === b.category)),
    },
  };
}

// The models that templates have no variant for yet, by model: { "1.54": ["id", ...] }
export function missingVariants(index) {
  const out = {};
  for (const model of Object.keys(REGISTRY))
    out[model] = index.templates.filter((t) => !t.models[model]).map((t) => t.id);
  return out;
}

async function main() {
  const out = path.resolve(process.argv[2] || path.join(HERE, "dist", "templates"));
  const readErrors = [];
  const cat = readCatalog(undefined, readErrors);
  const { errors, index } = await checkCatalog(cat);
  errors.unshift(...readErrors);
  if (errors.length) {
    console.error("The template catalog (templates/) has errors:\n  " + errors.join("\n  "));
    process.exit(1);
  }
  rmSync(out, { recursive: true, force: true });
  mkdirSync(out, { recursive: true });
  writeFileSync(path.join(out, "categories.json"), JSON.stringify(cat.categories, null, 2) + "\n");
  for (const { file, t } of cat.templates) {
    mkdirSync(path.join(out, path.dirname(file)), { recursive: true });
    writeFileSync(path.join(out, file), JSON.stringify(t) + "\n");
  }
  writeFileSync(path.join(out, "index.json"), JSON.stringify(index) + "\n");
  for (const [model, ids] of Object.entries(missingVariants(index)))
    if (ids.length) console.log(`Templates without a ${model}" variant: ${ids.join(", ")}`);
  console.log(`${index.templates.length} templates in ${index.categories.length} categories in ${out}`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await main();
