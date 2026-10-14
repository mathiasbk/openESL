# AP firmware (ESP32-C6, ESP-IDF)

One ESP32-C6 is the whole AP: an HTTP API over WiFi for any client software, and an 802.15.4 radio towards the tags. The AP holds no business logic, only text fields per tag. Plan and requirements: [docs/ap-firmware.md](../docs/ap-firmware.md). API and radio protocol: [docs/protokoll.md](../docs/protokoll.md).

**Status:** the core, the HTTP API (including layout files and the status page) and the radio receive path are written, and the host tests pass: 698 checks, 0 failures (gcc on Fedora, also with UBSan in trap mode). The tests run whole rounds on the air with a fake tag: client → API → store → 802.15.4 frame → fake tag → next POLL, including AP restarts from (fake) NVS. The ESP-IDF part (`main/`) is written but **not yet built or flashed**: it has only been syntax-checked against stub headers. The radio code against `esp_ieee802154` must be checked against the pinned ESP-IDF version on the first build.

## Layout

```
ap/
  components/esl_core/    plain C, no ESP-IDF dependency, tested on the PC
    esl_content   content model, serialisation, content_id (CRC32)
    esl_text      UTF-8 -> Latin-1 (æøå, dashes, curly quotes; unknown -> '?')
    esl_proto     POLL/IDLE/CONTENT and the 802.15.4 frame (15-byte header), MAC and model as text;
                  constants from shared/proto.h
    esl_radio     receive path: frame -> POLL -> store -> reply frame; fake tag for the stub and tests
    esl_store     table MAC -> content + status, diffing, prebuilt replies
    esl_layout    layout file per tag model, with version
    esl_persist   blob format for NVS
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

Additions to protokoll.md part 2 (not yet written into docs/):

- Every tag object has `"model"` (`"2.9"`, `"1.54"`, or `null` when the tag has not polled since boot or does not send a model) and `"draw_failed"` (bit1 of the flags in the tag's last POLL).
- Layout files per model (part 3, requirement A12). The file format is not specified yet, so the AP accepts any JSON object up to 8192 bytes and stores it as it is. The version is the CRC32 of the file. Model names in the URL: `2.9`, `1.54`.

| Request | Reply |
|---|---|
| `PUT /api/layouts/{model}` with the file as body | `{"model":"2.9","version":123,"size":456,"changed":true}`. `400` if not a JSON object, `413` above 8192 bytes, `404` for an unknown model. The same file again gives `changed:false` and no NVS write. |
| `GET /api/layouts` | `{"max_size":8192,"layouts":[{"model":"2.9","version":123,"size":456},{"model":"1.54","version":null,"size":0}]}` |
| `GET /api/layouts/{model}` | the file as uploaded, `404` if none |
| `DELETE /api/layouts/{model}` | `{"model":"2.9","deleted":true}`, `404` if none |

The layouts are stored and survive a restart, but are **not yet sent to the tags**: that needs the block transfer in protokoll.md part 3.

## Status page

`http://openesl.local/` shows a table of all tags (MAC, model, last seen, battery, RSSI, firmware, content, pending/shown, draw failed) and refreshes every 30 s (requirement A6). It needs no token, since it only reads; turn it off with `ESL_STATUS_PAGE` in menuconfig.

## Tag model in POLL

Byte 11 of POLL, formerly reserved and always 0, is the tag model (`ESL_POLL_OFF_MODEL` in `shared/proto.h`): 0 = unknown, 1 = 2.9", 2 = 1.54". Firmware that still sends 0 works as before and shows `"model":null`. The tag firmware does not send the model yet.

## Configuration

`idf.py menuconfig` -> «OpenESL AP»: WiFi, token, mDNS name (`openesl`), status page, channel (25), PAN ID (0x4553), next_poll_s, max tags, TX power, radio mode. SSID, password and token can be overridden in NVS (namespace `cfg`, keys `ssid`, `pass`, `token`).

Radio modes: `AP` (normal), `Sniffer` (logs every frame on the channel, does not answer, no WiFi), `Stub` (simulated tags `00000000FAC70001`..., for testing the API with curl before the tag radio works; their frames go through the same `esl_radio_handle()` as real ones).

## Build and flash

Requires ESP-IDF 5.2 or newer (pin the version once chosen, requirement G5). The custom partition table (`partitions.csv`) assumes 2 MB flash with one 1.875 MB app slot; OTA (A10) would need two app slots and a 4 MB layout.

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

Covers CRC32, text conversion, content, messages and frames, the NVS format, JSON, the table (including locking and persistence), layout files, the whole API (auth, validation, batches, limits, layouts, status page), and the radio path end to end with fake tags: new tag, same content (no redraw), new content, lost CONTENT, AP restart, tag restart, next_poll_s, 24 tags in one batch (G3), and frames that must be ignored. Not yet covered: the ESP-IDF glue in `main/` (not built), radio timing (must be measured on hardware, requirement A3), WiFi coexistence (phase 4) and NVS on real flash.

## Open points

- **Not built:** ESP-IDF is not installed on the development PC, so `main/` has never been compiled for real. Check on the first build: the `esp_ieee802154` callback names, `partitions.csv`, the `../../../shared` include path in `components/esl_core/CMakeLists.txt`, and `dependencies.lock` (commit it, G5).
- **Model in POLL:** the AP reads byte 11 as the model, with the same codes as `MODEL_CODE` in `tag/src/model.h` (1 = 2.9", 2 = 1.54"), but the tag firmware still sends 0. protokoll.md part 1 should say so once tag and AP agree. Another model only needs a new `ESL_MODEL_*` value and a name in `esl_model_name()`.
- **Validation per model (A13):** the AP knows the model of each tag but has nothing model-specific to check yet: the 92-byte radio limit is the same for both sizes, and per-model limits (characters per line, which layout names exist) come with the layout file format.
- **Layout files (A12, G7):** stored per model and versioned, but not sent to tags. Waiting for the file format, the radio block transfer and the field that tells a tag its layout is out of date (protokoll.md part 3). `layout` in `PUT /api/tags` is still `price` or `text`.
- **next_poll_s** is set in menuconfig only; there is no API to change it at run time.
- OTA (A10) and a WiFi setup portal are not started.
