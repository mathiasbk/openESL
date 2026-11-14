// dashnav.js - the menu of the dashboard: a column on the left on a wide screen, a bar at the
// bottom of a phone's screen. It shows one part of the dashboard at a time (Tags, Layouts,
// Data sources, Firmware, Access point, Settings), named in the address as #dashboard/<part>,
// so Back, a bookmark and a link from elsewhere (#dashboard/firmware) work. Each entry says
// in a few words how its part is doing, and has a badge when it needs a look.

const $ = (id) => document.getElementById(id);
const PARTS = ["tags", "layouts", "sources", "firmware", "ap", "settings"];
// The cards that come and go with what the AP has: no entry for them while they are hidden
const CARD = { layouts: "lay-card", sources: "src-box" };
// The one-line summary each card already keeps, shown under its entry
const SUM = { layouts: "lay-sum", sources: "src-sum", firmware: "dash-fw-sum", ap: "dash-ap-sum" };

const link = (p) => document.querySelector(`#dash-nav a[data-part="${p}"]`);
const pane = (p) => document.querySelector(`.dash-part[data-part="${p}"]`);

// The part the address names: #dashboard alone, or a part that is not there, is Tags
function wanted() {
  const [tab, part] = location.hash.slice(1).split("/");
  if (tab !== "dashboard" && tab !== "") return null;
  return PARTS.includes(part) && !link(part).hidden ? part : "tags";
}

function show() {
  const part = wanted();
  if (!part) return;
  for (const p of PARTS) {
    pane(p).hidden = p !== part;
    // a modal dialog left open in a part that is hidden would keep the page inert behind it
    if (p !== part) pane(p).querySelectorAll("dialog[open]").forEach((d) => d.close());
    if (p === part) link(p).setAttribute("aria-current", "page");
    else link(p).removeAttribute("aria-current");
  }
}

// Shows a part: the address changes, so Back returns to the one before
export function showPart(part) {
  const hash = part === "tags" ? "#dashboard" : "#dashboard/" + part;
  if (location.hash !== hash) location.hash = hash;
  else show();
}

// A badge on an entry: text (a number, or "!"), and its kind ("bad", "warn" or "new"); "" hides it
export function navBadge(part, text, kind = "", label = "") {
  const b = link(part)?.querySelector(".badge");
  if (!b) return;
  b.hidden = !text;
  b.textContent = text;
  b.className = "badge" + (kind ? " " + kind : "");
  // read with the entry's name: "Tags, 11 need attention"
  if (label) b.setAttribute("aria-label", label);
  else b.removeAttribute("aria-label");
}

// The line under an entry (the Tags entry has no card summary to copy)
export function navNote(part, text) {
  const s = link(part)?.querySelector("small");
  if (s && s.textContent !== text) s.textContent = text;
}

export function initDashNav() {
  const watch = (node, opts, fn) => {
    new MutationObserver(fn).observe(node, opts);
    fn();
  };
  for (const [part, id] of Object.entries(SUM))
    watch($(id), { childList: true, characterData: true, subtree: true }, () => navNote(part, $(id).textContent.trim()));
  for (const [part, id] of Object.entries(CARD))
    watch($(id), { attributes: true, attributeFilter: ["hidden"] }, () => {
      link(part).hidden = $(id).hidden;
      // the part on screen went away (an AP without it): back to Tags; or the address named it
      // before the AP said it has it
      show();
    });
  // Tags that show a layout the AP does not have
  watch($("lay-warn"), { attributes: true, attributeFilter: ["hidden"] },
    () => navBadge("layouts", $("lay-warn").hidden ? "" : "!", "warn", "tags show a layout the AP does not have"));
  window.addEventListener("hashchange", show);
  show();
}
