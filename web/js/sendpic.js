// sendpic.js - "A picture" in the Send form on the Dashboard: a PNG or JPG of any size is
// cropped or fitted to each ticked tag's screen, turned into the tag's colours in the browser
// (Floyd-Steinberg error diffusion, or the nearest colour), and sent as the raw screen image
// (PUT /api/tags/{mac}/image, the format GET gives back), so the preview is exactly what the
// tag shows and the AP has nothing left to decide. The drawing onto a canvas is in dashboard.js.

export const FITS = ["fill", "fit", "stretch"];
export const TURNS = [0, 90, 180, 270];
export const ZOOM_MAX = 4;

// Pixel values, as the renderer and decodeTagImage() give them
export const WHITE = 0, BLACK = 1, RED = 2;
const INK = [[255, 255, 255], [0, 0, 0], [255, 0, 0]];

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

// The picture's size once it is turned (turn: degrees clockwise)
export const turnedSize = (iw, ih, turn = 0) => (turn % 180 ? [ih, iw] : [iw, ih]);

// Where a picture of iw x ih (already turned) goes on a w x h screen: { dx, dy, dw, dh }.
// fill covers the screen and crops what is left over, fit shows all of it with white beside
// it, stretch fills the screen and changes the shape. zoom (1..ZOOM_MAX) enlarges fill and
// fit; x and y (0..1) move the picture within what is left over or cut off: 0 at the left
// or top edge, 0.5 centred, 1 at the right or bottom edge.
export function placement(iw, ih, w, h, { fit = "fill", zoom = 1, x = 0.5, y = 0.5 } = {}) {
  if (!(iw > 0 && ih > 0)) return { dx: 0, dy: 0, dw: w, dh: h };
  if (fit === "stretch") return { dx: 0, dy: 0, dw: w, dh: h };
  const base = fit === "fit" ? Math.min(w / iw, h / ih) : Math.max(w / iw, h / ih);
  const s = base * clamp(Number(zoom) || 1, 1, ZOOM_MAX);
  const dw = iw * s, dh = ih * s;
  return { dx: (w - dw) * clamp(x, 0, 1) + 0, dy: (h - dh) * clamp(y, 0, 1) + 0, dw, dh }; // + 0: never -0
}

// x or y after the picture has been dragged by d screen pixels; the same when it neither
// sticks out nor leaves room on that side
export function dragTo(pos, d, screen, drawn) {
  const room = screen - drawn;
  return Math.abs(room) < 0.5 ? pos : clamp(pos + d / room, 0, 1);
}

// Brightness and contrast (-100..100 each) on RGBA pixels, in place, then over white: what
// is transparent (the picture's own, or the room beside it) stays white whatever the tone
export function tone(rgba, { brightness = 0, contrast = 0 } = {}) {
  const b = clamp(Number(brightness) || 0, -100, 100) * 1.28;
  const k = 1 + clamp(Number(contrast) || 0, -100, 100) / 100;
  for (let i = 0; i < rgba.length; i += 4) {
    const a = rgba[i + 3] / 255;
    for (let c = 0; c < 3; c++) {
      const v = clamp((rgba[i + c] - 128) * k + 128 + b, 0, 255);
      rgba[i + c] = Math.round(v * a + 255 * (1 - a));
    }
    rgba[i + 3] = 255;
  }
  return rgba;
}

// The colours of a model's screen (models.js "colors") -> the pixel values it takes
export const inks = (colors = []) => (colors.includes("red") ? [WHITE, BLACK, RED] : [WHITE, BLACK]);

// RGBA pixels (w x h, after tone()) -> one pixel value per pixel (WHITE, BLACK or RED).
// Without red the picture becomes grey first, so black and white follow its brightness.
// dither: Floyd-Steinberg error diffusion (7/16 right, 3/16, 5/16 and 1/16 below), else
// each pixel takes the nearest colour (best for drawings, text and logos).
export function quantize(rgba, w, h, { red = false, dither = true } = {}) {
  const out = new Uint8Array(w * h);
  const ks = red ? [WHITE, BLACK, RED] : [WHITE, BLACK];
  const cur = new Float32Array((w + 2) * 3), next = new Float32Array((w + 2) * 3);
  for (let y = 0; y < h; y++) {
    next.fill(0);
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      let v = [rgba[i], rgba[i + 1], rgba[i + 2]];
      if (!red) {
        const g = 0.299 * v[0] + 0.587 * v[1] + 0.114 * v[2];
        v = [g, g, g];
      }
      if (dither) for (let c = 0; c < 3; c++) v[c] = clamp(v[c] + cur[(x + 1) * 3 + c], 0, 255);
      let best = ks[0], bestD = Infinity;
      for (const k of ks) {
        const d = (v[0] - INK[k][0]) ** 2 + (v[1] - INK[k][1]) ** 2 + (v[2] - INK[k][2]) ** 2;
        if (d < bestD) [best, bestD] = [k, d];
      }
      out[y * w + x] = best;
      if (!dither) continue;
      for (let c = 0; c < 3; c++) {
        const e = v[c] - INK[best][c];
        cur[(x + 2) * 3 + c] += (e * 7) / 16;
        next[x * 3 + c] += (e * 3) / 16;
        next[(x + 1) * 3 + c] += (e * 5) / 16;
        next[(x + 2) * 3 + c] += e / 16;
      }
    }
    cur.set(next);
  }
  return out;
}

// Pixel values (w x h, row by row) -> the raw screen image the API takes (protokoll.md, IMG
// fmt 0): column by column, (h + 7) / 8 bytes each, bit 7 of a column's first byte the top
// row, 1 = ink; the black plane, then on a model with red the red plane
export function packRaw(pixels, w, h, red = false) {
  const col = Math.ceil(h / 8);
  const plane = col * w;
  const out = new Uint8Array(plane * (red ? 2 : 1));
  for (let x = 0; x < w; x++) {
    for (let y = 0; y < h; y++) {
      const v = pixels[y * w + x];
      const at = x * col + (y >> 3), bit = 0x80 >> (y & 7);
      if (v === BLACK) out[at] |= bit;
      else if (v === RED && red) out[plane + at] |= bit;
    }
  }
  return out;
}

// What each ticked tag (rows of GET /api/tags) gets: [{ mac, model, text, ok }]. models: the
// tag models this page knows (models.js MODELS)
export function picturePlan(tags, models) {
  return tags.map((t) => {
    const m = models[t.model];
    if (!t.model) return { mac: t.mac, model: null, ok: false, text: "has not polled yet, so its screen is not known" };
    if (!m) return { mac: t.mac, model: t.model, ok: false, text: `a ${t.model}" tag, which this page does not know` };
    return { mac: t.mac, model: t.model, ok: true, text: `the picture, ${m.width} x ${m.height}` };
  });
}

// Sends each tag its model's image (images: { model: Uint8Array }) one after the other;
// client: ApClient or SerialAp (send() gives { status, body }). progress(done, total) after
// each. Returns the AP's replies; throws on the first refusal, with the AP's error and
// how many went before it in err.sent.
export async function sendPictures(client, plan, images, progress = () => {}) {
  const todo = plan.filter((p) => p.ok && images[p.model]);
  const replies = [];
  for (const p of todo) {
    const r = await client.send("PUT", `/api/tags/${encodeURIComponent(p.mac)}/image`, images[p.model]);
    let json = null;
    try {
      json = JSON.parse(new TextDecoder().decode(r.body));
    } catch { /* empty or not JSON */ }
    if (r.status >= 400) {
      const e = new Error(`${p.mac}: ${r.status}: ${json?.error || "error"}`);
      e.sent = replies.length;
      throw e;
    }
    replies.push(json || { mac: p.mac });
    progress(replies.length, todo.length);
  }
  return replies;
}
