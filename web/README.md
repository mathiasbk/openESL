# Web page: label designer and flasher

A static page for GitHub Pages (or any web server) with an overview, three tools and the API documentation:

| Tab | What it does |
|---|---|
| **Overview** | What OpenESL is, how the parts fit together, and what hardware you need. The landing page. |
| **Design** | Draws and edits layout files (docs/protokoll.md part 3) for the 2.9" and the 1.54". Boxes can be dragged and resized on the preview; text, inverted text, lines and filled boxes can be added. Saves the layout file for `PUT /api/layouts/{model}` and a label design (`esl-label`) for `tag/tools/label_design.py`. |
| **Flash tag** | Flashes tags through a D1 mini running atc1441's ZBS_Flasher, over Web Serial. The model is picked from a preview of each tag. The same steps and checks as `tools/flash-tui` (MAC, backup before erase, MAC to the info page, write, verify); the info page is never erased. Backups are kept in the browser (IndexedDB, kept between visits until the site's data is cleared) with an optional barcode per tag, and can be exported as a ZIP (all images plus `backups.csv`) or as the CSV list alone. |
| **Flash AP** | Flashes the ESP32-C6 access point with esp-web-tools. |
| **API** | The AP's HTTP API and the layout file format: parts 2 and 3 of `docs/protokoll.md`, turned into HTML by `apidocs.py` when the page is built, so the page and the repository say the same. Edit the Markdown, not the page. |

Web Serial needs Chrome or Edge on a computer, and a secure page (https, or http://localhost).

## The preview is the tag's own drawing

`wasm/web_api.c` is built to WebAssembly with Emscripten, once per tag model. It links the tag's drawing code (`tag/src/layout.c`, `draw.c`, the fonts), the tag's file check (`layout_file_install`) and the AP's layout compiler (`ap/components/esl_core/esl_layout_compile.c`), the same sources `tag/tools/render_host.py` uses for the previews. So:

- the preview is pixel for pixel what a tag of that model draws (the test checks this against the same code built natively);
- an error under the preview is the message the AP answers with (`422`) when the file is uploaded;
- the size and version shown are those of the binary file the tags download.

What the format can draw: text in a box (four fonts, alignment, up to 3 lines, black, white, inverted, highlighted), and filled boxes in black or white. A line is a box 1 or 2 px wide; a black area with white text is a black box with white text on it, or inverted text. A layout can hold at most 5 boxes (inverted and highlighted text count as one each) and 8 lines of text, because the tag's scene has no room for more. Diagonal lines and images are not in the format.

## Build and test

```bash
source /path/to/emsdk/emsdk_env.sh      # Emscripten 3.1.74
python web/build.py                     # renderer + page into web/dist/
python web/build.py --native            # native build of web_api.c, for the test
node --test web/test/*.test.mjs
cd web/dist && python -m http.server    # http://localhost:8000
```

`js/zbs.js` and `js/tagflash.js` are ports of `tools/flash-tui/zbs_proto.py` and `workflow.py`; `test/tagflash.test.mjs` runs them against a simulated ZBS_Flasher (`test/fake_flasher.mjs`, a port of `fake_flasher.py`). Keep the ports in step with the Python originals.

## Publishing

`.github/workflows/web.yml` builds the tag firmware (SDCC 4.5.0, both models), the AP (ESP-IDF v5.5.5, merged into one image at offset 0), the page and the renderer, runs the tests, and publishes `web/dist` to GitHub Pages on every push to `master`. The firmware the flash tabs offer is the one built from that commit (`firmware/info.json`). Pull requests build and test only, and attach the page as an artifact.

One-time setup in the repository: Settings → Pages → Source: **GitHub Actions**.

esp-web-tools is loaded from unpkg (`esp-web-tools@10.4.0`); everything else is in this folder.
