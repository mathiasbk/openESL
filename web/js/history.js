// history.js - undo and redo for the designer. Keeps snapshots (strings) of the document; the
// editor pushes one after each finished change. Pushing the same snapshot twice does nothing,
// so selecting things or redrawing never adds a step.

export class History {
  constructor(limit = 200) {
    this.limit = limit;
    this.reset(null);
  }

  // Start over from one state, e.g. when a model is loaded.
  reset(snap, extra) {
    this.stack = snap === null ? [] : [{ snap, extra }];
    this.i = this.stack.length - 1;
  }

  // Add a state after the current one. Anything that was undone is dropped. Returns false
  // when nothing changed.
  push(snap, extra) {
    if (this.i >= 0 && this.stack[this.i].snap === snap) {
      this.stack[this.i].extra = extra;
      return false;
    }
    this.stack.splice(this.i + 1);
    this.stack.push({ snap, extra });
    if (this.stack.length > this.limit) this.stack.shift();
    this.i = this.stack.length - 1;
    return true;
  }

  get canUndo() { return this.i > 0; }
  get canRedo() { return this.i < this.stack.length - 1; }

  // The state to go back to, or null.
  undo() { return this.canUndo ? this.stack[--this.i] : null; }
  redo() { return this.canRedo ? this.stack[++this.i] : null; }
}
