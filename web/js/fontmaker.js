// fontmaker.js - makes a font file for the AP (#177, "format": "esl-font", see
// ap/components/esl_core/include/esl_font.h) in the browser, without Python: from a font file
// the user picks (.ttf, .otf, .woff) or a font on their computer. The browser draws each
// character on a canvas, and the drawing is turned into the font's bitmap, so once it is on the
// AP the font is fixed pixels: the AP and the tags draw it the same whichever browser made it.
// tag/tools/gen_font.py --file makes the same kind of file with FreeType.
//
//   const file = makeFont({ name, family, weight, height, chars, size?, source? })
//       a font file (an object, PUT /api/fonts/{name} takes it) with file._made =
//       { size, clipped: [characters that reach out of the height], accents: [the same of
//       Latin-1's accented and other characters past 0xA0, which the built-in fonts cut too] }
//   await loadFontFile(file)           a .ttf/.otf/.woff File -> a family name to draw with
//   packFont({ name, height, baseline, codes, glyph, source })   the format part (tests)

export const ELLIPSIS = 0x7f;   // "…", which the AP draws when it shortens a text (tag/src/font.h)
const THRESHOLD = 110;          // alpha from which a pixel is black, as gen_font.py
export const MIN_H = 6, MAX_H = 64;
export const MAX_FILE = 24576;  // the AP takes a font file of at most this many bytes of JSON (ESL_API_MAX_BODY)

// Bytes of a font file as sent (its JSON) and as the AP keeps it (its blob, esl_font.h)
export function fontSize(file) {
  const bits = Math.floor((file.bitmap.length * 3) / 4) - (file.bitmap.match(/=*$/)[0].length);
  return { json: new TextEncoder().encode(JSON.stringify(file)).length,
           blob: 7 + file.name.length + (file.source || "").length + file.widths.length + bits };
}

// true when the browser has a font called family (document.fonts.check says true for any name)
export function hasFont(family) {
  const ctx = canvasFor(8, 8), s = "mmmmmmmmmmlli1WQ@", q = JSON.stringify(family);
  return ["monospace", "serif", "sans-serif"].some((fb) => {
    ctx.font = `40px ${fb}`;
    const w = ctx.measureText(s).width;
    ctx.font = `40px ${q}, ${fb}`;
    return ctx.measureText(s).width !== w;
  });
}

const range = (a, b) => Array.from({ length: b - a + 1 }, (_, i) => a + i);
export const CHARSETS = {
  latin1: [...range(0x20, 0x7e), ELLIPSIS, ...range(0xa0, 0xff)],
  price: [..." ,-.0123456789:"].map((c) => c.charCodeAt(0)),
};

// The character codes of a set: "latin1", "price" or the characters themselves (Latin-1 only);
// a space and "…" are always in it
export function codesFor(chars) {
  if (CHARSETS[chars]) return CHARSETS[chars];
  const codes = new Set([0x20, ELLIPSIS]);
  for (const ch of chars || "") {
    const c = ch === "…" ? ELLIPSIS : ch.codePointAt(0);
    if (c < 0x20 || c > 0xff || (c > 0x7e && c < 0xa0 && c !== ELLIPSIS))
      throw new Error(`"${ch}" is not a character the AP can draw (Latin-1 only)`);
    codes.add(c);
  }
  return [...codes].sort((a, b) => a - b);
}

export const charOf = (code) => (code === ELLIPSIS ? "…" : String.fromCharCode(code));

function base64(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

// The font file of a font: glyph(code) gives { width, ink(x, y) -> true for a black pixel }
export function packFont({ name, height, baseline, codes, glyph, source }) {
  if (!(height >= MIN_H && height <= MAX_H)) throw new Error(`the height must be ${MIN_H} to ${MAX_H} pixels`);
  const first = Math.min(...codes), last = Math.max(...codes);
  const want = new Set(codes), bpc = (height + 7) >> 3;
  const widths = [], bits = [];
  for (let c = first; c <= last; c++) {
    const g = want.has(c) ? glyph(c) : null;
    const w = g ? Math.max(0, Math.min(255, g.width | 0)) : 0;
    widths.push(w);
    for (let x = 0; x < w; x++) {
      const col = new Array(bpc).fill(0);
      for (let y = 0; y < height; y++) if (g.ink(x, y)) col[y >> 3] |= 0x80 >> (y & 7);
      bits.push(...col);
    }
  }
  if (!bits.length) throw new Error("the font has no characters");
  const file = { format: "esl-font", version: 1, name, height, baseline, first, last, widths, bitmap: base64(Uint8Array.from(bits)) };
  if (source) file.source = String(source).replace(/[^\x20-\x7e]/g, "?").slice(0, 63);
  return file;
}

// Loads a font file the user picked so the canvas can draw with it; gives its family name
let loaded = 0;
export async function loadFontFile(f) {
  const family = "openesl-font-" + ++loaded;
  const face = new FontFace(family, await f.arrayBuffer());
  try {
    await face.load();
  } catch {
    throw new Error(`"${f.name}" is not a font the browser can read (pick a .ttf, .otf or .woff file)`);
  }
  document.fonts.add(face);
  return family;
}

function canvasFor(w, h) {
  const c = document.createElement("canvas");
  c.width = w;
  c.height = h;
  return c.getContext("2d", { willReadFrequently: true });
}

const cssFont = (weight, size, family) =>
  `${weight} ${size}px ${/^[\w-]+$/.test(family) && !/^openesl-/.test(family) ? family : JSON.stringify(family)}`;

// Rows a character draws above and below the baseline at a size
function extent(ctx, ch) {
  const m = ctx.measureText(ch);
  return [Math.ceil(m.actualBoundingBoxAscent || 0), Math.ceil(m.actualBoundingBoxDescent || 0)];
}

// The size letters get when they must fit the height: the tallest of A-Z, digits, the
// descenders and the comma of a price (Å and other tall accents may reach out of it, as in the
// built-in fonts)
const FIT = "AHMZ0123456789gjpqy|(),";

function fitSize(ctx, weight, family, height, codes) {
  const sample = [...FIT].filter((ch) => codes.includes(ch.charCodeAt(0)));
  if (!sample.length) sample.push(...codes.filter((c) => c > 0x20).map(charOf));
  let lo = 4, hi = height * 2;
  const fits = (s) => {
    ctx.font = cssFont(weight, s, family);
    let up = 0, down = 0;
    for (const ch of sample) {
      const [a, d] = extent(ctx, ch);
      up = Math.max(up, a);
      down = Math.max(down, d);
    }
    return up + down <= height;
  };
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (fits(mid)) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

export function makeFont({ name, family, weight = "normal", height, chars = "latin1", size, source }) {
  height = Math.round(height);
  if (!(height >= MIN_H && height <= MAX_H)) throw new Error(`the height must be ${MIN_H} to ${MAX_H} pixels`);
  const codes = codesFor(chars);
  const probe = canvasFor(8, 8);
  size = Math.round(size) || fitSize(probe, weight, family, height, codes);
  probe.font = cssFont(weight, size, family);

  // Baseline: below the tallest of the fit characters, the rest of the height for descenders
  const sample = [...FIT].filter((ch) => codes.includes(ch.charCodeAt(0)));
  let up = 0, down = 0;
  for (const ch of sample.length ? sample : codes.filter((c) => c > 0x20).map(charOf)) {
    const [a, d] = extent(probe, ch);
    up = Math.max(up, a);
    down = Math.max(down, d);
  }
  const baseline = Math.max(1, Math.min(height, up + Math.max(0, (height - up - down) >> 1)));

  const clipped = [], accents = [];
  const glyph = (code) => {
    const ch = charOf(code);
    const width = Math.round(probe.measureText(ch).width);
    if (width <= 0) return null;
    const ctx = canvasFor(width, height);
    ctx.font = probe.font;
    ctx.textBaseline = "alphabetic";
    ctx.fillStyle = "#000";
    ctx.fillText(ch, 0, baseline);
    const px = ctx.getImageData(0, 0, width, height).data;
    const [a, d] = extent(probe, ch);
    if (ch.trim() && (a > baseline || d > height - baseline)) (code >= 0xa0 ? accents : clipped).push(ch);
    return { width, ink: (x, y) => px[(y * width + x) * 4 + 3] >= THRESHOLD };
  };
  const file = packFont({ name, height, baseline, codes, glyph: (c) => glyph(c) || { width: 0, ink: () => false }, source });
  Object.defineProperty(file, "_made", { value: { size, clipped, accents }, enumerable: false });
  return file;
}

// Draws text with a font file on a canvas, scale times bigger: what the AP draws, for a preview
export function previewFont(canvas, file, text, scale = 2) {
  const bytes = Uint8Array.from(atob(file.bitmap), (c) => c.charCodeAt(0));
  const bpc = (file.height + 7) >> 3;
  const offs = [];
  let o = 0;
  for (const w of file.widths) {
    offs.push(o);
    o += w * bpc;
  }
  const glyphs = [...text].map((ch) => {
    let c = ch === "…" ? ELLIPSIS : ch.codePointAt(0);
    if (c < file.first || c > file.last || !file.widths[c - file.first]) c = 0x3f;   // "?"
    const i = c - file.first;
    return i >= 0 && i < file.widths.length ? [file.widths[i], offs[i]] : [0, 0];
  });
  const w = Math.max(1, glyphs.reduce((n, [gw]) => n + gw, 0));
  canvas.width = w * scale;
  canvas.height = file.height * scale;
  const ctx = canvas.getContext("2d");
  ctx.fillStyle = "#fff";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = "#000";
  let x = 0;
  for (const [gw, off] of glyphs) {
    for (let cx = 0; cx < gw; cx++)
      for (let y = 0; y < file.height; y++)
        if (bytes[off + cx * bpc + (y >> 3)] & (0x80 >> (y & 7))) ctx.fillRect((x + cx) * scale, y * scale, scale, scale);
    x += gw;
  }
  return { width: w, ...fontSize(file) };
}
