// apdash.js - the dashboard as the AP serves it at /dashboard (ap/main/webui.py packs it into
// the firmware): the same js/dashboard.js, calling the AP's own /api/ with its token. The AP
// has no firmware files of its own, so the update choices come from the published web page
// (GitHub Pages allows that from any origin).
import { initDashboard } from "./dashboard.js";

initDashboard({ sameOrigin: true, firmwareBase: "https://mathiasbk.github.io/openESL/" });
