// Snapping in the designer (js/snap.js).
import { test } from "node:test";
import assert from "node:assert/strict";
import { snapMove, snapResize } from "../js/snap.js";

const screen = { w: 296, h: 128 };

test("a box near the left edge snaps to it", () => {
  const r = snapMove({ x: 2, y: 50, w: 40, h: 20 }, [], screen, 3);
  assert.equal(r.x, 0);
  assert.deepEqual(r.guides, [{ axis: "x", at: 0 }]);
});

test("centres snap to the screen's centre", () => {
  const r = snapMove({ x: 127, y: 10, w: 40, h: 20 }, [], screen, 3);
  assert.equal(r.x + 20, 148);
});

test("edges snap to another box, and nothing far away moves", () => {
  const other = { x: 100, y: 40, w: 50, h: 30 };
  const r = snapMove({ x: 152, y: 90, w: 20, h: 10 }, [other], screen, 3);
  assert.equal(r.x, 150);
  assert.equal(r.y, 90);
  assert.equal(r.guides.length, 1);
});

test("the nearest line wins", () => {
  const r = snapMove({ x: 48, y: 0, w: 10, h: 10 }, [{ x: 51, y: 100, w: 30, h: 1 }, { x: 47, y: 100, w: 30, h: 1 }], screen, 3);
  assert.equal(r.x, 47);
});

test("resizing snaps the right and bottom edges", () => {
  const r = snapResize({ x: 10, y: 10, w: 284, h: 116 }, [], screen, 3);
  assert.deepEqual([r.w, r.h], [286, 118]);
});
