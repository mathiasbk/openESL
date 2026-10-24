// apdesign.js - the Design tab as the AP serves it (ap/main/webui.py packs it into the firmware
// of a board with room for it): the same js/editor.js, which opens and saves the layout files
// stored on this AP through its own /api/. The renderer (WebAssembly) is packed too, at
// /ui/wasm/ (renderer.js's default), built from the same sources as the firmware, so the
// preview and the checks match what this AP draws and need no internet.
import { initEditor } from "./editor.js";

initEditor({ sameOrigin: true }).catch((e) => {
  const s = document.getElementById("status");
  s.textContent = "Could not start the renderer: " + e.message;
  s.className = "status error";
});
