# Web page: label designer and flasher

A static page for GitHub Pages (or any web server): an overview, the label designer, a tag flasher, an AP flasher, the AP dashboard, a short setup page and the API documentation. It needs no server of its own.

- **Build and test:** `python web/build.py` and `node --test web/test/*.test.mjs` (below; needs Emscripten 3.1.74 and Node).
- **Start in the code:** `js/main.js` sets up the tabs; each tab has its own module, listed under its section below.
- **The preview** is drawn by the AP's own code built to WebAssembly (`wasm/web_api.c`), so it is exactly what a tag shows.

Web Serial (the flashers and the USB connection to an AP) needs Chrome or Edge on a computer, and a secure page (https, or http://localhost).

## Build and test

```bash
source /path/to/emsdk/emsdk_env.sh      # Emscripten 3.1.74
python web/build.py                     # renderer + page into web/dist/
python web/build.py --native            # native build of web_api.c, for the test
node --test web/test/*.test.mjs
cd web/dist && python -m http.server    # http://localhost:8000
```

Some modules are ports of Python tools. Keep them in step with the originals:

| Port | Original | Test |
|---|---|---|
| `js/zbs.js`, `js/tagflash.js` | `tools/flash-tui/zbs_proto.py`, `workflow.py` | `test/tagflash.test.mjs`, against a simulated ZBS_Flasher (`test/fake_flasher.mjs`, a port of `fake_flasher.py`) |
| `js/apserial.js` | `SerialClient` in `tools/esl-client/esl_client.py` | `test/apserial.test.mjs`, against a fake AP with log lines in the stream |

## The tabs

| Tab | What it does | Main files |
|---|---|---|
| [Overview](#overview) | The landing page | `index.html` |
| [Design](#design) | Draws and edits layout files | `js/editor.js`, `css/designer.css` |
| [Flash tag](#flash-tag) | Flashes tags with a D1 mini running ZBS_Flasher | `js/tagwizard.js`, `js/tagflash.js` |
| [Flash AP](#flash-ap) | Flashes an AP and sets its WiFi and token | `js/apwizard.js`, `js/apflash.js`, `js/apsteps.js` |
| [Dashboard](#dashboard) | The AP's state and every tag, and sends text, layouts and firmware | `js/dashboard.js`, `js/dashdata.js` |
| [Set up](#set-up) | WiFi and API token over USB, and links to the wiki | `js/wifisetup.js`, `js/apserial.js` |
| [API](#api) | The AP's HTTP API, interactive | `api.html` |

### Overview

What OpenESL is in one sentence with a photo, buttons to the AP flasher, the wiki and GitHub, the three steps, and a card for each tool. The details live in the wiki.

### Design

Draws and edits layout files ([docs/protocol.md](../docs/protocol.md) part 3) for every tag model in `js/models.js`. Its styles are in `css/designer.css`, so the AP does not carry them.

**Files**

- `js/editor.js`: the designer.
- `js/renderer.js`: the WebAssembly renderer for one tag model (see [The preview is the AP's own drawing](#the-preview-is-the-aps-own-drawing)).
- `js/limits.js`: how much of the AP's limits a layout uses, counted as the AP's compiler counts them (checked against the compiler by `test/limits.test.mjs`).
- `js/layoutinfo.js`: which layouts the AP has and how many tags show each. It asks each tag for its layout, since `GET /api/tags` leaves it out.
- `js/tplpicker.js` and `js/catalog.js`: the template library dialog, and what the picker and the build check share.
- `js/handoff.js` and `js/apreceive.js`: sending a layout to the AP's own page over WiFi.
- `templates.mjs`: checks the template catalog and writes it to `dist/templates/` during the build. The format: `wiki/labels/Label-templates.md`.

**Left column: the layouts**

- The layouts of the file, each with a small picture drawn with its own sample text, its number of fields and, once an AP is connected, whether the AP has it as it is: "On the AP" (with how many tags show it), "Changed, not sent" or "Only in this browser".
- The layouts the AP has that the file lacks. The built-in layouts are folded under them.
- "+ Empty" adds an empty layout. The selected layout has copy and delete.
- "+ From template" opens the template library: the catalog in `templates/` by category, with a search. Each template is drawn on the current model with its `_sample` text and shows its fields and what it uses. It is added only when the file still compiles with it, under a unique name, with its icons and sample text. With a template's layout selected, changing the model offers that template's variant for the other model.
- The catalog is read from `templates/index.json` next to the page, else from GitHub Pages. The AP's own designer reads GitHub Pages and falls back to its example layouts.
- Below, the elements of the selected layout, the one in front at the top. Drag to change the order.

**Middle: the tag**

- The tools over it: text, fixed text, box, line, icon, barcode, QR code.
- A bar by the selected element for how it looks: font, alignment, colour. Under "More": vertical alignment, lines, smaller fonts if the text does not fit, placing against an edge or in the middle, stretching, turning a line, turning text 90°, 180° or 270° (the box swaps width and height when it goes from across to up and down), and the drawing order.
- Boxes are dragged and resized on the preview and snap to the screen's edges and centre and to the other boxes (Alt turns it off).
- Undo and redo (Ctrl+Z, Ctrl+Shift+Z), and keys for the selected box: arrows, Tab, Esc, Delete, Ctrl+D to duplicate, Ctrl+C and Ctrl+V to copy a box to another layout.
- The status bar under the tag shows the fullest of the AP's limits. The full list (lines of text, boxes, file bytes, JSON bytes, fields and text bytes) opens from it. It also shows whether the AP accepts the file, the preview's options and the zoom (1× to 6× tag pixels, or fitted to the window).
- The limits are set per AP board (`"limits"` in `ap/boards/<board>/board.json`). Once "Send to your AP…" has connected, they are the ones that AP reports in `GET /api/layouts` (`renderer.setLimits`, `web_set_limit` in `wasm/web_api.c`). Otherwise they are the default board's (the ESP32-C6, the smallest), and the list says which.

**Right column**

The layout's name, its fields each with the sample text the preview shows (renamed, added and removed there), where the selected element is and what it shows, and the description.

**Send to your AP…**

A dialog that first asks how the AP is connected:

- **USB cable:** opens the AP's port (Web Serial, the connection the Dashboard tab shares, `js/apport.js`, through `PortClient` in `js/apclient.js`, no token). It lists the AP's tags of the model, uploads the layout file when the AP does not have this version, sends the sample text, then asks the AP whether the tag shows it.
- **WiFi:** opens the AP's own page (`http://<address>/dashboard?from=designer`) in a new tab and hands it the layout file, the content for a tag and the preview with `postMessage` (`js/handoff.js`, `test/handoff.test.mjs`). A page on https may not call the AP on http, but may open its page. The AP's page shows it at the top of its dashboard (`js/apreceive.js`, started by `js/apdash.js`) with how it differs from the AP's file, and stores it only when Save is pressed there, with the dashboard's token. It takes messages only from the published page or a page on localhost, and tells the designer how it went.
- **WiFi → Advanced:** connects to the bridge in `tools/esl-client` (`esl_client.py --url ... serve`) at the address the Dashboard tab uses, as USB does.

**File menu**

Saves the layout file for `PUT /api/layouts/{model}` and a label design (`esl-label`) for `tag/tools/label_design.py`, opens a file, shows the file as JSON, or puts the example back.

**On a narrow screen** (under 1000 px)

The columns stack: a layout picker over the tag, and under it the tabs Fields (name, fields and sample text, description), Elements (the list and the selected element) and Layouts (the list with its buttons), one at a time. Boxes are dragged and resized with a finger as with a mouse (pointer events), with larger handles on a touch screen.

### Flash tag

A guide in steps like Flash AP (`js/tagwizard.js`, styles in `css/apwizard.css`). It does the same steps and checks as `tools/flash-tui` (`js/zbs.js`, `js/tagflash.js`).

1. **Model:** the tag model from a photo of each tag (`img/`), and the firmware, from the page's release or from a file.
2. **Flasher:** the D1 mini running atc1441's ZBS_Flasher over Web Serial (drawn by `js/appics.js`).
3. **Flash:** the tag in the jig with its parts (MAC, backup, write, verify) and a row per tag flashed. "Keep going" waits for the next tag in the jig after each one.
4. **Done:** a summary with the backups to export.

- The order is MAC, backup before erase, MAC to the info page, write, verify. The info page is never erased.
- A tag with no MAC left (an earlier build wiped 0xFC06) gets a field, shown only then, for the barcode on its label or its MAC, and is flashed again with it.
- Backups are kept in the browser (IndexedDB, kept between visits until the site's data is cleared) with the barcode printed on the tag's label. It is filled in from the stock flash 0xFC06–0xFC0B as 12 hex digits (see `tools/flash-tui`) and can be changed by hand.
- Backups can be exported as a ZIP (all images plus `backups.csv`) or as the CSV list alone.

### Flash AP

A guide in steps (`js/apwizard.js`, styles in `css/apwizard.css`, which the AP does not pack). The plan for each setup, the partition table check and the radio's log lines are in `js/apsteps.js` (`test/apsteps.test.mjs`).

1. **Choose AP:** a card per AP setup from the boards in `info.json` (`ap_boards`, from `ap/boards/`), drawn by `js/appics.js`: one board, a board with a screen, an S3 with a C6 radio on a cable or over WiFi. Where the board takes either, the C6 radio plain or with a screen.
2. **Flash:** each board in the setup. The C6 radio first (`radios` in `info.json`, from `ap/radio/boards/` and `ap/netradio/boards/`), then the main board (an S3 or C5 on its port marked USB), then for the cable the wiring.
3. **WiFi and token:** on the same port when the browser gives it back after the reset (`portAgain()`; otherwise the user picks it): network, password, name and a random token with a copy button, with the setup network as the way out. An AP already on a network keeps it and its token unless the user changes them (no empty password is sent). For the radio over WiFi it then sends the C6 its network and, if typed, the S3's address (empty: mDNS) on the C6's USB console, and reads its log until it is connected (`js/radioconsole.js`).
4. **Done:** the AP's address, board and version, with its dashboard.

How flashing keeps the AP's data (`js/apflash.js`, with esptool-js):

- It checks that the chip is the one the step is for, and reads the partition table on the board.
- A blank board is erased first.
- An AP that runs OpenESL gets only the bootloader, partition table, otadata and app of the manifest (`build.py` splits the merged image), so it keeps its WiFi, token, tags, layout files and tag firmware.
- When NVS is not where the new table has it (OpenESL from before 0.3.0, other firmware), it stops and offers to erase.
- The C6 radio on a cable is always erased first. The radio over WiFi is treated as an AP (its NVS is where the AP has it).

### Dashboard

The AP's state and a table of every tag, refreshed every 10 s while the tab is in view. The same dashboard runs on the AP itself at `/dashboard`.

**Files**

- `js/dashboard.js`: the tab. `js/dashdata.js` works out the figures (`test/dashdata.test.mjs`).
- `js/apport.js`: the connection over the AP's USB port, shared with the Design tab and the setup page.
- `js/aplayouts.js`: the Layouts card.
- `js/sendtext.js`: sending text to ticked tags (`test/sendtext.test.mjs`).
- `js/fwupload.js`: the Firmware card (`test/fwupload.test.mjs`).
- `js/apdash.js`: starts the dashboard on the AP's own page.

**Connecting**

- Over the AP's USB port (Web Serial, the same connection as "WiFi and API token over USB", `js/apport.js`).
- Or over HTTP through the bridge in `tools/esl-client` ("Bridge (advanced)"). A page on https may not call an AP on the local network itself.
- On the AP: `ap/main/webui.py` packs this tab, `css/style.css` and the dashboard's modules into the firmware, and `js/apdash.js` starts it with `initDashboard({ sameOrigin: true })`, so over WiFi it needs no bridge. **Keep the dashboard's modules free of imports outside the list in `webui.py`.** On the AP's page the renderer and the template library come from GitHub Pages.
- The browser demo, `?demo=browser` (the "Try the demo" button on the Overview): `js/demoap.js` is a pretend AP with six tags that answers the API in the page, drawing each screen with the renderer (`test/demoap.test.mjs`). `js/main.js` makes it and gives it to `initDashboard({ demoAp })` and `initEditor({ demoAp })`, so neither imports it and the AP's page stays as it is. In the demo `localStorage` and `sessionStorage` are kept in memory (`memoryStorage()`, set up when `main.js` imports `demoap.js` first), so it never touches the drafts and picks of real use, and a reload starts over. It never connects to a real AP: the dashboard ignores a USB port opened elsewhere, and the designer hides its USB, bridge and address controls. Keep it in step when an API call the dashboard or the designer uses changes. `?demo` alone, or with an address, is the bridge of `tools/demo.py` instead.

**What it shows**

- One line for the connection: where, which AP (its board, from `board_name` in `GET /api/tags`, or the chip of an older AP), its name on the network, its firmware, and how long ago it last answered. Red when it stops answering, and the figures below go grey.
- Four figures (tags, need attention, busy or late, scheduled) that also filter the table.
- The tag table: the state first, the last six digits of the MAC in bold, last check-in, the next one expected from its poll interval or how late it is, signal, battery, firmware, and what it is waiting for or what is wrong (from `GET /api/health`). Times are relative, with the clock time on hover. On a phone each tag is one row with its details on a line under it.
- The Access point card (firmware, uptime, why it last started, channel, poll interval, time, schedule, WiFi) and the Firmware card, folded, each with a one-line summary.

**Ticked tags**

- Each row has a tick box; the header's ticks every tag the table shows. A box then shows the count (and how many of them Find or a filter hides) and the form to send them text or firmware.
- **Forget** removes the ticked tags from the AP (`DELETE /api/tags/{mac}`: content, settings and scheduled content), after asking twice. A broken tag stays gone; one that still polls comes back as a new tag.
- **Text** with the built-in layout `text`: the AP's address and WiFi, the tag's own info, today's date, or your own four lines. A preview is drawn by the AP's own code (on the AP's page as in the Layouts card), with a list of what each tag gets.
- A word in braces in your own lines is filled in for each tag: `{ap_ip}`, `{ap_name}`, `{wifi}`, `{ap_fw}`, `{channel}`, `{tags}` from the AP, `{mac}`, `{model}`, `{battery}`, `{rssi}`, `{fw}`, `{seen}` from the tag, and `{weekday}`, `{date}` (Norwegian) and `{time}`. Buttons put them in.
- When the lines have the date, the next days go to the AP as scheduled content for midnight (at most 50 rows in one `PUT /api/tags`), after the AP's clock is checked and, if it has none, set from the computer. `esl_client.py text` and `date` do the same for one tag.
- **Tag firmware** published with the page: the AP gets the image for the first ticked tag of a model that needs it and gives it to the others too. The list says what each tag gets or why it gets nothing (already has it, no radio updates).

**Layouts card** (`js/aplayouts.js`)

- Per tag model: the built-in layouts and the AP's layout file drawn with the renderer, with how many tags show each (`js/layoutinfo.js` asks each tag whose content changed for its layout).
- On the AP's page, a board with the Design tab draws with the renderer it packs. One without it (the C6) loads the published page's renderer in a sandboxed frame that cannot reach the page's token (`js/renderframe.js`). The card says when the previews could not be loaded, or come from a renderer built for other AP firmware than the AP runs.
- The templates of the library the AP lacks, each sent with one press (`addTemplate` from `js/catalog.js`, then `PUT /api/layouts/{model}`).
- A layout no tag uses can be removed. Edit opens it in the designer where the page has one (an `openesl-edit-layout` event to `js/editor.js`).
- Picking a layout shows its fields in order, the template's text for each, examples for curl, JSON, an `esl_client.py sync` CSV and Home Assistant, and the tags that show it. Tags whose layout the AP lacks are named at the top.

**Firmware card** (`js/fwupload.js`)

- Sends new AP firmware (`POST /api/ota`, in 16 KB parts) or tag firmware (`PUT /api/tag-firmware`, in 8 KB parts, which the AP then sends to the tags over the radio).
- The files come from those published with the page (the `-ota.bin` files `build.py --firmware` copies) or a file of your own.
- It shows the AP's app slots and how far the tags of each model are, and waits for the AP to come back after its update.
- Before AP firmware goes out it names the version in the image and the one the AP runs, and warns when it is the same or older, or made for another chip or board (`chip` and `display` in `GET /api/ota`).
- When the page has a newer release than the AP runs, an arrow-up button next to the AP's version sends the image that fits it.
- A tag that takes firmware over the radio (`"ota"` in `GET /api/tags`) and runs older firmware than the page has for its model gets a small arrow-up button in its firmware column (the tooltip names the version), which gives the page's image to the AP.

### Set up

- **WiFi and API token over USB** sets the AP's network and API token from the browser over its USB port, for when a phone cannot join the setup network (`js/wifisetup.js`, `initWifiSetup(prefix)`). It uses Web Serial and `PUT /api/wifi` in the `@ESL` frames of `ap/main/serial_api.c` (`js/apserial.js`). It shows the AP's scan list, fills in a random token (`newToken()`) for an AP that has none, and watches the AP join. Step 3 of the Flash AP tab does the same right after flashing.
- Below it, five short points with links to the wiki pages: flashing, the setup network, the token, the dashboard, running it. The details live only in the wiki and `ap/README.md`.

### API

`api.html` shows `docs/openapi.yaml` with [Scalar](https://github.com/scalar/scalar) (loaded from jsdelivr, version pinned), in a frame in this tab.

- "Test Request" sends real requests through the bridge in `tools/esl-client` (`esl_client.py --serial /dev/ttyACM0 serve`), which passes them on to the AP over USB or WiFi. A web page cannot reach the AP itself.
- Edit the spec, not the page. `ap/test/openapi/check_openapi.py` checks it against the AP code.

## The preview is the AP's own drawing

`wasm/web_api.c` is built to WebAssembly with Emscripten, once per tag model. It links the AP's layout compiler (`ap/components/esl_core/esl_layout_compile.c`) and the AP's drawing (`esl_render.c`, with the tag's fonts and barcodes), which makes the image the AP sends to a tag. So:

- the preview is pixel for pixel the image a tag of that model gets and shows (the test checks the WebAssembly build against the same code built natively, and `sim/` checks the AP's drawing);
- an error under the preview is the message the AP answers with (`422`) when the file is uploaded;
- the size and version shown are those of the binary file the tags download.

What a layout can draw:

- Text in a box from a field: six fonts, alignment, up to 3 lines, black, white, inverted or highlighted. The client sends the fields with each update, e.g. `price` and `price_ex` for the price inc. and ex. VAT (the AP does not work out VAT).
- Fixed texts that are part of the layout (such as "inkl. mva").
- Boxes in black or white, filled or only a border, with round corners if you like. A black area with white text is a black box with white text on it, or inverted text.
- Straight and slanted lines, 1 to 16 px wide.
- Small pictures (icons) made from a PNG, JPEG, GIF or SVG with the Icon button: dark pixels are drawn, up to 128 px.
- One barcode per layout (Code 128 or EAN-13) and one QR code per layout (e.g. a link), each from a field.

The compiler limits the number of boxes (inverted and highlighted text count as one each), lines, icons and lines of text per layout: `ESL_DRAW_MAX_RECTS`, `ESL_DRAW_MAX_LINES`, `ESL_DRAW_MAX_ICONS` and `ESL_DRAW_MAX_RUNS` in `shared/proto.h`, and more on AP boards with PSRAM (`ap/boards/README.md`). The status bar under the preview shows how much of each a layout uses on the connected AP.

## Publishing

`.github/workflows/web.yml`, on every push to `master`:

1. builds the tag firmware (SDCC 4.5.0, every model) and the AP (ESP-IDF v5.5.5, every board, each merged into one image at offset 0) through `firmware.yml`;
2. builds the page and the renderer and runs the tests;
3. publishes `web/dist` to GitHub Pages.

A fork that publishes its own page sets its address in `SITE` in `js/site.js`: the AP's page and the designer take it from there.

The firmware the flash tabs offer is the latest GitHub release (`firmware/info.json` says which); before the first release it is the firmware built from that commit. Pull requests build and test only, and attach the page, with the firmware built from the pull request, as an artifact. See "Versions and release files" in [docs/release.md](../docs/release.md#7-versions-and-release-files).

One-time setup in the repository: Settings → Pages → Source: **GitHub Actions**.

esptool-js is loaded from unpkg (`esptool-js@0.6.1`, in `js/apflash.js`); everything else is in this folder.

## Notes

- The Dashboard was tried on the AP and a 1.54" tag on 2026-10-15.
