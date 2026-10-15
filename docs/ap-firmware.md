# AP firmware (ESP32-C6, ESP-IDF)

## Tools
- **ESP-IDF** (latest stable 5.x, pinned version). Apache 2.0, fine commercially.
- Components: `esp_wifi`, `esp_http_server` (the API), `cJSON` (ships with ESP-IDF), `mdns` (the name `openesl.local`), `nvs_flash`, `esp_ieee802154`.
- Not Arduino: ESP-IDF gives direct access to the 802.15.4 driver and the coexistence settings.

## Tasks
1. **Radio** (`radio.c`): `esp_ieee802154_enable()`, set channel, PAN ID and short address `0x0000`, `rx_when_idle = true`. Reception happens in the callback `esp_ieee802154_receive_done()`. It runs in interrupt context, so copy the frame to a queue and let a high-priority task reply.
2. **Reply quickly** (`esl_radio.c`, `esl_store.c`): build the reply (IDLE, CONTENT or a LAYOUT block) straight from the table in RAM; it is a few hundred bytes of copying. Target: under 5 ms from the received POLL to the sent reply (requirement A3).
3. **Storage** (`store.c`): table MAC → {content, content_id, last seen, battery, RSSI, fw}. Content is stored in NVS when it changes (requirement A4). Status is kept in RAM.
4. **HTTP API** (`api.c`): `esp_http_server` with the endpoints in [protokoll.md](protokoll.md) part 2. Check the token, parse JSON with cJSON, convert UTF-8 to Latin-1, validate length, calculate CRC32, store, and build the finished CONTENT reply for the radio. Protect the table with a mutex, since HTTP and radio run in separate tasks.
5. **WiFi setup**: version 1 can keep SSID, password and token in `menuconfig`/NVS. A setup portal can come later. Start mDNS with the name `openesl`.
6. **Status page** (SHOULD): a simple HTML table at `/`, with the same data as `GET /api/tags`.
7. **Layout files per model** (requirements A12, G7): store the layout files for 2.9" and 1.54" in NVS, check and compile each one for its model when it is uploaded ([protokoll.md](protokoll.md) part 3), and send it to a tag in blocks when the version in its POLL is not the AP's (part 1, "Layout transfer"). Done in `esl_layout` / `esl_layout_compile`.
8. **Model per tag** (requirements A13, T18): store the model the tag reports in POLL, and validate the layout name and the number of fields against that model's file.

## Coexistence (WiFi + 802.15.4 on the same chip)
- The C6 shares one 2.4 GHz radio between WiFi and 802.15.4 by time-division. Enable software coexistence in `menuconfig`.
- WiFi is mostly quiet; traffic only comes when a client sends content or reads status, so the conflict should be small. Turn off WiFi power save (`esp_wifi_set_ps(WIFI_PS_NONE)`) so the API responds quickly. The tag's single extra attempt covers most collisions.
- Measure packet loss in phase 4. If it is above a few percent, move the radio to the second C6 (connected by UART) as [risiko.md](risiko.md) describes.

## Test tools you get almost for free
- **Sniffer:** the same radio code in promiscuous mode, writing every frame on the channel to serial. The first thing you build; you need it to see whether the tag actually transmits.
- **Fake tag:** the second C6 runs a small program that sends POLL like a tag. That lets you develop and test the AP and a client before the tag's radio works.

## Writing a client
The AP is only an API, so the client can be anything that speaks HTTP. Tips for a robust client:
- Send `PUT /api/tags/{mac}` when content changes, and retry if the AP does not respond.
- Do a periodic full send (`PUT /api/tags` in chunks of 50). It catches anything that was lost and costs no extra drawing, because the AP compares `content_id`.
- Read `GET /api/tags` now and then to see battery, "last seen" and new tags (`content_id: 0`).
- `tag/tools/label_design.py` turns a label design and your data into the body of `PUT /api/tags/{mac}`.
