// site.js - where the web page is published (GitHub Pages). The one place a fork changes to
// use its own site: the AP's own page loads the renderer and the template library from it
// (js/apdash.js, js/catalog.js), the AP's page takes layout files only from its origin
// (js/handoff.js), and ap/main/webui.py reads SITE from this file for the links on the AP's
// page. Two copies are left where this file cannot be read: BRIDGE_ORIGINS in
// tools/esl-client/esl_client.py (it runs without the repository) and the link on the radio's
// page in ap/netradio/main/netradio_main.c; web/test/renderframe.test.mjs checks they agree.
export const SITE = "https://mathiasbk.github.io/openESL/";
export const SITE_ORIGIN = new URL(SITE).origin;
