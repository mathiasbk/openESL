// appics.js - the drawings of the AP setups on the Flash AP tab's cards (inline SVG): the
// boards, the cable or the WiFi between them, and the radio waves to a tag; and the tag flasher
// on the Flash tag tab. Shapes only, not photos of a particular board.

const PCB = "#23272e";
const CHIP = "#cfd3d8";
const GOLD = "#c9a227";
const USB = "#b9bcc2";
const ON = "#1f5fbf";

const text = (x, y, size, fill, s, weight = 700) =>
  `<text x="${x}" y="${y}" font-size="${size}" fill="${fill}" text-anchor="middle" font-family="system-ui,sans-serif" font-weight="${weight}">${s}</text>`;
const usbc = (x, y, on) => `<rect x="${x}" y="${y}" width="13" height="7" rx="3" fill="${on ? ON : USB}"/>`;

// A small board such as the C6 Super Mini, the USB port at the bottom; screen: a 0.96" screen
function small(x, y, label, screen) {
  const h = screen ? 50 : 44;
  let s = `<rect x="${x}" y="${y}" width="34" height="${h}" rx="3" fill="${PCB}"/>`;
  if (screen)
    s += `<rect x="${x + 4}" y="${y + 5}" width="26" height="30" rx="1" fill="#0d1b2e"/>` +
      text(x + 17, y + 18, 6.5, "#7fb3ff", "AP") + text(x + 17, y + 27, 5, "#9fd39f", "3 tags", 400);
  else
    s += `<path d="M${x + 5} ${y + 6}h24" stroke="${GOLD}" stroke-width="2"/>` +
      `<rect x="${x + 7}" y="${y + 12}" width="20" height="18" rx="1.5" fill="${CHIP}"/>` + text(x + 17, y + 23.5, 5.5, "#2b2f36", label);
  return s + usbc(x + 10.5, y + h - 6);
}

// A DevKitC-style board: the module at the top, two USB ports at the bottom, COM on the left and
// USB on the right; usb: the port marked USB drawn as the one to use
export function devkit(x, y, label, usb) {
  const pins = [...Array(10).keys()].map((i) =>
    `<circle cx="${x + 3.5}" cy="${y + 18 + i * 6}" r="1.2" fill="${GOLD}"/><circle cx="${x + 46.5}" cy="${y + 18 + i * 6}" r="1.2" fill="${GOLD}"/>`).join("");
  return `<rect x="${x}" y="${y}" width="50" height="86" rx="3" fill="${PCB}"/>` +
    `<rect x="${x + 8}" y="${y + 5}" width="34" height="38" rx="2" fill="${CHIP}"/><rect x="${x + 8}" y="${y + 5}" width="34" height="9" fill="#a9aeb5"/>` +
    text(x + 25, y + 31, 7, "#2b2f36", label) + pins +
    usbc(x + 9, y + 80, false) + usbc(x + 28, y + 80, usb) +
    text(x + 15.5, y + 77, 5, "#fff", "COM", 400) + text(x + 34.5, y + 77, 5, "#fff", "USB", 400);
}

const waves = (x, y, c = "#1d6b4f") =>
  `<g fill="none" stroke="${c}" stroke-width="2" stroke-linecap="round" opacity=".85"><path d="M${x} ${y} a6 6 0 0 1 0 -10"/><path d="M${x - 5} ${y + 3} a11 11 0 0 1 0 -16"/></g>`;
const tag = (x, y) =>
  `<rect x="${x}" y="${y}" width="22" height="13" rx="2" fill="#fff" stroke="#888"/><rect x="${x + 3}" y="${y + 3}" width="10" height="2" fill="#333"/><rect x="${x + 3}" y="${y + 7}" width="7" height="3" fill="#c62828"/>`;
const wifi = (cx, cy) =>
  `<g fill="none" stroke="${ON}" stroke-width="2.4" stroke-linecap="round"><path d="M${cx - 14} ${cy - 4} a20 20 0 0 1 28 0"/><path d="M${cx - 9} ${cy + 1} a13 13 0 0 1 18 0"/><path d="M${cx - 4} ${cy + 6} a6 6 0 0 1 8 0"/></g><circle cx="${cx}" cy="${cy + 10}" r="2.2" fill="${ON}"/>`;

// The picture of a card (kind from setupKind() in js/apsteps.js), chip "C6", "S3", "C5" ...
export function setupPicture(kind, chip) {
  const svg = (vb, body) => `<svg viewBox="${vb}" role="img" aria-hidden="true">${body}</svg>`;
  if (kind === "cable")
    return svg("0 0 200 110", devkit(30, 12, chip) + small(128, 36, "C6") +
      `<g fill="none" stroke-width="2.2" stroke-linecap="round"><path d="M80 40 C105 40 105 50 128 50" stroke="#e8a23a"/><path d="M80 47 C105 47 105 56 128 56" stroke="#4aa36c"/><path d="M80 54 C105 54 105 62 128 62" stroke="#333"/></g>` +
      waves(172, 62));
  if (kind === "wifi")
    return svg("0 0 200 110", devkit(22, 12, chip) + small(140, 36, "C6") + wifi(108, 40) +
      `<path d="M80 62 H136" stroke="${ON}" stroke-width="1.6" stroke-dasharray="3 4"/>` + waves(184, 62));
  if (kind === "screen") return svg("60 16 120 66", small(83, 28, chip, true) + waves(130, 53) + tag(148, 40));
  if (chip === "C6") return svg("60 18 120 66", small(83, 33, chip) + waves(130, 55) + tag(148, 42));
  // a bigger board: the C5, an S3 on its own, or a chip added later
  return svg("40 4 130 100", devkit(62, 10, chip) + waves(132, 60) + tag(140, 66));
}

// The tag flasher of the Flash tag tab: a D1 mini with the ZBS_Flasher on USB, wired to the
// pogo pins of a jig with a tag in it
export function flasherPicture() {
  const pins = [...Array(8).keys()].map((i) => `<circle cx="74" cy="${30 + i * 7}" r="1.4" fill="${GOLD}"/>`).join("");
  const wires = ["#e8a23a", "#4aa36c", "#c62828", "#333", "#1f5fbf"].map((c, i) =>
    `<path d="M74 ${37 + i * 7} C100 ${37 + i * 7} 104 ${48 + i * 6} 128 ${48 + i * 6}" stroke="${c}" stroke-width="2" fill="none" stroke-linecap="round"/>`).join("");
  const pogo = [...Array(5).keys()].map((i) => `<rect x="128" y="${46 + i * 6}" width="6" height="3" rx="1" fill="${GOLD}"/>`).join("");
  return `<svg viewBox="0 0 250 120" role="img" aria-hidden="true">` +
    // the D1 mini, its USB cable down to the computer
    `<path d="M45 96 V118" stroke="#555" stroke-width="5"/>` +
    `<rect x="16" y="20" width="62" height="78" rx="4" fill="#1d4f91"/>` +
    `<rect x="24" y="26" width="38" height="36" rx="2" fill="${CHIP}"/>` + text(43, 47, 7, "#2b2f36", "ESP8266") +
    pins + `<rect x="38" y="91" width="14" height="7" rx="2" fill="${USB}"/>` +
    text(43, 80, 7, "#fff", "D1 mini") + text(43, 89, 5.5, "#cfe0ff", "ZBS_Flasher", 400) +
    wires +
    // the jig, the tag lying on its pins
    `<rect x="126" y="28" width="112" height="72" rx="6" fill="#8a8f98"/>` + pogo +
    `<rect x="138" y="36" width="92" height="56" rx="4" fill="#fff" stroke="#666"/>` +
    `<rect x="146" y="44" width="50" height="6" fill="#333"/><rect x="146" y="56" width="34" height="5" fill="#333"/>` +
    `<rect x="146" y="68" width="40" height="12" fill="#c62828"/><rect x="200" y="72" width="22" height="12" fill="#bbb"/>` +
    `</svg>`;
}
