// Undo and redo in the designer (js/history.js).
import { test } from "node:test";
import assert from "node:assert/strict";
import { History } from "../js/history.js";

test("undo and redo walk the states", () => {
  const h = new History();
  h.reset("a");
  assert.equal(h.canUndo, false);
  h.push("b");
  h.push("c");
  assert.equal(h.undo().snap, "b");
  assert.equal(h.undo().snap, "a");
  assert.equal(h.undo(), null);
  assert.equal(h.redo().snap, "b");
  assert.equal(h.redo().snap, "c");
  assert.equal(h.redo(), null);
});

test("the same state twice is one step, but keeps the newest extra", () => {
  const h = new History();
  h.reset("a", 1);
  assert.equal(h.push("a", 2), false);
  assert.equal(h.canUndo, false);
  h.push("b");
  assert.equal(h.undo().extra, 2);
});

test("a change after undo drops the redo states", () => {
  const h = new History();
  h.reset("a");
  h.push("b");
  h.undo();
  h.push("x");
  assert.equal(h.canRedo, false);
  assert.equal(h.undo().snap, "a");
});

test("the oldest states fall off past the limit", () => {
  const h = new History(3);
  h.reset("a");
  for (const s of ["b", "c", "d"]) h.push(s);
  assert.equal(h.undo().snap, "c");
  assert.equal(h.undo().snap, "b");
  assert.equal(h.undo(), null);
});
