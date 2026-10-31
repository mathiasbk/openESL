// limits.js - how much of the AP's drawing budget a layout uses, counted the way the AP's
// compiler counts it (esl_layout_compile.c): each text element needs as many lines of text
// as its largest font choice can fit (1..3, capped by "lines"), and each rect element and
// each inverted or highlighted text needs one rectangle. The limits themselves come from the
// renderer (renderer.limits), so they follow shared/proto.h. A barcode needs neither, but a
// layout holds at most limits.barcodes of them, limits.qrs QR codes, limits.lines line elements
// and limits.icons icons.

const INVERTED = new Set(["invert", "highlight", "red_invert"]);

const NONE = { runs: 0, rects: 0, barcodes: 0, lines: 0, icons: 0, qrs: 0 };

// One element: { runs, rects, barcodes, lines, icons }.
export function elementUsage(e, limits) {
  if (e.type === "rect") return { ...NONE, rects: 1 };
  if (e.type === "barcode") return { ...NONE, barcodes: 1 };
  if (e.type === "line") return { ...NONE, lines: 1 };
  if (e.type === "icon") return { ...NONE, icons: 1 };
  if (e.type === "qr") return { ...NONE, qrs: 1 };
  if (e.type !== "text") return { ...NONE };
  let most = 1;
  // turned 90 or 270 degrees, the lines are stacked across w (ESL_LE_ROT)
  const across = e.rotate === 90 || e.rotate === 270 ? e.w : e.h;
  for (const f of (e.fonts || []).slice(0, 3)) {
    const fh = limits.fontHeight[f];
    if (!fh) continue;
    let n = Math.floor(across / fh);
    if (e.lines && e.lines < n) n = e.lines;
    if (n > limits.linesPerText) n = limits.linesPerText;
    if (n > most) most = n;
  }
  return { ...NONE, runs: most, rects: INVERTED.has(e.color) ? 1 : 0 };
}

// A whole layout: { runs, rects, barcodes, lines, icons, elements: [ the same per element ] }.
export function layoutUsage(layout, limits) {
  const elements = (layout?.elements || []).map((e) => elementUsage(e, limits));
  return {
    runs: elements.reduce((a, u) => a + u.runs, 0),
    rects: elements.reduce((a, u) => a + u.rects, 0),
    barcodes: elements.reduce((a, u) => a + u.barcodes, 0),
    lines: elements.reduce((a, u) => a + u.lines, 0),
    icons: elements.reduce((a, u) => a + u.icons, 0),
    qrs: elements.reduce((a, u) => a + u.qrs, 0),
    elements,
  };
}
