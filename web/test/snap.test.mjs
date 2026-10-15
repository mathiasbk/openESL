// Snapping in the designer (js/snap.js).
import { test } from "node:test";
import assert from "node:assert/strict";
import { snapMove, resizeBox } from "../js/snap.js";

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

test("resizing by the bottom-right corner snaps the right and bottom edges", () => {
  const r = resizeBox({ x: 10, y: 10, w: 200, h: 100 }, "se", 84, 16, [], screen, 3);
  assert.deepEqual([r.x, r.y, r.w, r.h], [10, 10, 286, 118]);
});

test("the left handle moves only the left edge, and stops before the right edge", () => {
  const box = { x: 50, y: 20, w: 40, h: 30 };
  let r = resizeBox(box, "w", -20, 99, [], screen, 0);
  assert.deepEqual([r.x, r.y, r.w, r.h], [30, 20, 60, 30]);
  r = resizeBox(box, "w", 100, 0, [], screen, 0);
  assert.deepEqual([r.x, r.w], [89, 1]);
});

test("the top-left handle snaps to the screen corner and stays on the screen", () => {
  const r = resizeBox({ x: 5, y: 4, w: 40, h: 30 }, "nw", -3, -2, [], screen, 3);
  assert.deepEqual([r.x, r.y, r.w, r.h], [0, 0, 45, 34]);
  const off = resizeBox({ x: 5, y: 4, w: 40, h: 30 }, "nw", -50, -50, [], screen, 0);
  assert.deepEqual([off.x, off.y], [0, 0]);
});
