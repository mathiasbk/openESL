# Code map

Where the code for what lives, how the parts talk to each other, and where to start when you want to change something. The details are in the README of each folder.

## The parts and how they talk

```
 client (script, Home Assistant,         AP (ESP32-C6, or S3 + C6)                       tags (ZBS243)
 esl-client, the web page)               ┌──────────────────────────────────────┐
 ┌───────────────────────┐   HTTP + token│ ap/main/          ESP-IDF glue:       │  802.15.4  ┌──────────────┐
 │ PUT /api/tags/{mac}   │──────────────►│   http_api.c, wifi.c, radio_*.c, NVS  │◄──────────►│ tag/src/     │
 │ GET /api/tags, health │◄──────────────│ ap/components/esl_core/  plain C:     │ POLL →     │  app.c       │
 └───────────────────────┘   JSON        │   esl_api → esl_store → esl_render    │ ← IMG, FW, │  hal/zbs243/ │
                                         │   → esl_image → esl_radio             │   CONFIG   └──────────────┘
                                         └──────────────────────────────────────┘
               shared/proto.h: the radio messages, used by both the tag and the AP
               shared/models.json: the tag models, generated into C and JS by tools/gen_models.py
```

1. A client sends text fields and a layout name to the AP's HTTP API ([openapi.yaml](openapi.yaml)).
2. `esl_api` checks the request and puts the content in `esl_store`, which keeps it in NVS.
3. When the tag polls, `esl_radio` answers. If the tag's picture is out of date, `esl_render` draws the screen and `esl_image` sends it in blocks.
4. The tag stores the picture in its SPI flash and shows it. It only shows pictures and never draws layouts itself.

The protocol in detail: [protocol.md](protocol.md). The reasons behind the design: [architecture.md](architecture.md).

## Folders

| Folder | What it is | README |
|---|---|---|
| `ap/components/esl_core/` | The AP's core in plain C with no ESP-IDF: API, store, layouts, drawing, radio replies, scheduling, data sources. Tested on the PC. | [ap/README.md](../ap/README.md) |
| `ap/main/` | The ESP-IDF layer: WiFi, the HTTP server, NVS, the radio drivers (802.15.4, UART, network), OTA, the LCD | [ap/README.md](../ap/README.md) |
| `ap/boards/` | One folder per AP board: `board.json`, `sdkconfig.defaults`, partitions | [ap/boards/README.md](../ap/boards/README.md) |
| `ap/radio/`, `ap/netradio/` | Firmware for a C6 that is only the radio of an S3 AP, over a cable or over WiFi | [ap/radio/README.md](../ap/radio/README.md), [ap-netradio.md](ap-netradio.md) |
| `ap/test/` | Host tests of the AP core | [ap/README.md](../ap/README.md#tests-on-the-pc) |
| `tag/src/` | The tag firmware: `app.c` is the state machine with no hardware code, `hal/` is everything chip specific | [tag/README.md](../tag/README.md) |
| `tag/boot/` | The tag bootloader that installs firmware received over the air | [tag-ota.md](tag-ota.md) |
| `tag/fonts/`, `tag/src/barcode.c` | Fonts and barcodes for the AP's drawing. The tag keeps only the small and medium fonts (with `font.c` and `draw.c`) for its own status screens; `barcode.c` and the other fonts are compiled into the AP alone | [tag/README.md](../tag/README.md) |
| `tag/test/` | Host tests of the tag firmware against fake hardware | [tag/README.md](../tag/README.md#build-and-test) |
| `shared/` | `proto.h` (radio messages) and `models.json` (tag models) | [shared/README.md](../shared/README.md) |
| `sim/` | The tag firmware and the AP core together on the PC, with a simulated radio and clock | [sim/README.md](../sim/README.md) |
| `web/` | The web page: label designer, tag and AP flashers, dashboard, API docs | [web/README.md](../web/README.md) |
| `web/wasm/` | The AP's own drawing code built to WebAssembly, so the designer's preview is exactly what the AP draws | [web/README.md](../web/README.md#the-preview-is-the-aps-own-drawing) |
| `templates/` | The designer's template library | [templates/README.md](../templates/README.md) |
| `tools/esl-client/` | Command-line client for the API, backup and restore, tag OTA, and the bridge the web page uses | [tools/esl-client/README.md](../tools/esl-client/README.md) |
| `tools/flash-tui/` | Terminal UI for flashing many tags in a row | [tools/flash-tui/README.md](../tools/flash-tui/README.md) |
| `tools/` | `check.py` (runs what CI runs), `gen_models.py`, `fw_version.py`, `release_notes.py`, `demo.py`, `power-budget/` | – |
| `wiki/` | The user guide, published to the GitHub wiki | – |

## Want to change X? Start in Y

| You want to change | Start in |
|---|---|
| An API endpoint | `ap/components/esl_core/esl_api_*.c` (the route table is in `esl_api.c`), then [openapi.yaml](openapi.yaml), which CI checks against the code |
| How a layout is drawn | `esl_render.c`, and `esl_layout_compile.c` for the checks on the JSON. The web preview uses the same code. |
| What a tag is sent and when | `esl_store.c` (what is next for a tag) and `esl_radio.c` (the reply to a frame) |
| Bluetooth tags (the AP sends to them) | `esl_push.c`, and [ble-tags.md](ble-tags.md) |
| A radio message | `shared/proto.h` and [protocol.md](protocol.md) part 1, then both `tag/src/proto.c` and `esl_proto.c`. Older tags must keep working. |
| The tag's behaviour (polling, status screens, OTA) | `tag/src/app.c`, `status.c`, `ota.c` |
| A new tag model | `shared/models.json` and `tag/src/panels/`, see the wiki page "Adding hardware" |
| A new AP board | `ap/boards/` and [access-points.md](access-points.md) |
| WiFi, NVS, the HTTP server | `ap/main/` |
| A tab of the web page | `web/js/` (each tab has its own module, listed in [web/README.md](../web/README.md)) |
| The command-line client | `tools/esl-client/esl_client.py` |

## Tests

`python3 tools/check.py` runs what CI runs, on your machine; `--list` shows the steps and what each needs.

| Test | Covers |
|---|---|
| `ap/test/` (`check.py ap-test`) | The AP core: API, store, layouts, drawing, radio rounds with a fake tag, NVS formats, scheduling, data sources |
| `tag/test/` (`check.py tag`) | The tag firmware for every model: protocol, image download, status screens, OTA |
| `sim/` (`check.py sim`) | End to end: the real tag firmware against the real AP core, through the API |
| `web/test/` (`check.py web`) | The web page's modules, the renderer against the native build, the flasher against a simulated ZBS_Flasher |
| `tools/` (`check.py tools`) | esl-client against a fake AP, the power budget, the release notes, the AP flash budget |
| `check.py openapi`, `check.py wiki` | `openapi.yaml` against the AP code, and the links in the wiki |
| `check.py models` | The generated model files match `shared/models.json` |
| `check.py tag-fw`, `check.py ap-fw` | The tag firmware for every model (SDCC) and the AP firmware for every board (ESP-IDF) build, and each AP image keeps at least 64 KB of its app slot free (`ap/build.py --sizes`) |
| `check.py ap-pack` | The AP's web pack (`ap/main/webui.py`) holds nothing that is only for the web page (the browser demo, the gallery, pictures) |
