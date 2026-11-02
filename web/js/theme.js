// theme.js - the light/dark/auto button in the top bar (#317). Auto follows the system setting
// (prefers-color-scheme in css/style.css); Light and Dark set data-theme on <html>, which
// style.css reads. The choice is kept in this browser. A small script in the <head> of
// index.html applies a kept choice before the page is drawn, so it does not flash.
const KEY = "openesl.theme";
// After Auto comes the opposite of what the system shows, so the first click always changes something
const dark = () => matchMedia("(prefers-color-scheme: dark)").matches;
const order = () => (dark() ? ["auto", "light", "dark"] : ["auto", "dark", "light"]);
const MODES = ["auto", "light", "dark"];
const nextOf = (mode) => { const o = order(); return o[(o.indexOf(mode) + 1) % o.length]; };
const LABEL = { auto: "Auto", light: "Light", dark: "Dark" };
// 24x24 outline icons: half-filled circle, sun, moon
const ICON = {
  auto: '<circle cx="12" cy="12" r="9"/><path d="M12 3a9 9 0 0 1 0 18z" fill="currentColor"/>',
  light: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>',
  dark: '<path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z"/>',
};

export function stored() {
  try {
    const v = localStorage.getItem(KEY);
    return MODES.includes(v) ? v : "auto";
  } catch {
    return "auto";
  }
}

export function apply(mode) {
  if (mode === "light" || mode === "dark") document.documentElement.dataset.theme = mode;
  else delete document.documentElement.dataset.theme;
}

function save(mode) {
  try {
    if (mode === "auto") localStorage.removeItem(KEY);
    else localStorage.setItem(KEY, mode);
  } catch { /* private window: the choice lasts until the page is closed */ }
}

function draw(btn, mode) {
  const next = nextOf(mode);
  btn.innerHTML = `<svg viewBox="0 0 24 24" aria-hidden="true">${ICON[mode]}</svg><span>${LABEL[mode]}</span>`;
  btn.setAttribute("aria-label", `Colour theme: ${LABEL[mode]}. Change to ${LABEL[next]}`);
  btn.title = `Colour theme: ${LABEL[mode]} (click for ${LABEL[next]})`;
}

// Adds the button to the end of the page's top bar
export function mount(header = document.querySelector("header.top")) {
  if (!header || header.querySelector(".theme-btn")) return null;
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "theme-btn";
  let mode = stored();
  apply(mode);
  draw(btn, mode);
  btn.addEventListener("click", () => {
    mode = nextOf(mode);
    apply(mode);
    save(mode);
    draw(btn, mode);
  });
  header.append(btn);
  return btn;
}

mount();
