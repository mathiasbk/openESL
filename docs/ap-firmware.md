# AP firmware (ESP32-C6, ESP-IDF)

**Status:** built. This file is the original plan, with notes on what came of it; [ap/README.md](../ap/README.md) describes the firmware as it is.

## Tools
- **ESP-IDF** (latest stable 5.x, pinned version). Apache 2.0, fine commercially.
- Components: `esp_wifi`, `esp_http_server` (the API), `esl_json` (a small JSON parser and writer of our own, host-tested; cJSON is not used), `mdns` (the name `openesl.local`), `nvs_flash`, `esp_ieee802154`.
- Not Arduino: ESP-IDF gives direct access to the 802.15.4 driver and the coexistence settings.

## Tasks
1. **Radio** (`main/radio_ieee802154.c`): `esp_ieee802154_enable()`, set channel, PAN ID and short address `0x0000`, `rx_when_idle = true`. Reception happens in the callback `esp_ieee802154_receive_done()`. It runs in interrupt context, so copy the frame to a queue and let a high-priority task reply.
2. **Reply quickly** (`esl_radio.c`, `esl_store.c`): build the reply (IDLE, CONFIG, or an IMG or FW block) straight from the table in RAM. The image for an IMG block is drawn at the POLL (`esl_render.c`, `esl_image.c`). Target: under 5 ms from the received POLL to the sent reply (requirement A3).
3. **Storage** (`esl_store.c`, `main/store_nvs.c`): table MAC → {content, content_id, last seen, battery, RSSI, fw}. Content is stored in NVS when it changes (requirement A4). Status is kept in RAM.
4. **HTTP API** (`esl_api.c`, `main/http_api.c`): `esp_http_server` with the endpoints in [protokoll.md](protokoll.md) part 2. Check the token, parse the JSON, convert UTF-8 to Latin-1, validate length, calculate CRC32 (`content_id`) and store; the radio path draws the image from it. Protect the table with a mutex, since HTTP and radio run in separate tasks.
5. **WiFi setup**: done. A setup network `openesl-XXXX` with a form at `/setup` (with a captive portal), `GET`/`PUT /api/wifi`; the values go to NVS (`cfg`). mDNS name `openesl`. See ap/README.md, "WiFi setup".
6. **Status page** (SHOULD): done, an HTML table at `/` with the same data as `GET /api/tags`, plus the health flags and the webhook.
7. **Layout files per model** (requirements A12, G7): store the layout files for 2.9" and 1.54" in NVS, check and compile each one for its model when it is uploaded ([protokoll.md](protokoll.md) part 3), and draw the screens of that model's tags with it (`esl_render.c`); the file never goes to the tags. Done in `esl_layout` / `esl_layout_compile`.
8. **Model per tag** (requirements A13, T18): store the model the tag reports in POLL, and validate the layout name and the number of fields against that model's file.

Added after the plan, all described in ap/README.md: drawing each tag's screen on the AP and sending it as an image (`esl_render`, `esl_image`, protokoll.md part 1 "Image transfer"), firmware for the tags (`esl_tagfw`), the API over the USB port (`ESL_SERIAL_API`), scheduled content and the AP's clock (`/api/schedule`, `/api/time`, NTP), the health report and webhook (`/api/health`), OTA for the AP (`/api/ota`, A10), the token lockout, the optional LCD status screen, and the task watchdog.

## Coexistence (WiFi + 802.15.4 on the same chip)
- The C6 shares one 2.4 GHz radio between WiFi and 802.15.4 by time-division. Software coexistence is compiled in (`CONFIG_ESP_COEX_SW_COEXIST_ENABLE` in `ap/sdkconfig.defaults`) and switched on at run time with `esp_coex_wifi_i154_enable()` in `radio_start()`, as Espressif's Zigbee gateway and Thread border router examples do. Without that call, 802.15.4 receiving makes WiFi deaf: scans find nothing (or only a network or two on one channel), joining fails with "no AP found", and a joined AP gets no traffic. The state outlives a USB or software reset, so a board that ran firmware without the call stays deaf until firmware with it starts (tested on the C6 Super Mini, 2026-10-16).
- WiFi uses modem sleep (`WIFI_PS_MIN_MODEM`). It is what gives 802.15.4 its turns: with `WIFI_PS_NONE` WiFi holds the radio and the AP hears no tags. API replies can wait up to a beacon interval. The tag's single extra attempt covers most collisions.
- Measure packet loss in phase 4. If it is above a few percent, move the radio to the second C6 (connected by UART) as [risiko.md](risiko.md) describes.

## Test tools you get almost for free
- **Sniffer:** the same radio code in promiscuous mode, writing every frame on the channel to serial. The first thing you build; you need it to see whether the tag actually transmits.
- **Fake tag:** what was built instead of a second C6: the Stub radio mode simulates tags inside the AP itself, and [sim/](../sim/README.md) runs the real tag firmware against the AP core on the PC (in CI).

## Writing a client
The AP is only an API, so the client can be anything that speaks HTTP. Tips for a robust client:
- Send `PUT /api/tags/{mac}` when content changes, and retry if the AP does not respond.
- Do a periodic full send (`PUT /api/tags` in chunks of 50). It catches anything that was lost and costs no extra drawing, because the AP compares `content_id`.
- Read `GET /api/tags` now and then to see battery, "last seen" and new tags (`content_id: 0`).
- Or use [tools/esl-client](../tools/esl-client/README.md): CSV import (`sync`), scheduling (`--at`), `health`, `backup`/`restore`, `ota`, and the USB port (`--serial`).
- `tag/tools/label_design.py` turns a label design and your data into the body of `PUT /api/tags/{mac}`.
