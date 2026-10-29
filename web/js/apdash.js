// apdash.js - the dashboard as the AP serves it at /dashboard (ap/main/webui.py packs it into
// the firmware): the same js/dashboard.js, calling the AP's own /api/ with its token. The AP
// has no firmware files of its own, so the update choices come from the published web page
// (GitHub Pages allows that from any origin). A layout file the designer on the web page hands
// over (Open my AP in its Send dialog) shows up at the top (js/apreceive.js).
import { initDashboard } from "./dashboard.js";
import { initReceive } from "./apreceive.js";

initDashboard({ sameOrigin: true, firmwareBase: "https://mathiasbk.github.io/openESL/" });
initReceive();
