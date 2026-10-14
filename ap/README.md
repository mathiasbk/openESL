# AP firmware (ESP32-C6, ESP-IDF)

One ESP32-C6 is the whole AP: an HTTP API over WiFi for any client software, and an 802.15.4 radio towards the tags. The AP holds no business logic, only text fields per tag. Plan and requirements: [docs/ap-firmware.md](../docs/ap-firmware.md). API and radio protocol: [docs/protokoll.md](../docs/protokoll.md).

**Status:** the core and the HTTP API are written, and the tests pass: 373 checks, 0 failures, also with UBSan (built with zig cc for Linux and run in WSL, see below). The ESP-IDF part (`main/`) is written but **not yet built or flashed**. The radio code against `esp_ieee802154` must be checked against the pinned ESP-IDF version on the first build.

## Layout

```
ap/
  components/esl_core/    plain C, no ESP-IDF dependency, tested on the PC
    esl_content   content model, serialisation, content_id (CRC32)
    esl_text      UTF-8 -> Latin-1 (æøå, dashes, curly quotes; unknown -> '?')
    esl_proto     POLL/IDLE/CONTENT and the 802.15.4 frame (15-byte header), MAC as text
    esl_store     table MAC -> content + status, diffing, prebuilt replies
    esl_persist   blob format for NVS
    esl_json      small JSON parser and writer
    esl_api       HTTP API (routes, token, validation), independent of the HTTP server
  main/                   ESP-IDF glue
    app_main.c  startup         store_nvs.c  mutex + NVS storage
    wifi.c      WiFi + mDNS     http_api.c   esp_http_server -> esl_api_handle()
    radio_ieee802154.c  AP and sniffer modes
    radio_stub.c        simulated tags, no radio
  test/                   host unit tests (Makefile)
```

## How content and diffing work

- `content_id` = CRC32 of layout, flags and text fields (never 0). The same content twice gives `changed:false`, no NVS write and no redraw on the tag.
- Trailing empty fields are removed before the id is computed, so `["a",""]` and `["a"]` are the same content.
- Text is converted to Latin-1 and rejected with `422` above 90 bytes in total.
- A tag is `pending` until it has reported the correct `content_id` in a POLL. After an AP restart the content is read from NVS; a tag that already shows it gets IDLE, not a redraw.
- Unknown tags that send POLL are added to the table without content (`content_id:0`). If the table is full, the tag that was seen longest ago and has no content is replaced when a client sends content for a new MAC.

## HTTP API

See [docs/protokoll.md](../docs/protokoll.md) part 2. Every call requires `Authorization: Bearer <token>` (an empty token always gives `401`).

```bash
curl -X PUT http://openesl.local/api/tags/00000123456789A8 \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"layout":"price","fields":["Milk 1 L","24.90","per litre 24.90"],"price_red":true}'

curl -H "Authorization: Bearer $TOKEN" http://openesl.local/api/tags
```

## Configuration

`idf.py menuconfig` -> «OpenESL AP»: WiFi, token, mDNS name (`openesl`), channel (25), PAN ID (0x4553), max tags, TX power, radio mode. SSID, password and token can be overridden in NVS (namespace `cfg`, keys `ssid`, `pass`, `token`).

Radio modes: `AP` (normal), `Sniffer` (logs every frame on the channel, does not answer, no WiFi), `Stub` (simulated tags `00000000FAC70001`..., for testing the API with curl before the tag radio works).

## Build and flash

Requires ESP-IDF 5.2 or newer (pin the version once chosen, requirement G5).

```bash
cd ap
idf.py set-target esp32c6
idf.py menuconfig
idf.py build flash monitor
```

## Tests on the PC

```bash
cd ap/test
make          # build and run (needs cc/gcc/clang)
make SAN=1    # with AddressSanitizer and UBSan
```

Without gcc/clang on Windows: `pip install ziglang`, then build for Linux with `python -m ziglang cc -target x86_64-linux-musl ...` (zig's native Windows target fails in its mingw libc). Run the binary in WSL.

Covers CRC32, text conversion, content, messages and frames, the NVS format, JSON, the table (including locking and persistence), and the whole API (auth, validation, batches, limits). Not yet covered: radio timing (must be measured on hardware, requirement A3), WiFi coexistence (phase 4) and NVS on real flash.

## Open points

- `esl_proto.h` mirrors the message constants from protokoll.md. They should come from `shared/proto.h` instead.
- Tag size: POLL has no model field, so the AP does not know whether a tag is 2.9" or 1.54". The AP stores only text fields and does not depend on the screen, but a 1.54" (152×152) screen fits fewer characters per line. To validate length per model, the AP needs a model field in POLL (using the `reserved` byte) or a `model` parameter in the API.
- **Layout files (todo):** screen layouts are to be made in a design tool and stored as JSON, not fixed in the firmware. For the AP this means: (1) store the layout file per tag model (2.9" and 1.54") in NVS/flash, (2) a new API endpoint to upload and fetch layouts (e.g. `PUT /api/layouts/{model}`), (3) send the layout to the tag over radio, and store a version/id so the tag only fetches it when it changes, (4) let `layout` in `PUT /api/tags` refer to a layout name from the file. This needs a new message type and block transfer in protokoll.md part 3, a file format and a size limit (110 bytes per radio packet). The fixed `price` and `text` layouts are the first version.
- Status page at `/` (requirement A6), OTA (A10) and a setup portal are not started.
