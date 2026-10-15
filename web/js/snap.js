// snap.js - snapping while dragging in the designer. A box being moved snaps its left edge,
// centre or right edge (and top, middle, bottom) to the screen's edges and centre and to the
// edges and centres of the other boxes, when within `near` pixels. Returns the snapped
// position and the guide lines to show.

function targets(others, size) {
  const t = [0, Math.floor(size.w / 2), size.w];
  const u = [0, Math.floor(size.h / 2), size.h];
  for (const o of others) {
    t.push(o.x, o.x + Math.floor(o.w / 2), o.x + o.w);
    u.push(o.y, o.y + Math.floor(o.h / 2), o.y + o.h);
  }
  return { x: t, y: u };
}

// The best shift (smallest distance) that puts one of `points` on one of `lines`, or null.
function best(points, lines, near) {
  let found = null;
  for (const p of points) {
    for (const l of lines) {
      const d = l - p;
      if (Math.abs(d) <= near && (found === null || Math.abs(d) < Math.abs(found.d))) found = { d, at: l };
    }
  }
  return found;
}

// Moving: box {x, y, w, h} at its unsnapped position. size {w, h} is the screen.
export function snapMove(box, others, size, near) {
  const t = targets(others, size);
  const out = { x: box.x, y: box.y, guides: [] };
  const sx = best([box.x, box.x + Math.floor(box.w / 2), box.x + box.w], t.x, near);
  const sy = best([box.y, box.y + Math.floor(box.h / 2), box.y + box.h], t.y, near);
  if (sx) {
    out.x += sx.d;
    out.guides.push({ axis: "x", at: sx.at });
  }
  if (sy) {
    out.y += sy.d;
    out.guides.push({ axis: "y", at: sy.at });
  }
  return out;
}

// Resizing by one of the eight handles ("n", "ne", "e", "se", "s", "sw", "w", "nw"): start is
// the box when the drag began, dx/dy how far the pointer moved. The edges the handle moves
// follow the pointer, snap like a moved box does, and stay on the screen; the box keeps at
// least 1 px. Returns { x, y, w, h, guides }.
export function resizeBox(start, handle, dx, dy, others, size, near) {
  const t = targets(others, size);
  let l = start.x, r = start.x + start.w, top = start.y, b = start.y + start.h;
  const guides = [];
  const edge = (v, lines, axis) => {
    const s = best([v], lines, near);
    if (!s) return v;
    guides.push({ axis, at: s.at });
    return v + s.d;
  };
  if (handle.includes("w")) l = Math.min(Math.max(0, edge(l + dx, t.x, "x")), r - 1);
  if (handle.includes("e")) r = Math.max(Math.min(size.w, edge(r + dx, t.x, "x")), l + 1);
  if (handle.includes("n")) top = Math.min(Math.max(0, edge(top + dy, t.y, "y")), b - 1);
  if (handle.includes("s")) b = Math.max(Math.min(size.h, edge(b + dy, t.y, "y")), top + 1);
  return { x: l, y: top, w: r - l, h: b - top, guides };
}
