# AP firmware (ESP32-C6, ESP-IDF)

One ESP32-C6 is the whole AP: an HTTP API over WiFi for any client software, and an 802.15.4 radio towards the tags. The AP holds no business logic, only text fields per tag. Plan and requirements: [docs/ap-firmware.md](../docs/ap-firmware.md). API and radio protocol: [docs/protokoll.md](../docs/protokoll.md).

**Status:** the core, the HTTP API (including layout files, per-tag settings and the status page), the radio receive path, the layout transfer and CONFIG to the tags are written, and the host tests pass: 1582 checks, 0 failures (gcc on Fedora, also with UBSan in trap mode). The tests run whole rounds on the air with a fake tag: client → API → store → 802.15.4 frame → fake tag → next POLL, including AP restarts from (fake) NVS, layout files downloaded block by block and settings sent with CONFIG. The ESP-IDF part (`main/`) builds without warnings with ESP-IDF v5.5.5 in all three radio modes (AP, sniffer, stub); image about 1 MB of the 1.875 MB app slot. It has **not been flashed** yet.

## Layout

```
ap/
  components/esl_core/    plain C, no ESP-IDF dependency, tested on the PC
    esl_content   content model, serialisation, content_id (CRC32)
    esl_text      UTF-8 -> Latin-1 (æøå, dashes, curly quotes; unknown -> '?')
    esl_proto     POLL/IDLE/CONTENT/LAYOUT/LAYOUT_REQ/CONFIG and the 802.15.4 frame (15-byte header),
                  MAC and model as text; constants from shared/proto.h
    esl_radio     receive path: frame -> POLL or LAYOUT_REQ -> store/layouts -> reply frame;
                  fake tag (with layout download and CONFIG) for the stub and tests
    esl_store     table MAC -> content + settings + status, diffing, the answer to a POLL
                  (priority LAYOUT, CONTENT, CONFIG, IDLE)
    esl_layout    layout files per tag model: storage, version, blocks for the radio
    esl_layout_compile  layout file (JSON) -> the binary the tags download, checked per model
    esl_persist   blob formats for NVS (content and settings per tag)
    esl_json      small JSON parser and writer
    esl_api       HTTP API (routes, token, validation) and the status page, independent of the HTTP server
  main/                   ESP-IDF glue
    app_main.c  startup         store_nvs.c  mutex + NVS storage (tags and layouts)
    wifi.c      WiFi + mDNS     http_api.c   esp_http_server -> esl_api_handle(), status page
    radio_ieee802154.c  AP and sniffer modes, calls esl_radio_handle()
    radio_stub.c        simulated tags (esl_faketag), no radio
  partitions.csv          NVS enlarged to 84 KB for tag content and layout files
  test/                   host unit tests (Makefile)
```

## How content and diffing work

- `content_id` = CRC32 of layout, flags and text fields (never 0). The same content twice gives `changed:false`, no NVS write and no redraw on the tag.
- Trailing empty fields are removed before the id is computed, so `["a",""]` and `["a"]` are the same content.
- Content with a layout from a layout file is stored by name; the id covers the name, so it is the same on both models. The store turns the name into "layout n of the file" for the tag's model each time it answers a POLL.
- Text is converted to Latin-1 and rejected with `422` above 92 bytes in total (`ESL_MAX_TEXT_BYTES` in `shared/proto.h`: what fits in the 110-byte payload with four length bytes and the 4 bytes reserved for a MIC; the same limit as `tag/tools/label_design.py`).
- A tag is `pending` until it has reported the correct `content_id` in a POLL. After an AP restart the content is read from NVS; a tag that already shows it gets IDLE, not a redraw.
- Unknown tags that send POLL are added to the table without content (`content_id:0`). If the table is full, the tag that was seen longest ago and has no content is replaced when a client sends content for a new MAC.

## HTTP API

See [docs/protokoll.md](../docs/protokoll.md) part 2. Every call requires `Authorization: Bearer <token>` (an empty token always gives `401`).

```bash
curl -X PUT http://openesl.local/api/tags/00000123456789A8 \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"layout":"price","fields":["Milk 1 L","24.90","per litre 24.90"],"price_highlight":true}'

curl -H "Authorization: Bearer $TOKEN" http://openesl.local/api/tags
```

Every tag object also has `"model"`, `"draw_failed"`, `"layout_version"`, `"layout_pending"`, `"layout_failed"` and `"content_error"` (protokoll.md part 2).

### Layout files (krav A12-A14)

The file format, the checks and the endpoints are in [docs/protokoll.md](../docs/protokoll.md) part 3 and part 2; the radio transfer in part 1. In short:

```bash
curl -X PUT http://openesl.local/api/layouts/2.9 -H "Authorization: Bearer $TOKEN" \
  --data-binary @../tag/designs/layouts/2.9.json
curl -X PUT http://openesl.local/api/tags/00000123456789A8 -H "Authorization: Bearer $TOKEN" \
  -d '{"layout":"shelf","fields":["Kaffe 500 g","49,90","kr 99,80/kg","Tilbud"],"price_highlight":true}'
```

- `PUT /api/layouts/{model}` compiles the file for that model (`esl_layout_compile`) and answers `422` with the place and reason if it would not work on the tag (outside the screen, unknown font, box lower than its font, more than 8 text lines or 5 rectangles per layout, more than 768 bytes for the tag, ...). The JSON is stored in NVS (namespace `esl_layouts`) and compiled again at boot.
- The version is the CRC-32 of the compiled binary. A tag whose POLL reports another version gets the first LAYOUT block instead of IDLE/CONTENT, and fetches the rest with LAYOUT_REQ in the same wake-up (94 bytes per block).
- `layout` in `PUT /api/tags` may name a layout in the file (A14). It must be in the file for the tag's model once the model is known, or in either file before that, and the tag's firmware must be able to download layouts (18-byte POLL); otherwise `422`.
- Firmware whose POLL is shorter than 18 bytes never gets a LAYOUT and keeps working with `price` and `text`.

## Tag settings (poll interval and CONFIG)

Each tag can have its own settings, set with `PUT /api/tags/{mac}/config` (`poll_s`, `max_poll_s`, `lost_ap_s`), and the AP has a default interval for tags without their own `poll_s`, set with `PUT /api/config`. Details: [docs/protokoll.md](../docs/protokoll.md) parts 1 and 2.

- The interval reaches every tag through `next_poll_s` in IDLE/CONTENT/LAYOUT, so older tag firmware gets it too.
- Tags that send `cfg_ver` in POLL (14 bytes or more; the current firmware sends 18) also get a CONFIG message (type 0x08) whenever their `cfg_ver` differs from the AP's version of their settings. A new layout file and new content go first; CONFIG comes on the next poll (protokoll.md part 1, "Priority"). `config_pending` in the API shows when a tag has not confirmed.
- `cfg_ver` is a 16-bit hash of the settings (0 = all defaults), so it needs no state of its own and nothing is resent after an AP restart.
- NVS: per-tag settings are blob `c<slot>` in namespace `esl_tags` next to the content blob `t<slot>`; the default interval is `poll_s` (u16) in namespace `cfg`. `PUT /api/config {"poll_s":null}` erases it, and the `menuconfig` value (`ESL_NEXT_POLL_S`) applies again.
- A tag with its own settings counts as holding data: it is not evicted when the table is full, and `DELETE` erases its settings too.

## Status page

`http://openesl.local/` shows the layout files per model (version, size for the tags, layout names) and a table of all tags (MAC, model, last seen, battery, RSSI, firmware, layout and content, pending/shown, draw failed, why named content is not sent, layout file pending/rejected) and refreshes every 30 s (requirement A6). It needs no token, since it only reads; turn it off with `ESL_STATUS_PAGE` in menuconfig.

## Tag model in POLL

Byte 11 of POLL, formerly reserved and always 0, is the tag model (`ESL_POLL_OFF_MODEL` in `shared/proto.h`): 0 = unknown, 1 = 2.9", 2 = 1.54". Firmware that still sends 0 works as before and shows `"model":null`. Bytes 12-13 are the settings version `cfg_ver` (see "Tag settings"), bytes 14-17 are the tag's layout file version (protokoll.md part 1).

## Configuration

`idf.py menuconfig` -> «OpenESL AP»: WiFi, token, mDNS name (`openesl`), status page, channel (25), PAN ID (0x4553), next_poll_s, max tags, TX power, radio mode. `next_poll_s` is the built-in default interval; `PUT /api/config` overrides it at run time. SSID, password and token can be overridden in NVS (namespace `cfg`, keys `ssid`, `pass`, `token`).

Radio modes: `AP` (normal), `Sniffer` (logs every frame on the channel, does not answer, no WiFi), `Stub` (simulated tags `00000000FAC70001`..., for testing the API with curl before the tag radio works; their frames go through the same `esl_radio_handle()` as real ones, and they download layout files like the tag firmware does).

## Build and flash

Pinned to ESP-IDF v5.5.5 (requirement G5); `dependencies.lock` pins the managed components. The custom partition table (`partitions.csv`) assumes 2 MB flash with one 1.875 MB app slot; OTA (A10) would need two app slots and a 4 MB layout.

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
make SAN=1    # with AddressSanitizer and UBSan (needs libasan/libubsan; on Fedora: dnf install libasan libubsan)
```

Without gcc/clang on Windows: `pip install ziglang`, then build for Linux with `python -m ziglang cc -target x86_64-linux-musl ...` (zig's native Windows target fails in its mingw libc). Run the binary in WSL.

Covers CRC32, text conversion, content, messages and frames, the NVS format, JSON, the table (including locking and persistence), layout files (the compiler with the example files in `tag/designs/layouts/` and every error message, storage, blocks), the whole API (auth, validation, batches, limits, layouts and named content, settings, status page), and the radio path end to end with fake tags: new tag, same content (no redraw), new content, lost CONTENT, AP restart, tag restart, next_poll_s, per-tag settings over CONFIG (the fake tag uses the new interval, confirms with `cfg_ver`, gets them again after a tag restart but not after an AP restart; an old-firmware fake tag never gets CONFIG but still gets its interval), the run-time default interval in NVS, 24 tags in one batch (G3), frames that must be ignored, and layout files over the air: download in blocks then content drawn with the downloaded layout in the same round, no second download of the same version, a lost block resumed in the next round, the file changed during a download, a rejected file, a deleted file, firmware without layout files, 24 tags of both models. One test gives a tag a layout file, content and settings at once and checks the order (layout and content in one wake-up, CONFIG in the next; layout then CONFIG in one wake-up when the content did not change). The test reads `../../tag/designs/layouts/*.json`, so run it from `ap/test`. Not yet covered: the ESP-IDF glue in `main/` (built, not run), radio timing (must be measured on hardware, requirement A3), WiFi coexistence (phase 4) and NVS on real flash.

## Open points

- **Not run on hardware yet:** `main/` builds without warnings with ESP-IDF v5.5.5 (AP and stub modes checked). Commit `dependencies.lock` (G5).
- **Another model** only needs a new `ESL_MODEL_*` value and screen size in `shared/proto.h`, and a name in `esl_model_name()` / `esl_model_size()`.
- **Validation per model (A13):** layout files are checked against the model's screen and what the tag can draw, and named content against the tag's model. The 92-byte text limit is the same for both sizes; there is no per-field or per-line length check (the tag shrinks and truncates text that does not fit).
- **Layout transfer timing:** the block exchange (a LAYOUT_REQ answered within the tag's 20 ms window) is tested on the PC only; check on hardware with the sniffer that 9 blocks in a row arrive, and how long a round takes.
- **Tag settings:** the tag firmware keeps CONFIG in flash (`hal_store_*`), so a restarted tag uses its settings at once; the fake tag does not (it reports `cfg_ver` 0 after a restart and gets one extra CONFIG, which the tests rely on). The fake tag and `radio_stub.c` do not sleep per tag; the stub still polls all simulated tags every `ESL_STUB_INTERVAL_S`.
- **Development builds with layouts but without CONFIG** (the layout branch before the merge) send 0 in POLL bytes 12–13. The AP reads that as "understands CONFIG, built-in settings", so such a tag with its own settings would get CONFIG, which it drops. No such firmware was released; flash the current one.
- OTA (A10) and a WiFi setup portal are not started.
