// layoutinfo.js - how the layouts in a layout file compare with the AP's, and which of the
// AP's tags use each one. Used by the designer's layout column and the dashboard's Layouts page.

import { layoutModel } from "./models.js";

// JSON with the keys sorted, so two layouts that differ only in key order compare equal
export function canon(v) {
  if (Array.isArray(v)) return "[" + v.map(canon).join(",") + "]";
  if (v && typeof v === "object")
    return "{" + Object.keys(v).sort().map((k) => JSON.stringify(k) + ":" + canon(v[k])).join(",") + "}";
  return JSON.stringify(v);
}

// Each layout of `file` against the AP's file of the same model (null: the AP has none):
// { states: { name: "ap" | "changed" | "new" }, gone: [names the AP has and the file not] }.
// "ap": the AP has it as it is; "changed": the AP has one by that name, but not this one;
// "new": the AP has none by that name.
export function compareLayouts(file, apFile) {
  const theirs = new Map((apFile?.layouts || []).map((l) => [l.name, canon(l)]));
  const states = {};
  for (const l of file?.layouts || []) {
    const t = theirs.get(l.name);
    states[l.name] = t === undefined ? "new" : t === canon(l) ? "ap" : "changed";
  }
  const gone = [...theirs.keys()].filter((n) => !(n in states));
  return { states, gone };
}

// The layout each tag shows, by MAC. GET /api/tags leaves the content out, so this asks for
// each tag not in `known` yet (a few at a time); a tag without content maps to "". client:
// anything with call(method, path), an ApClient or a SerialAp.
export async function tagLayouts(client, tags, known = {}, { max = 200, parallel = 3 } = {}) {
  const todo = tags.filter((t) => !(t.mac in known)).slice(0, max);
  const out = { ...known };
  let next = 0;
  const worker = async () => {
    while (next < todo.length) {
      const t = todo[next++];
      try {
        out[t.mac] = (await client.call("GET", "/api/tags/" + encodeURIComponent(t.mac))).layout || "";
      } catch { /* left out: asked again next time */ }
    }
  };
  await Promise.all(Array.from({ length: Math.min(parallel, todo.length) }, worker));
  return out;
}

// How many of `tags` of `model` show each layout: { name: count }
export function countByLayout(tags, layouts, model) {
  const n = {};
  for (const t of tags) {
    if (layoutModel(t.model) !== model) continue; // also the models whose sameAs names it
    const l = layouts[t.mac];
    if (l) n[l] = (n[l] || 0) + 1;
  }
  return n;
}
