// apflash.js - writes AP and radio firmware to an ESP32 over Web Serial for the Flash AP tab,
// with Espressif's esptool-js (loaded from unpkg, version pinned): the parts in a manifest
// (web/build.py, split_ap_image), after checking the chip and, for an update, the partition
// table already on the board.
import { compareTables } from "./apsteps.js";

const ESPTOOL_URL = "https://unpkg.com/esptool-js@0.6.1/bundle.js";
let esptool = null;
const loadEsptool = () => (esptool ??= import(ESPTOOL_URL));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class FlashError extends Error {
  // code: "chip" (another chip on the port), "blank" or "different" (the partition table on
  // the board: an update cannot keep its data), or "" for the rest
  constructor(code, message) {
    super(message);
    this.code = code;
  }
}

// The parts of a manifest in firmware/: [{ address, data }]. v is the release, so a browser
// does not use files cached from an earlier one.
export async function loadParts(manifest, v = "") {
  const q = v ? "?v=" + encodeURIComponent(v) : "";
  const r = await fetch("firmware/" + manifest + q, { cache: "no-cache" });
  if (!r.ok) throw new FlashError("", `firmware/${manifest}: ${r.status}`);
  const parts = (await r.json()).builds?.[0]?.parts || [];
  return Promise.all(parts.map(async (p) => {
    const f = await fetch("firmware/" + p.path);
    if (!f.ok) throw new FlashError("", `firmware/${p.path}: ${f.status}`);
    return { address: p.offset, data: new Uint8Array(await f.arrayBuffer()) };
  }));
}

// Flashes parts to the board on port. chip is the chip family the step expects ("ESP32-S3").
// erase wipes the whole flash first. Otherwise the partition table on the board decides: the
// same NVS as in parts keeps it (WiFi, token, tags, layout files), a blank board is erased (there
// is nothing to keep), and another table is refused with FlashError "different", so the page
// can ask before it erases.
// on(state, fraction): state "connect", "keep" or "erase", "write" (fraction 0..1), "reset".
// Returns { chip, kept }. The port is closed again afterwards.
export async function flashBoard(port, { chip, parts, erase, on = () => {} }) {
  const { Transport, ESPLoader } = await loadEsptool();
  const transport = new Transport(port, false);
  const quiet = { clean() {}, writeLine() {}, write() {} };
  const loader = new ESPLoader({ transport, baudrate: 460800, terminal: quiet });
  try {
    on("connect", 0);
    try {
      await loader.main();
    } catch (e) {
      throw new FlashError("", "the board did not answer. Hold BOOT while you plug it in, then try again (" + (e.message || e) + ")");
    }
    const found = loader.chip.CHIP_NAME;
    if (found !== chip)
      throw new FlashError("chip", `this is an ${found}, and this step is for the ${chip}. Plug in the ${chip}.`);
    let kept = false;
    if (!erase) {
      const pt = parts.find((p) => p.address === 0x8000);
      const same = pt ? compareTables(await loader.readFlash(0x8000, 0x400), pt.data) : "blank";
      if (same === "different")
        throw new FlashError(same, "this board has another partition table (OpenESL from before 0.3.0, or other firmware), so what it has stored cannot be kept.");
      kept = same === "same";
    }
    if (kept) on("keep", 0);
    else {
      on("erase", 0);
      await loader.eraseFlash();
    }
    const total = parts.reduce((n, p) => n + p.data.length, 0);
    let before = 0;
    let last = -1;
    await loader.writeFlash({
      fileArray: parts.map((p) => ({ data: p.data, address: p.address })),
      flashSize: "keep", flashMode: "keep", flashFreq: "keep",
      eraseAll: false, compress: true,
      reportProgress: (i, written, size) => {
        if (i !== last) {
          before = parts.slice(0, i).reduce((n, p) => n + p.data.length, 0);
          last = i;
        }
        on("write", (before + (size ? (written / size) * parts[i].data.length : 0)) / total);
      },
    });
    on("reset", 1);
    // RTS low then high, DTR high: a reset into the new firmware, on a UART bridge and on the
    // chip's own USB (USB Serial/JTAG) alike
    await transport.setDTR(false);
    await transport.setRTS(true);
    await sleep(200);
    await transport.setRTS(false);
    await sleep(200);
    return { chip: found, kept };
  } finally {
    await transport.disconnect().catch(() => {});
  }
}

// The same port again after a reset: the chip's own USB goes away and comes back as a new
// port object, a UART bridge stays. Found by its USB ids among the ports the page may use;
// null when it does not come back within ms. A port the page has open (another board on the same
// kind of USB, such as the S3 next to the C6 radio) is never the one.
export async function portAgain(port, ms = 8000) {
  const info = port.getInfo?.() || {};
  const end = Date.now() + ms;
  await sleep(1200);
  while (Date.now() < end) {
    const ports = await navigator.serial.getPorts();
    if (ports.includes(port)) return port;
    const same = ports.filter((p) => {
      if (p.readable) return false;
      const i = p.getInfo?.() || {};
      return i.usbVendorId === info.usbVendorId && i.usbProductId === info.usbProductId;
    });
    if (same.length === 1) return same[0];
    await sleep(500);
  }
  return null;
}
