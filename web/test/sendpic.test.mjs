// "A picture" in the Send form (js/sendpic.js): placing, toning, dithering, packing, sending.
import { test } from "node:test";
import assert from "node:assert/strict";
import { placement, dragTo, turnedSize, tone, quantize, packRaw, picturePlan, sendPictures, inks, WHITE, BLACK, RED } from "../js/sendpic.js";
import { decodeTagImage } from "../js/dashdata.js";
import { MODELS } from "../js/models.js";

const rgbaOf = (w, h, f) => {
  const a = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) a.set([...f(x, y), 255], (y * w + x) * 4);
  return a;
};

test("fill covers the screen and crops, fit shows all of it", () => {
  // a square picture on the 2.9" screen
  assert.deepEqual(placement(100, 100, 296, 128, { fit: "fill" }), { dx: 0, dy: -84, dw: 296, dh: 296 });
  assert.deepEqual(placement(100, 100, 296, 128, { fit: "fit" }), { dx: 84, dy: 0, dw: 128, dh: 128 });
  assert.deepEqual(placement(100, 100, 296, 128, { fit: "stretch" }), { dx: 0, dy: 0, dw: 296, dh: 128 });
  // moved to the top, and zoomed twice
  assert.deepEqual(placement(100, 100, 296, 128, { fit: "fill", y: 0 }), { dx: 0, dy: 0, dw: 296, dh: 296 });
  const z = placement(100, 100, 296, 128, { fit: "fill", zoom: 2 });
  assert.equal(z.dw, 592);
  assert.equal(z.dx, -148);
  // out-of-range values are held in
  assert.deepEqual(placement(100, 100, 296, 128, { fit: "fill", y: 7, zoom: 99 }).dw, 296 * 4);
});

test("dragging moves within what sticks out, and not at all when nothing does", () => {
  // 296 drawn 592 wide: 296 px to spare, so dragging 148 px right moves from the middle to the left edge
  assert.equal(dragTo(0.5, 148, 296, 592), 0);
  assert.equal(dragTo(0.5, -74, 296, 592), 0.75);
  assert.equal(dragTo(0.5, -1000, 296, 592), 1);
  assert.equal(dragTo(0.3, 50, 296, 296), 0.3);
  // fit leaves room beside the picture: dragging right moves it right
  assert.equal(dragTo(0.5, 42, 296, 128), 0.75);
});

test("turning swaps the sides", () => {
  assert.deepEqual(turnedSize(40, 30, 0), [40, 30]);
  assert.deepEqual(turnedSize(40, 30, 90), [30, 40]);
  assert.deepEqual(turnedSize(40, 30, 180), [40, 30]);
});

test("tone: transparency over white, brightness and contrast", () => {
  const a = new Uint8ClampedArray([0, 0, 0, 0, 100, 100, 100, 255, 200, 50, 128, 255]);
  tone(a);
  assert.deepEqual([...a], [255, 255, 255, 255, 100, 100, 100, 255, 200, 50, 128, 255]);
  const b = tone(new Uint8ClampedArray([100, 128, 160, 255]), { brightness: 50 });
  assert.deepEqual([...b], [164, 192, 224, 255]);
  const c = tone(new Uint8ClampedArray([100, 128, 160, 255]), { contrast: 100 });
  assert.deepEqual([...c], [72, 128, 192, 255]);
  // transparent stays white when darkened; half transparent black is toned first, then over white
  const d = tone(new Uint8ClampedArray([0, 0, 0, 0, 0, 0, 0, 128]), { brightness: -100 });
  assert.deepEqual([...d], [255, 255, 255, 255, 127, 127, 127, 255]);
});

test("nearest colour: black and white by brightness, red only where the model has it", () => {
  const w = 4, h = 1;
  const rgba = rgbaOf(w, h, (x) => [[10, 10, 10], [240, 240, 240], [230, 20, 20], [30, 30, 220]][x]);
  assert.deepEqual([...quantize(rgba, w, h, { dither: false })], [BLACK, WHITE, BLACK, BLACK]);
  assert.deepEqual([...quantize(rgba, w, h, { red: true, dither: false })], [BLACK, WHITE, RED, BLACK]);
  assert.deepEqual(inks(["black", "white"]), [WHITE, BLACK]);
  assert.deepEqual(inks(["black", "white", "red"]), [WHITE, BLACK, RED]);
});

test("dithering keeps the grey: about half the pixels of 50 % grey are black", () => {
  const w = 64, h = 64;
  const px = quantize(rgbaOf(w, h, () => [128, 128, 128]), w, h);
  const black = px.reduce((n, v) => n + (v === BLACK), 0);
  assert.ok(Math.abs(black / (w * h) - 0.5) < 0.03, `${black} of ${w * h} black`);
  // without dithering the same grey is one colour
  assert.equal(new Set(quantize(rgbaOf(w, h, () => [128, 128, 128]), w, h, { dither: false })).size, 1);
  // pure black and white stay as they are
  const bw = rgbaOf(w, h, (x) => (x < 32 ? [0, 0, 0] : [255, 255, 255]));
  assert.ok(quantize(bw, w, h).every((v, i) => v === (i % w < 32 ? BLACK : WHITE)));
});

test("the raw image decodes back to the same pixels (both models, and red)", () => {
  for (const [name, m] of Object.entries(MODELS)) {
    const { width: w, height: h } = m;
    const px = Uint8Array.from({ length: w * h }, (_, i) => ((i * 7919) >> 3) % 3 === 0 ? BLACK : WHITE);
    const raw = packRaw(px, w, h);
    assert.equal(raw.length, w * Math.ceil(h / 8), name);
    const back = decodeTagImage({ width: w, height: h, red: false, data: Buffer.from(raw).toString("base64") });
    assert.deepEqual(back.pixels, px, name);
  }
  // the 2.9": 4736 bytes, as the API wants it
  assert.equal(packRaw(new Uint8Array(296 * 128), 296, 128).length, 4736);
  const px = Uint8Array.from({ length: 30 * 13 }, (_, i) => i % 3);
  const raw = packRaw(px, 30, 13, true);
  assert.equal(raw.length, 2 * 30 * 2);
  assert.deepEqual(decodeTagImage({ width: 30, height: 13, red: true, data: Buffer.from(raw).toString("base64") }).pixels, px);
  // top-left pixel is bit 7 of byte 0; the pixel below it bit 6
  const one = new Uint8Array(4 * 9);
  one[0] = BLACK;
  one[4] = BLACK;
  assert.deepEqual([...packRaw(one, 4, 9).slice(0, 2)], [0xc0, 0]);
});

const tags = [
  { mac: "00000123456789A8", model: "1.54" },
  { mac: "0000020542663B18", model: "2.9" },
  { mac: "0000020542663B19", model: null },
  { mac: "0000020542663B20", model: "7.5" },
];

test("the plan says which ticked tags can take it", () => {
  const plan = picturePlan(tags, MODELS);
  assert.deepEqual(plan.map((p) => p.ok), [true, true, false, false]);
  assert.match(plan[1].text, /296 x 128/);
  assert.match(plan[2].text, /not polled/);
  assert.match(plan[3].text, /does not know/);
});

const fakeAp = (fail = null) => {
  const calls = [];
  return {
    calls,
    async send(method, path, body) {
      calls.push([method, path, body.length]);
      const mac = path.split("/")[3];
      if (mac === fail) return { status: 507, body: new TextEncoder().encode('{"error":"no room to save the picture"}') };
      return { status: 200, body: new TextEncoder().encode(JSON.stringify({ mac, content_id: 5, changed: true, pending: true })) };
    },
  };
};

test("each tag gets its model's image, one after the other", async () => {
  const ap = fakeAp();
  const images = { "1.54": new Uint8Array(5000), "2.9": new Uint8Array(4736) };
  const seen = [];
  const replies = await sendPictures(ap, picturePlan(tags, MODELS), images, (d, n) => seen.push([d, n]));
  assert.deepEqual(ap.calls, [
    ["PUT", "/api/tags/00000123456789A8/image", 5000],
    ["PUT", "/api/tags/0000020542663B18/image", 4736],
  ]);
  assert.deepEqual(seen, [[1, 2], [2, 2]]);
  assert.equal(replies[1].mac, "0000020542663B18");
});

test("a refusal stops the sending with the AP's error and how many went", async () => {
  const ap = fakeAp("0000020542663B18");
  const images = { "1.54": new Uint8Array(5000), "2.9": new Uint8Array(4736) };
  await assert.rejects(sendPictures(ap, picturePlan(tags, MODELS), images), (e) => {
    assert.equal(e.sent, 1);
    assert.match(e.message, /0000020542663B18: 507: no room/);
    return true;
  });
});
