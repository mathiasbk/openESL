// limits.js - how much of the tag's drawing budget a layout uses, counted the way the AP's
// compiler counts it (esl_layout_compile.c): each text element needs as many lines of text
// as its largest font choice can fit (1..3, capped by "lines"), and each rect element and
// each inverted or highlighted text needs one rectangle. The limits themselves come from the
// renderer (renderer.limits), so they follow shared/proto.h.

const INVERTED = new Set(["invert", "highlight"]);

// One element: { runs, rects }.
export function elementUsage(e, limits) {
  if (e.type === "rect") return { runs: 0, rects: 1 };
  if (e.type !== "text") return { runs: 0, rects: 0 };
  let most = 1;
  for (const f of (e.fonts || []).slice(0, 3)) {
    const fh = limits.fontHeight[f];
    if (!fh) continue;
    let n = Math.floor(e.h / fh);
    if (e.lines && e.lines < n) n = e.lines;
    if (n > limits.linesPerText) n = limits.linesPerText;
    if (n > most) most = n;
  }
  return { runs: most, rects: INVERTED.has(e.color) ? 1 : 0 };
}

// A whole layout: { runs, rects, elements: [{ runs, rects }] }.
export function layoutUsage(layout, limits) {
  const elements = (layout?.elements || []).map((e) => elementUsage(e, limits));
  return {
    runs: elements.reduce((a, u) => a + u.runs, 0),
    rects: elements.reduce((a, u) => a + u.rects, 0),
    elements,
  };
}
