// apsteps.js - what the Flash AP tab's wizard (js/apwizard.js) does for each AP setup: the
// cards of step 1 and the parts of steps 2 and 3, from the boards and radios in
// firmware/info.json (web/build.py). No DOM here, so test/apsteps.test.mjs can check it.

// "ESP32-S3" -> "S3"
export const shortChip = (family) => String(family || "").replace(/^ESP32-?/i, "") || "ESP32";

// The kind of setup a board is: one board ("single", "screen"), or an S3 with a C6 radio on a
// cable ("cable") or over WiFi ("wifi"). It picks the card's picture and title.
export function setupKind(board) {
  if (board.radio === "uart") return "cable";
  if (board.radio === "net") return "wifi";
  return board.display ? "screen" : "single";
}

// The boards of an info.json, also one from before ap_boards (only the C6 and the C6 with the
// status screen)
export function boardsOf(info) {
  if (info?.ap_boards?.length) return info.ap_boards;
  return [
    ...(info?.ap ? [{ id: "c6-supermini", name: "ESP32-C6", chip_family: "ESP32-C6", status: "supported", manifest: "ap-manifest.json" }] : []),
    ...(info?.ap_lcd ? [{ id: "c6-supermini-lcd", name: "ESP32-C6 with status screen", chip_family: "ESP32-C6", display: true, status: "supported", manifest: "ap-lcd-manifest.json" }] : []),
  ];
}

// The radio images a board takes (radio_fw, keys in info.radios) that the page has
export function radiosFor(board, info) {
  const all = info?.radios || [];
  return (board.radio_fw || []).map((k) => all.find((r) => r.key === k)).filter(Boolean);
}

// A card of step 1: { board, title, chips, status, pic, ready }. A board with a C6 radio is
// ready only when the page has its radio image too.
export function card(board, info) {
  const kind = setupKind(board);
  const chip = shortChip(board.chip_family || board.target?.replace(/^esp32/, "ESP32-"));
  const title = {
    cable: `${chip} + C6 on cable`,
    wifi: `${chip} + C6 over WiFi`,
    screen: `${chip} with screen`,
    single: `ESP32-${chip}`,
  }[kind];
  const radio = kind === "cable" || kind === "wifi";
  return {
    board,
    title,
    chips: radio ? [chip, "C6 radio"] : [chip],
    status: board.status || "supported",
    pic: kind,
    chip,
    ready: !radio || radiosFor(board, info).length > 0,
  };
}

export const cards = (info) => boardsOf(info).map((b) => card(b, info));

// Boards with two USB ports (DevKitC-style S3 and C5 boards): the AP's serial API and the
// WiFi step answer only on the one marked USB (the chip's own USB Serial/JTAG), not on the
// UART bridge (COM)
export const twoPorts = (board) => /^esp32(s3|c5)$/.test(board.target || "");

// Espressif's USB vendor id: a port with it is the chip's own USB (USB Serial/JTAG)
export const ESPRESSIF_VID = 0x303a;
export const isNativeUsb = (info) => info?.usbVendorId === ESPRESSIF_VID;

// The parts of steps 2 (flash) and 3 (connect) for a board and the radio image picked:
//   flash:   { kind: "flash", role: "radio" | "ap", chip, manifest, title, usbPort }
//            { kind: "wire" }
//   connect: { kind: "wifi" } and, for the radio over WiFi, { kind: "join" }
// The C6 radio is flashed first, so the main board stays plugged in from its flashing to its
// WiFi: one change of cable for the S3 with a C6 on a cable, two over WiFi.
export function plan(board, radio) {
  const kind = setupKind(board);
  const chip = board.chip_family || "ESP32-C6";
  const flash = [];
  if ((kind === "cable" || kind === "wifi") && radio)
    flash.push({ kind: "flash", role: "radio", chip: radio.chip_family, manifest: radio.manifest, title: "C6 radio", usbPort: false });
  flash.push({ kind: "flash", role: "ap", chip, manifest: board.manifest, title: "ESP32-" + shortChip(chip), usbPort: twoPorts(board) });
  if (kind === "cable") flash.push({ kind: "wire" });
  const connect = [{ kind: "wifi" }];
  if (kind === "wifi") connect.push({ kind: "join" });
  return { flash, connect };
}

// ---------- the partition table already on the board ----------

const PT_ENTRY = 32;

// The NVS partition of a partition table (0xAA 0x50, type data, subtype nvs): { offset, size },
// or null
function nvsOf(pt) {
  for (let n = 0; n + PT_ENTRY <= pt.length && pt[n] === 0xaa && pt[n + 1] === 0x50; n += PT_ENTRY) {
    if (pt[n + 2] !== 0x01 || pt[n + 3] !== 0x02) continue;
    const v = new DataView(pt.buffer, pt.byteOffset + n, PT_ENTRY);
    return { offset: v.getUint32(4, true), size: v.getUint32(8, true) };
  }
  return null;
}

// Whether an update keeps what the board has stored, from the partition table at 0x8000 on the
// board and the one about to be written: "same" (NVS, with WiFi, token, tags and layout files,
// is where the new table has it: kept, also when the table gained a partition, as when tagfw came),
// "blank" (nothing there: a new or erased board), or "different" (NVS elsewhere, as
// OpenESL from before 0.3.0, or other firmware: its data cannot be kept)
export function compareTables(onBoard, ours) {
  if (onBoard.subarray(0, PT_ENTRY).every((b) => b === 0xff)) return "blank";
  const have = nvsOf(onBoard);
  const want = nvsOf(ours);
  return have && want && have.offset === want.offset && have.size === want.size ? "same" : "different";
}

// ---------- the radio over WiFi's console (ap/netradio/main/netradio_main.c) ----------

// The lines that set up a radio over WiFi: its network, the AP's address (empty: the radio
// looks for the AP on mDNS), then a restart
export function radioCommands({ ssid, pass, host }) {
  return [`ssid ${ssid}`, `pass ${pass}`, `host ${host || ""}`, "reboot"];
}

// What a line of the radio's log says about joining the AP: { state, ip?, rssi? } with state
// "joined" (also from the "link: up" line of status, with the WiFi signal at the radio),
// "link-down", "wifi" (on the network), "no-ap" (not found on mDNS, or the address does not
// answer), "no-wifi" (no network set), or null for anything else
export function radioLine(line) {
  let m = line.match(/connected to the AP at ([0-9.]+)(?::\d+)?(, but HELLO failed)?/);
  if (m) return m[2] ? { state: "no-ap", ip: m[1] } : { state: "joined", ip: m[1] };
  if ((m = line.match(/link: (up|down), WiFi rssi (-?\d+)/)))
    return { state: m[1] === "up" ? "joined" : "link-down", rssi: Number(m[2]) || null };
  if ((m = line.match(/WiFi up: ([0-9.]+)/))) return { state: "wifi", ip: m[1] };
  if (/no AP found on mDNS|AP \S+ does not answer/.test(line)) return { state: "no-ap" };
  if (/no WiFi set/.test(line)) return { state: "no-wifi" };
  return null;
}
