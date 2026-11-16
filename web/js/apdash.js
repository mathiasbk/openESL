// apdash.js - the dashboard as the AP serves it at /dashboard (ap/main/webui.py packs it into
// the firmware): the same js/dashboard.js, calling the AP's own /api/ with its token. The AP
// has no firmware files of its own, so the update choices come from the published web page
// (GitHub Pages allows that from any origin). A layout file the designer on the web page hands
// over (Open my AP in its Send dialog) shows up at the top (js/apreceive.js).
//
// The previews' renderer: a board with the Design tab packs its own (/ui/js/renderer.js and
// /ui/wasm/, the AP's own code and version, no internet needed). One without it (the C6) loads
// the published page's in a sandboxed frame, which cannot reach the token (js/renderframe.js).
import { initDashboard } from "./dashboard.js";
import { initReceive } from "./apreceive.js";
import { SITE } from "./site.js";

const ownRenderer = !!document.getElementById("tab-design");
initDashboard({ sameOrigin: true, firmwareBase: SITE, rendererBase: ownRenderer ? "" : SITE });
initReceive();
