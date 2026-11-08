// renderframe.js - the renderer for the dashboard's previews (the Layouts card, js/aplayouts.js,
// and the preview of "Send text", js/dashboard.js), wherever the page runs:
//
//   const r = await openRenderer("2.9", base)
//       base "" (the web page, an AP with the Design tab): js/renderer.js next to this file
//       base a URL (the AP's own page on a board without the renderer, the C6): the published
//       page's js/renderer.js and wasm/, run in a sandboxed frame
//   draw(r, { file, limits, fonts, layout, flags, fields, lang })
//       -> { ok, pixels } (r.width x r.height, 1 = black, 2 = red) or { ok: false, error },
//       straight away from the page's own renderer, as a promise from the frame
//
// Why the frame: the AP's page keeps the API token in this origin's localStorage, and code
// from another site that runs here could read it. The frame has sandbox="allow-scripts" and
// no allow-same-origin, so its origin is opaque: the code in it cannot read this page, its
// storage or its token, and talks to it only through the messages below. What the page sends
// it is what a preview needs (a layout file, sample text, the AP's limits and font files).
// The published page always has the newest release, so rendererNote() says when it was built
// for other AP firmware than this AP runs, or could not be loaded at all.

import { MODELS } from "./models.js";

export const LAYOUT_TEXT = 2;      // as in js/renderer.js
export const FLAG_FRAME = 0x02;

const TIMEOUT_MS = 20000;

// One picture, in one go so that nothing else uses the renderer (shared with the designer)
// between loading the file and drawing it. The frame runs this same function (its source, put
// into frameCode's template: keep it free of backticks and template literals).
function drawWith(r, job) {
  if (job.file) {
    // checked with this AP's limits and font files each time, which the file may name
    if (r.setLimits) r.setLimits(job.limits || null);
    if (r.setFonts) r.setFonts(job.fonts?.files || {}, job.fonts?.slots || {});
    const res = r.loadLayouts(JSON.stringify(job.file));
    if (!res.ok) return { ok: false, error: res.error };
  }
  r.lang = job.lang === "nb" ? "nb" : "en"; // the AP's date language
  const out = r.render(job.layout, job.flags || 0, job.fields || []);
  return out.ok ? { ok: true, pixels: out.pixels } : { ok: false, error: out.error };
}

export async function openRenderer(model, base = "") {
  if (!base) {
    const R = await import("./renderer.js");
    return R.loadRenderer(model);
  }
  if (!MODELS[model]) throw new Error("unknown model " + model);
  const frame = frameFor(base);
  await frame.call(model, "open");
  // the screen's size from this page's own models.js, not from the frame
  return { model, width: MODELS[model].width, height: MODELS[model].height, frame };
}

export function draw(r, job) {
  return r.frame ? r.frame.call(r.model, "draw", job).then((out) => checked(r, out)) : drawWith(r, job);
}

// The frame's answer as drawWith gives it, or { ok: false }: the page only draws pixels from it
function checked(r, out) {
  const n = r.width * r.height;
  if (out?.ok === true && out.pixels instanceof Uint8Array && out.pixels.length >= n) return { ok: true, pixels: out.pixels };
  return { ok: false, error: typeof out?.error === "string" ? out.error : "no picture from the renderer" };
}

// The code in the frame. It imports the renderer from base and answers each { id, model,
// method, job } from its parent with { id, value } or { id, error }.
function frameCode(base) {
  return `
const base = ${JSON.stringify(base)};
${drawWith.toString()}
let R;
const renderers = {};
addEventListener("message", async (e) => {
  if (e.source !== parent || typeof e.data?.id !== "number") return;
  const { id, model, method, job } = e.data;
  try {
    R ||= await import(new URL("js/renderer.js", base).href);
    // a renderer that failed to load is tried again next time (a fetch that failed once)
    const r = await (renderers[model] ||= R.loadRenderer(model, new URL("wasm/", base)).catch((err) => {
      delete renderers[model];
      throw err;
    }));
    let value = null;
    if (method === "draw") value = drawWith(r, job);
    else if (method !== "open") throw new Error("no method " + method);
    parent.postMessage({ id, value }, "*");
  } catch (err) {
    parent.postMessage({ id, error: String(err?.message || err) }, "*");
  }
});
parent.postMessage({ ready: true }, "*");
`;
}

const frames = {};
function frameFor(base) {
  return (frames[base] ||= makeFrame(base));
}

function makeFrame(base) {
  let seq = 0;
  const waiting = new Map();
  const f = document.createElement("iframe");
  f.setAttribute("sandbox", "allow-scripts");
  f.setAttribute("aria-hidden", "true");
  f.tabIndex = -1;
  f.hidden = true;
  // Its own policy too: it loads code and data from base only, so even a changed published
  // page cannot send what the page gives it (layout files, sample text) anywhere else
  const origin = new URL(base).origin;
  const csp = `default-src 'none'; script-src 'unsafe-inline' 'wasm-unsafe-eval' ${origin}; connect-src ${origin}`;
  f.srcdoc = `<!doctype html><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="${csp}">` +
    `<script type="module">${frameCode(base)}</script>`;
  const ready = new Promise((resolve, reject) => {
    setTimeout(() => reject(new Error("the renderer frame did not start")), TIMEOUT_MS);
    addEventListener("message", (e) => {
      // only the frame's own messages, and only plain answers (they are drawn, never run)
      if (e.source !== f.contentWindow || typeof e.data !== "object" || e.data === null) return;
      if (e.data.ready) return resolve();
      const w = waiting.get(e.data.id);
      if (!w) return;
      waiting.delete(e.data.id);
      clearTimeout(w.timer);
      if (typeof e.data.error === "string") w.reject(new Error(e.data.error));
      else w.resolve(e.data.value);
    });
  });
  ready.catch(() => {});   // said by each call
  document.body.append(f);
  return {
    async call(model, method, job = null) {
      await ready;
      const id = ++seq;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          waiting.delete(id);
          reject(new Error(`could not load the renderer from ${base}`));
        }, TIMEOUT_MS);
        waiting.set(id, { resolve, reject, timer });
        f.contentWindow.postMessage({ id, model, method, job }, "*");
      });
    },
  };
}

// What to say about the previews on the AP's own page: r the renderer (null: it could not be
// loaded), base where it came from ("" for the AP's own renderer), apFw the version the AP
// runs and pageAp the AP version of the published page (ap_version in its firmware/info.json).
// "" when there is nothing to say.
export function rendererNote(r, base, apFw, pageAp) {
  if (!base) return "";
  const site = String(base).replace(/^https?:\/\//, "").replace(/\/$/, "");
  if (!r) return `No previews: this AP has no renderer of its own and could not load the one from ${site} (no internet?). The layouts work as before.`;
  const strip = (v) => String(v || "").replace(/^v/, "").trim();
  if (strip(apFw) && strip(pageAp) && strip(apFw) !== strip(pageAp))
    return `The previews are drawn by the renderer of AP firmware ${strip(pageAp)} from ${site}; this AP runs ${strip(apFw)}, so a layout may look a little different on the tags.`;
  return "";
}
