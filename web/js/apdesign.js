// apdesign.js - the Design tab as the AP serves it (ap/main/webui.py packs it into the firmware
// of a board with room for it): the same js/editor.js, which opens and saves the layout files
// stored on this AP through its own /api/. The renderer (WebAssembly) is not in the firmware;
// it comes from the published web page, as the dashboard's screen previews do.
import { initEditor } from "./editor.js";

initEditor({ sameOrigin: true, rendererBase: new URL("https://mathiasbk.github.io/openESL/wasm/") }).catch((e) => {
  const s = document.getElementById("status");
  s.textContent = "Could not start the renderer: " + e.message;
  s.className = "status error";
});
