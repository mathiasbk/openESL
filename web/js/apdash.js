// apdash.js - the dashboard as the AP serves it at /dashboard (ap/main/webui.py packs it into
// the firmware): the same js/dashboard.js, calling the AP's own /api/ with its token.
import { initDashboard } from "./dashboard.js";

initDashboard({ sameOrigin: true });
