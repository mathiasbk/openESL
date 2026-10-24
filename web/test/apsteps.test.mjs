// The Flash AP tab's plan for each AP setup (js/apsteps.js): cards, the parts of the flash and
// connect steps, the partition table check and the radio over WiFi's console.
import { test } from "node:test";
import assert from "node:assert/strict";
import { cards, plan, radiosFor, compareTables, radioCommands, radioLine, isNativeUsb, twoPorts } from "../js/apsteps.js";

const info = {
  ap_boards: [
    { id: "c6-supermini", chip_family: "ESP32-C6", target: "esp32c6", status: "supported", manifest: "ap-manifest.json", radio: "native", radio_fw: [] },
    { id: "c6-supermini-lcd", chip_family: "ESP32-C6", target: "esp32c6", display: true, status: "supported", manifest: "ap-lcd-manifest.json", radio: "native", radio_fw: [] },
    { id: "s3-n16r8-c6", chip_family: "ESP32-S3", target: "esp32s3", status: "experimental", manifest: "ap-s3-n16r8-manifest.json", radio: "uart", radio_fw: ["uart-c6", "uart-c6-lcd"] },
    { id: "s3-n16r8-netradio", chip_family: "ESP32-S3", target: "esp32s3", status: "experimental", manifest: "ap-s3-n16r8-netradio-manifest.json", radio: "net", radio_fw: ["net-c6"] },
    { id: "c5-wroom-1u-n16r8", chip_family: "ESP32-C5", target: "esp32c5", status: "experimental", manifest: "ap-c5-n16r8-manifest.json", radio: "native", radio_fw: [] },
  ],
  radios: [
    { key: "uart-c6", chip_family: "ESP32-C6", manifest: "radio-uart-c6-manifest.json" },
    { key: "uart-c6-lcd", chip_family: "ESP32-C6", display: true, manifest: "radio-uart-c6-lcd-manifest.json" },
    { key: "net-c6", chip_family: "ESP32-C6", manifest: "radio-net-c6-manifest.json" },
  ],
};

test("a card for every board, with its title and chips", () => {
  const c = cards(info);
  assert.deepEqual(c.map((x) => x.title), ["ESP32-C6", "C6 with screen", "S3 + C6 on cable", "S3 + C6 over WiFi", "ESP32-C5"]);
  assert.deepEqual(c[2].chips, ["S3", "C6 radio"]);
  assert.deepEqual(c.map((x) => x.pic), ["single", "screen", "cable", "wifi", "single"]);
  assert.ok(c.every((x) => x.ready));
});

test("a board with a C6 radio is not ready without its radio image", () => {
  const c = cards({ ...info, radios: [] });
  assert.deepEqual(c.map((x) => x.ready), [true, true, false, false, true]);
});

test("an info.json from before ap_boards still gives the C6 boards", () => {
  assert.deepEqual(cards({ ap: true, ap_lcd: true }).map((x) => x.title), ["ESP32-C6", "C6 with screen"]);
  assert.deepEqual(cards(null), []);
});

test("one board: flash it, then its WiFi", () => {
  const p = plan(info.ap_boards[0], null);
  assert.deepEqual(p.flash, [{ kind: "flash", role: "ap", chip: "ESP32-C6", manifest: "ap-manifest.json", title: "ESP32-C6", usbPort: false }]);
  assert.deepEqual(p.connect, [{ kind: "wifi" }]);
});

test("S3 + C6 on a cable: the C6 radio picked first, then the S3 on its USB port, then the wires", () => {
  const b = info.ap_boards[2];
  const r = radiosFor(b, info);
  assert.deepEqual(r.map((x) => x.key), ["uart-c6", "uart-c6-lcd"]);
  const p = plan(b, r[1]);
  assert.deepEqual(p.flash.map((x) => x.kind + ":" + (x.manifest || "")), ["flash:radio-uart-c6-lcd-manifest.json", "flash:ap-s3-n16r8-manifest.json", "wire:"]);
  assert.equal(p.flash[1].usbPort, true);
  assert.deepEqual(p.connect, [{ kind: "wifi" }]);
});

test("S3 + C6 over WiFi: the radio's network and the AP's address after the S3's WiFi", () => {
  const b = info.ap_boards[3];
  const p = plan(b, radiosFor(b, info)[0]);
  assert.deepEqual(p.flash.map((x) => x.role), ["radio", "ap"]);
  assert.deepEqual(p.connect.map((x) => x.kind), ["wifi", "join"]);
});

test("the WiFi step needs the chip's own USB on boards with two ports", () => {
  assert.equal(twoPorts(info.ap_boards[2]), true);
  assert.equal(twoPorts(info.ap_boards[4]), true);
  assert.equal(twoPorts(info.ap_boards[0]), false);
  assert.equal(isNativeUsb({ usbVendorId: 0x303a }), true);
  assert.equal(isNativeUsb({ usbVendorId: 0x0403 }), false);   // FT232R
});

function table(entries) {
  const t = new Uint8Array(0xc00).fill(0xff);
  entries.forEach(([type, sub, off, size, name], i) => {
    const e = new Uint8Array(32);
    const v = new DataView(e.buffer);
    e.set([0xaa, 0x50, type, sub]);
    v.setUint32(4, off, true);
    v.setUint32(8, size, true);
    e.set(new TextEncoder().encode(name), 12);
    t.set(e, i * 32);
  });
  return t;
}

test("the partition table on the board: same, blank or different", () => {
  const ours = table([[1, 2, 0x9000, 0x75000, "nvs"], [0, 0x10, 0x90000, 0x200000, "ota_0"]]);
  assert.equal(compareTables(ours.slice(0, 0x400), ours), "same");
  assert.equal(compareTables(new Uint8Array(0x400).fill(0xff), ours), "blank");
  const old = table([[1, 2, 0x9000, 0x6000, "nvs"], [0, 0, 0x10000, 0x100000, "factory"]]);
  assert.equal(compareTables(old.slice(0, 0x400), ours), "different");
  // a partition added after the others (tagfw) keeps NVS where it was
  const more = table([[1, 2, 0x9000, 0x75000, "nvs"], [0, 0x10, 0x90000, 0x200000, "ota_0"], [1, 0x40, 0x290000, 0x70000, "tagfw"]]);
  assert.equal(compareTables(ours.slice(0, 0x400), more), "same");
  // other firmware without a table there
  assert.equal(compareTables(new Uint8Array(0x400).fill(0), ours), "different");
});

test("the radio over WiFi: its commands and what its log says", () => {
  assert.deepEqual(radioCommands({ ssid: "My net", pass: "a b c", host: "192.168.3.112" }),
    ["ssid My net", "pass a b c", "host 192.168.3.112", "reboot"]);
  assert.deepEqual(radioCommands({ ssid: "x", pass: "y" }).at(2), "host ");
  assert.deepEqual(radioLine("I (2100) radio: connected to the AP at 192.168.3.112:4553"), { state: "joined", ip: "192.168.3.112" });
  assert.equal(radioLine("I (2100) radio: connected to the AP at 192.168.3.112:4553, but HELLO failed").state, "no-ap");
  assert.equal(radioLine("W (9) radio: no AP found on mDNS (_openesl-ap._tcp)").state, "no-ap");
  assert.equal(radioLine("W (9) radio: AP 192.168.3.9:4553 does not answer").state, "no-ap");
  assert.deepEqual(radioLine("I (9) radio: link: up, WiFi rssi -78, connects 1 syncs 2"), { state: "joined", rssi: -78 });
  assert.equal(radioLine("W (9) radio: no WiFi set: type ssid <name>, then pass <password>").state, "no-wifi");
  assert.equal(radioLine("W (9) radio: WiFi lost (reason 15), joining again").state, "bad-pass");
  assert.equal(radioLine("W (9) radio: WiFi lost (reason 8), joining again"), null);
  assert.equal(radioLine("I (9) radio: heap: free 1000"), null);
});
