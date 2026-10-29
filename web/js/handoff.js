// handoff.js - hands a layout file from the designer on the web page to the AP's own page
// over WiFi. A page on https may not call an AP on http, but it may open the AP's page in a
// new tab and talk to it with postMessage:
//
//   web page -> opens  http://<AP>/dashboard?from=designer
//   AP page  -> web:   { type: "openesl-ready", v: 1 }
//   web -> AP page:    { type: "openesl-layouts", v: 1, model, file, layout, content, preview }
//   AP page  -> web:   { type: "openesl-saved", v: 1, model, ok, text }
//
// The AP's page shows what came (js/apreceive.js) and stores nothing until the user presses
// Save there, with the token that page already has. It takes messages only from the
// published web page, or from a page on this computer (a local build).

export const READY = "openesl-ready";
export const LAYOUTS = "openesl-layouts";
export const SAVED = "openesl-saved";
export const VERSION = 1;
export const QUERY = "from=designer";
export const WEB_ORIGIN = "https://mathiasbk.github.io";

// What the user typed as the AP's address -> { url } of its page that receives, or { error }.
// The AP serves http only; "openesl.local" or "192.168.1.20" get http:// in front.
export function apPageUrl(input) {
  let t = String(input || "").trim() || "openesl.local";
  if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(t)) t = "http://" + t;
  let u;
  try {
    u = new URL(t);
  } catch {
    return { error: `"${input}" is not an address` };
  }
  if (u.protocol !== "http:") return { error: `the AP's page is on http, so use http://${u.host}` };
  if (!u.hostname) return { error: `"${input}" is not an address` };
  return { url: `${u.origin}/dashboard?${QUERY}` };
}

// The AP's page takes layouts from the published web page and from pages on this computer
export function trustedOrigin(origin) {
  if (origin === WEB_ORIGIN) return true;
  try {
    const u = new URL(origin);
    return ["localhost", "127.0.0.1", "[::1]"].includes(u.hostname);
  } catch {
    return false;
  }
}

// The message the designer sends: the layout file of one model, the layout shown, the content
// to put on a tag if the user picks one there, and the preview as a PNG data URL
export function layoutsMessage({ model, file, layout, content, preview }) {
  return { type: LAYOUTS, v: VERSION, model, file, layout, content, preview };
}

// A message from the designer -> { model, file, layout, content, preview } or { error }
export function readLayoutsMessage(data) {
  if (!data || data.type !== LAYOUTS) return { error: "not a layout message" };
  if (data.v !== VERSION) return { error: "made by another version of the web page; reload it" };
  const { model, file, layout, content, preview } = data;
  if (typeof model !== "string" || !model) return { error: "no tag model" };
  if (!file || typeof file !== "object" || file.format !== "esl-layouts" || !Array.isArray(file.layouts))
    return { error: "not a layout file" };
  if (file.model !== undefined && file.model !== model) return { error: `a file for ${file.model}", sent for ${model}"` };
  const pic = typeof preview === "string" && /^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(preview) ? preview : "";
  const c = content && typeof content === "object" && typeof content.layout === "string" && Array.isArray(content.fields)
    ? content : null;
  return { model, file, layout: typeof layout === "string" ? layout : "", content: c, preview: pic };
}

// The layout names of a file, and how they compare with the file the AP has (null: none):
// { names, added, removed, kept }
export function compareFiles(incoming, current) {
  const names = (f) => (f?.layouts || []).map((l) => l.name).filter((n) => typeof n === "string");
  const now = names(current);
  const next = names(incoming);
  return {
    names: next,
    added: next.filter((n) => !now.includes(n)),
    removed: now.filter((n) => !next.includes(n)),
    kept: next.filter((n) => now.includes(n)),
  };
}
