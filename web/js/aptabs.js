// aptabs.js - the tabs of the page the AP serves at /dashboard (ap/main/webui.py packs the
// tabs its board has room for, each a <main class="tab">): #name in the address picks one,
// the first is shown without it. The nav has a link per tab when there is more than one.
const tabs = [...document.querySelectorAll("main.tab")];

function show() {
  const want = "tab-" + location.hash.slice(1);
  const tab = tabs.find((t) => t.id === want) || tabs[0];
  for (const t of tabs) {
    // a modal dialog left open in a tab that is hidden (Back, a typed #name) would keep the
    // whole page inert behind it, unseen
    if (t !== tab) t.querySelectorAll("dialog[open]").forEach((d) => d.close());
    t.hidden = t !== tab;
  }
  document.querySelectorAll(".tabs a[data-tab]").forEach((a) => {
    if ("tab-" + a.dataset.tab === tab.id) a.setAttribute("aria-current", "page");
    else a.removeAttribute("aria-current");
  });
}

window.addEventListener("hashchange", () => {
  show();
  window.scrollTo(0, 0);
});
show();
