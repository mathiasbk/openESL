# AP firmware (ESP32-C6, ESP-IDF)

One ESP32-C6 is the whole AP: an HTTP API over WiFi for any client software, and an 802.15.4 radio towards the tags. The AP holds no business logic, only text fields per tag. Plan and requirements: [docs/ap-firmware.md](../docs/ap-firmware.md). API and radio protocol: [docs/protokoll.md](../docs/protokoll.md).

**Status:** the core, the HTTP API (including layout files, per-tag settings and the status page), the radio receive path, the layout transfer and CONFIG to the tags are written, and the host tests pass: 2183 checks, 0 failures (gcc on Fedora, also with UBSan in trap mode). The tests run whole rounds on the air with a fake tag: client → API → store → 802.15.4 frame → fake tag → next POLL, including AP restarts from (fake) NVS, layout files downloaded block by block and settings sent with CONFIG. The ESP-IDF part (`main/`) builds without warnings with ESP-IDF v5.5.5 in all three radio modes (AP, sniffer, stub); image about 1 MB of the 1.5 MB app slot. It runs on an ESP32-C6 Super Mini with the API over USB (phase 5, see "Open points"); WiFi, CONFIG and the scheduled content are not tried on hardware yet; OTA (`POST /api/ota`) was tried on 2026-10-15, its rollback not.

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
    esl_sched     scheduled content ("at") and the AP's clock
    esl_json      small JSON parser and writer
    esl_health    health flags per tag (silent, unseen, draw_failed, low_battery, layout_failed,
                  content_error) for /api/health, the status page and the webhook
    esl_barcode   barcode checks (Code 128, EAN-13) for content the tags must encode
    esl_crc32     CRC32 for content_id and the layout version
    esl_api       HTTP API (routes, token, validation) and the status page, independent of the HTTP server
    esl_wifi      WiFi setup: checks, the /setup form and page, GET/PUT /api/wifi JSON
    esl_screen    the status screen's lines and their rendering (ESL_DISPLAY)
  main/                   ESP-IDF glue
    app_main.c  startup         store_nvs.c  mutex + NVS storage (tags and layouts)
    sched.c     scheduled content in NVS, its task, NTP
    wifi.c      WiFi, setup network, mDNS     http_api.c   esp_http_server -> esl_api_handle(), status page, /dashboard
    webui.py    packs web/'s dashboard into the firmware at build time (ESL_WEB_DASHBOARD)
    captive.c   DNS server of the setup network (captive portal)
    radio_ieee802154.c  AP and sniffer modes, calls esl_radio_handle()
    radio_stub.c        simulated tags (esl_faketag), no radio
    serial_api.c        the API over the USB port, "@ESL" frames next to the log
    health.c            posts the health report to the webhook when it changes (ESL_WEBHOOK)
    ota.c               new AP firmware through POST /api/ota (ESL_OTA)
    display.c           status on a small ST7789 LCD (ESL_DISPLAY, boards/c6-supermini-lcd)
  boards/                 one folder per AP board: board.json, sdkconfig.defaults, partitions.csv (boards/README.md)
    c6-supermini/       ESP32-C6, 4 MB: NVS of 468 KB, two app slots for OTA, 128 KB for tag firmware (the default)
    c6-supermini-lcd/   the same with the 0.96" status screen
    c5-wroom-1u-n16r8/  ESP32-C5 with 16 MB flash and 8 MB PSRAM, external antenna (experimental)
  build.py                builds one or every board into out/ (merged image and OTA image)
  test/                   host unit tests (Makefile)
```

## How content and diffing work

- `content_id` = CRC32 of layout, flags and text fields (never 0). The same content twice gives `changed:false`, no NVS write and no redraw on the tag.
- Trailing empty fields are removed before the id is computed, so `["a",""]` and `["a"]` are the same content.
- Content with a layout from a layout file is stored by name; the id covers the name, so it is the same on both models. The store turns the name into "layout n of the file" for the tag's model each time it answers a POLL.
- Text is converted to Latin-1. The fields share 96 bytes (`ESL_CONTENT_FIELD_SPACE` in `shared/proto.h`: what is left of the 110-byte payload after the header and the 4 bytes reserved for a MIC), one length byte per field plus the text, so the AP rejects more than 92 bytes of text with four fields, or 90 with six, with `422`. Empty fields at the end are dropped first and cost nothing. `tag/tools/label_design.py` checks the same limit.
- A tag is `pending` until it has reported the correct `content_id` in a POLL. After an AP restart the content is read from NVS; a tag that already shows it gets IDLE, not a redraw.
- Every tag the AP has heard stays in the table across a restart or an OTA update, also one that only polls and has no content: what it said last (model, firmware, battery, signal) and the wall-clock time of its last POLL are kept in NVS (`s<slot>` in `esl_tags`, `esl_persist.h`). The record is written when a tag is new or changes model or firmware, every hour for the tags heard since, and right before an OTA restart, so one small write per tag per hour at most. After the restart such a tag shows those values with `seen_s_ago` counted from that time (once the AP knows the time again), and after the usual quiet period it is flagged `unseen` like any tag that has not polled since the restart; a tag that went quiet before the update is not forgotten. Erasing the flash (the web flasher's erase, `idf.py erase-flash`) still clears everything; use `esl-client backup` and `restore` for that.
- Unknown tags that send POLL are added to the table without content (`content_id:0`). If the table is full, the tag that was seen longest ago and has no content is replaced when a client sends content for a new MAC.

## HTTP API

The reference is [docs/openapi.yaml](../docs/openapi.yaml), shown with test requests in the API tab of the web page; [docs/protokoll.md](../docs/protokoll.md) part 2 has an overview. `make openapi` in `test/` checks the spec against `esl_api.c`. Every call requires `Authorization: Bearer <token>` (an empty token always gives `401`). Five wrong tokens in a row lock the API for 60 s: every request, also with the right token, gets `429` until then, and `POST /api/ota` counts the same way (`esl_api_auth_check()`, `esl_api_lock_t`). The USB port has no token and no lockout.

```bash
curl -X PUT http://openesl.local/api/tags/00000123456789A8 \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"layout":"price","fields":["Milk 1 L","24.90","per litre 24.90"],"price_highlight":true}'

curl -H "Authorization: Bearer $TOKEN" http://openesl.local/api/tags
```

Every tag object also has `"model"`, `"draw_failed"`, `"layout_version"`, `"layout_pending"`, `"layout_failed"` and `"content_error"` (the `Tag` schema in openapi.yaml).

### Layout files (krav A12-A14)

The file format, the checks and the endpoints are in [docs/protokoll.md](../docs/protokoll.md) part 3 and part 2; the radio transfer in part 1. In short:

```bash
curl -X PUT http://openesl.local/api/layouts/2.9 -H "Authorization: Bearer $TOKEN" \
  --data-binary @../tag/designs/layouts/2.9.json
curl -X PUT http://openesl.local/api/tags/00000123456789A8 -H "Authorization: Bearer $TOKEN" \
  -d '{"layout":"shelf","fields":["Kaffe 500 g","49,90","kr 99,80/kg","Tilbud"],"price_highlight":true}'
```

- `PUT /api/layouts/{model}` compiles the file for that model (`esl_layout_compile`) and answers `422` with the place and reason if it would not work on the tag (outside the screen, unknown font, box lower than its font, more than 16 text lines or 10 rectangles per layout, more than 768 bytes for the tag, ...). The JSON is stored in NVS (namespace `esl_layouts`) and compiled again at boot; only the compiled file (about 1.4 KB) and the JSON's length and CRC-32 stay in RAM, and `GET` reads the JSON back from NVS.
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

## Scheduled content

`"at"` (Unix seconds, UTC) in `PUT /api/tags` sets content at a given time instead of now: new prices from Monday 07:00, or a sale that starts and one that ends it. The AP applies it itself, so the client does not need to run then (`esl-client sync prices.csv --at "2026-10-19 07:00"`).

- **Clock:** the AP has none that survives a restart. It gets the time from NTP once it is on WiFi (`ESL_NTP_SERVER`, default `pool.ntp.org`), or from `PUT /api/time`; `esl-client` sets it from the computer before it schedules anything if the AP does not know it. Without a known time `at` gives `409`, and after a restart without NTP nothing is applied until the time is set again; then the entries that came due meanwhile are applied, the latest per tag.
- **Room:** `ESL_MAX_SCHEDULED` (menuconfig, default 256, 0 turns it off) entries for all tags together; a tag can have several (the same tag and time replaces). Each costs 16 bytes of RAM and up to about 160 bytes of NVS (blob `s<slot>` in namespace `esl_sched`) while it waits, and is erased once applied.
- **Applying:** a task (`main/sched.c`) checks every second and sets the content as `PUT /api/tags` would have; the tag then gets it at its next poll.
- **Waking the tag:** after each change (and once a minute) the task tells the store when each tag's next entry is due (`esl_store_note_due`). A tag that polls before then gets `next_poll_s` = time left + 2 s instead of its interval, if that is shorter, so the price shows a few seconds after `at` even with a long interval. It needs one poll between scheduling and `at`, so schedule at least one interval ahead. The radio path does not take the schedule's lock (which is held during NVS writes); the due time sits in the tag table.
- Content sent without `at` leaves scheduled entries in place; `DELETE /api/schedule/{mac}` removes them, and `DELETE /api/tags/{mac}` removes them with the tag. `GET /api/schedule` lists them with their content, `GET /api/tags/{mac}` gives the times under `scheduled`.
## Health and webhook

`GET /api/health` lists the tags that need attention, for monitoring: `silent` (no poll for `silent_s`, default 3 x the interval the AP sends the tag), `unseen` (in the table from NVS, but no poll since the AP started for that long), `draw_failed`, `low_battery` (below `battery_mv`, default 2500; a tag that reports 0 cannot measure it), `layout_failed` and `content_error`. `?silent_s=600&battery_mv=2400` changes the thresholds for one call, and `ok` says whether anything is listed. `esl-client health` prints it and exits 1 when there are problems.

With `PUT /api/config {"webhook":"http://host:port/path"}` (`esl-client config --webhook ...`, kept in NVS `cfg/webhook`) the AP posts the same report, with `changed` = how many tags' problems differ from the last report, to that URL whenever it changes; `health.c` checks every `ESL_HEALTH_PERIOD_S` (30 s). Plain HTTP, no retries: a failed POST is logged and the change goes out with the next one. The logic is in `esl_health.c` and `esl_api_health()` (host-tested); the task and `esp_http_client` are the ESP-IDF side (`ESL_WEBHOOK`, on by default).

### Over the USB port

With `ESL_SERIAL_API` (on by default; the console is on the built-in USB-Serial-JTAG port) the same API also answers on the USB port, for setups without WiFi. Requests and replies are framed lines in the log stream:

```
host -> AP:  @ESL <seq> <METHOD> <uri> <len>\n<len bytes body>
AP -> host:  @ESL <seq> <status> <len> <len bytes body>\n
```

A reply that grows with the tag table (`GET /api/tags`) comes in pieces of up to 23 KB: every piece but the last has `+` after its length (`@ESL 7 200 22912+ {"ap":...`), and the host joins them; log lines can come between pieces, never inside one. No token is needed there, since whoever has the cable has the device. WiFi can be set this way too: `esl-client --serial /dev/ttyACM0 wifi "Network" --password ...` (`PUT /api/wifi`). `tools/esl-client --serial /dev/ttyACM0 ...` speaks this. HTTP and serial requests take turns through one mutex (`api_handle()` in http_api.c).

## OTA: new AP firmware (krav A10)

`POST /api/ota` with the app image as the body (`esl-client ota build/esl_ap.bin`, or `openesl-ap-esp32c6-<version>-ota.bin` from a release, `openesl-ap-esp32c6-lcd-<version>-ota.bin` on a board with the LCD; not the merged `openesl-ap...bin`, which has the bootloader in front) writes it to the app slot that is not running (`ota.c`, `esp_ota_*`). The last byte makes that slot the boot slot and the AP restarts a second after the reply; tag content, settings, every tag's last poll (saved just before the restart) and layout files stay, since they are in NVS. `GET /api/ota` shows which slot runs and what the other holds. Over WiFi the whole image goes in one request; over the USB port it goes in parts (`?offset=N&total=T`, `esl-client` sends 16 KB at a time), since the serial driver drops bytes when its buffer is full. With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` the new image must confirm itself: once everything in `app_main` has started, it is kept as soon as WiFi has an address or the API has answered a request over WiFi or USB (`ota_watch_running()`, `ota_confirm_running()`). If it never gets there within three minutes (it broke WiFi, say), the AP restarts into the old slot, so an update over WiFi cannot leave the AP reachable only over USB; if it crashes before, the next reset boots the old slot. Without a WiFi network to join it is kept once it has started. `GET /api/ota` says whether the running image is kept yet (`confirmed`) and whether it has the status screen (`display`), which the dashboard uses to offer the right release image. Flashing over USB (`idf.py flash`, the web flasher) still works and always writes `ota_0`; `ESL_OTA` (on by default) turns the endpoint off, and a one-slot partition table answers `501`.

**Tag firmware.** `PUT /api/tag-firmware` (`esl-client tag-ota openesl-tag-<model>-<version>-ota.bin`) stores firmware for one tag model in the `tagfw` partition (64 KB per model, `tagfw.c`, `esl_tagfw.c`), and the AP sends it to the tags of that model over the radio, block by block, when they have nothing else waiting ([docs/tag-ota.md](../docs/tag-ota.md), [docs/protokoll.md](../docs/protokoll.md) "Firmware transfer"). `GET /api/tag-firmware` shows each image and how many tags run it, still need it, rejected it or cannot take updates. The image survives restarts. The partition sits after `ota_1` in space that was free, so nothing else moves; an AP that got its firmware through `POST /api/ota` still has the old table without it and answers `404` until it is flashed over USB once.

## WiFi setup

With no network saved (first start, or after `PUT /api/wifi {"ssid":null}`), the AP opens its own network `openesl-XXXX` and a form at `http://192.168.4.1/setup` (`/` redirects there): pick the network from the scan list, enter the password, and choose the API token if none is set. The AP saves both in NVS (`cfg/ssid`, `cfg/pass`, `cfg/token`) and joins at once. If the saved network gives no IP for `ESL_WIFI_SETUP_AFTER_S` (180 s), the setup network opens again next to it, so a changed WiFi password can be fixed without a cable; it closes a minute after the AP has joined. While it is open the AP is a captive portal (`ESL_WIFI_CAPTIVE`, `main/captive.c`): a small DNS server on 192.168.4.1 answers every name with that address, and any page the AP does not have, asked for by another name, redirects to the form, so a phone that joins `openesl-XXXX` shows its "sign in to network" page with the form in it. The DNS server runs only while the setup network is open. While the setup network is open the AP retries the saved one every 20 s (joining hops channels, which drops phones on the setup network). Once a token is set, the form asks for it before changing anything, so an open setup network only lets strangers configure an AP that has no token yet, and wrong tokens in the form count in the same lockout as the API (five in a row lock both for 60 s); set `ESL_WIFI_SETUP_PASSWORD` for WPA2 on it. The same settings are in `GET/PUT /api/wifi` and over the USB port: `PUT /api/wifi` also takes `"token"` for a new API token (with or without the network), and `GET` says whether one is set (`"token_set"`, never the token). `"hostname"` sets the AP's name on the network, which it gives the router over DHCP and uses for mDNS (`<name>.local`; NVS `cfg/hostname`, `openesl` from `ESL_MDNS_HOSTNAME` until changed); the AP rejoins at once so the router learns it. Over the USB port that needs no old token, so a lost token is replaced with `esl_client.py --serial /dev/ttyACM0 wifi --new-token ...`, and the web page sets the WiFi and the token over the same cable right after flashing (Flash AP tab). The checks, the form and the pages are in `esl_wifi.c` (host-tested); `wifi.c` is the ESP-IDF side.

## Status screen (boards with an LCD)

For ESP32-C6 boards with a 0.96" 160x80 ST7789 screen, `ESL_DISPLAY` shows four lines, refreshed every 2 s:

```
OpenESL 0.4.0          ch 25     firmware and 802.15.4 channel
12 tags, 2 pending               yellow while tags have not fetched their content
192.168.1.40                     green; "Setup: openesl-1A2B" or "Joining <network>" in yellow, "WiFi off" in red
up 3 d 4 h, 9 seen               uptime and tags heard from in the last 10 min; "192.168.4.1/setup" on the setup network
```

It is off by default, so the C6 Super Mini build is unchanged. Build with the screen:

```sh
python build.py c6-supermini-lcd
idf.py -B build-c6-supermini-lcd -p /dev/ttyACM0 flash monitor
```

CI builds the same as `openesl-ap-lcd.bin` (and releases attach it). The pins in `boards/c6-supermini-lcd/sdkconfig.defaults` (MOSI 6, SCLK 7, CS 14, DC 15, RST 21, backlight 22) are a guess from the Waveshare ESP32-C6-LCD boards, since the seller gives no pinout; check them against the board's silkscreen or schematic. At start the screen shows a yellow frame around the edge for two seconds: if a side is missing or doubled, change `ESL_LCD_GAP_X`/`ESL_LCD_GAP_Y` (default 1/26, the usual offset of 80x160 panels); wrong colours mean `ESL_LCD_BGR` or `ESL_LCD_INVERT`, upside down means `ESL_LCD_ROTATE_270`. The text comes from `esl_screen.c` in the tag's small font (`tag/fonts/font_small.c`), host-tested; `ESL_SCREEN_DUMP=dir make` in `test/` writes the test screens as pictures. `display.c` only drives the panel (`esp_lcd`).

## How many tags

`ESL_MAX_TAGS` (menuconfig, default 256, at most 1000) is the size of the tag table. What each slot costs:

- **RAM:** about 220 bytes per slot for the whole run (`sizeof(esl_tag_t)`): 56 KB at 256, 220 KB at 1000. The boot log ends with `free heap N bytes, tag table M bytes`; keep a good margin for WiFi and HTTP. 1000 is likely too much for the C6 next to WiFi, a few hundred is fine.
- **NVS:** up to about 320 bytes per tag once it has content and settings (two blobs, 32-byte NVS entries), plus one layout file of up to 8 KB per tag model that has one (in RAM only its compiled form, about 1.4 KB; a model without a file holds none) and up to about 160 bytes per waiting scheduled entry (40 KB at the default 256, "Scheduled content"). The NVS partition is 468 KB, so 1000 tags fit.
- **API replies:** `GET /api/tags` and the status page are streamed (HTTP chunks, `+` pieces on the USB port) through one 23 KB buffer per request, so they do not grow with the table. A batch (`PUT /api/tags`) is still at most 50 tags.
- **Radio:** a tag's POLL exchange takes 3-6 ms on the air, and tags send without listening for a free channel first (one retry after 2-10 ms). With the default 60 s interval, 64 tags keep the channel busy 0.5 % of the time and about 1 % of first POLLs collide, 200 tags about 3 %, 1000 tags about 15 % (the retry catches most). Beyond that: a longer interval, more APs on different channels, or CCA on the tag.

## Status page

`http://openesl.local/` shows the layout files per model (version, size for the tags, layout names), the webhook when one is set, and a table of all tags (MAC, model, last seen, battery, RSSI, firmware, layout and content, pending/shown, draw failed, why named content is not sent, layout file pending/rejected, and an Attention column with the same flags as `GET /api/health`: silent, unseen, low battery, ...; the heading counts the tags that need it) and refreshes every 30 s (requirement A6). It needs no token, since it only reads; turn it off with `ESL_STATUS_PAGE` in menuconfig.

## Dashboard on the AP

`http://openesl.local/dashboard` is the web page's Dashboard tab, served by the AP itself: the AP's state and WiFi, every tag with what needs attention, and the firmware card (AP OTA and tag firmware: the latest release, fetched from the web page, or a file). A page on https (GitHub Pages) may not call the AP's plain http API, which is why the page on the web needs USB or the bridge in `tools/esl-client`; a page that comes from the AP may. It asks for the API token once and keeps it in that browser for that address (localStorage). The status page links to it.

The files are not a copy: `main/webui.py` takes the `<main id="tab-dashboard">` part of `web/index.html`, `web/css/style.css` and the dashboard's modules from `web/js/` (`apdash.js` starts it) at build time, gzips them (about 23 KB) into a C table in flash, and `http_api.c` serves `/dashboard` and `/ui/...` with `Content-Encoding: gzip`. No RAM is used for them. `ESL_WEB_DASHBOARD` in menuconfig turns it off. A change to the dashboard on the web page reaches the AP with its next firmware.

## Tag model in POLL

Byte 11 of POLL, formerly reserved and always 0, is the tag model (`ESL_POLL_OFF_MODEL` in `shared/proto.h`): 0 = unknown, 1 = 2.9", 2 = 1.54". Firmware that still sends 0 works as before and shows `"model":null`. Bytes 12-13 are the settings version `cfg_ver` (see "Tag settings"), bytes 14-17 are the tag's layout file version (protokoll.md part 1).

## Configuration

`idf.py menuconfig` -> «OpenESL AP»: WiFi, setup network (`ESL_WIFI_SETUP_AFTER_S`, `ESL_WIFI_SETUP_PASSWORD`, `ESL_WIFI_CAPTIVE`), token, mDNS name (`openesl`), status page, channel (25), PAN ID (0x4553), `ESL_NEXT_POLL_S` (0 = the tags keep their own interval), max tags, TX power, radio mode (`ESL_STUB_TAGS`, `ESL_STUB_INTERVAL_S` for Stub), the API over USB (`ESL_SERIAL_API`), scheduled content (`ESL_MAX_SCHEDULED`, `ESL_NTP_SERVER`), health and webhook (`ESL_HEALTH_PERIOD_S`, `ESL_WEBHOOK`), OTA (`ESL_OTA`) and the LCD (`ESL_DISPLAY`, `ESL_LCD_*`). `ESL_NEXT_POLL_S` is the built-in default interval; `PUT /api/config` overrides it at run time. SSID, password and token can be overridden in NVS (namespace `cfg`, keys `ssid`, `pass`, `token`), which the setup page and `PUT /api/wifi` write.

Radio modes: `AP` (normal), `Sniffer` (logs every frame on the channel, does not answer, no WiFi), `Stub` (simulated tags `00000000FAC70001`..., for testing the API with curl before the tag radio works; their frames go through the same `esl_radio_handle()` as real ones, and they download layout files like the tag firmware does).

## Watchdog

The ESP-IDF task watchdog restarts the AP when a watched task has not checked in for 10 s (`sdkconfig.defaults`). It watches the idle task, the radio task (`radio_ieee802154.c`, or the stub task in `radio_stub.c`) and the serial API task (`serial_api.c`). Each of these waits at most a second for work before it checks in again, so only a task that is stuck (a lock never released, a driver call that never returns, a loop that never yields) trips it. The HTTP server task belongs to ESP-IDF and is not watched. After such a restart `GET /api/tags` shows `"reset":"task_wdt"` under `"ap"`, and the tag table and content come back from NVS.

A `sdkconfig` made before this change keeps its old watchdog settings (no restart, only a log line); delete it or set them in menuconfig (Component config -> ESP System Settings).

## Build and flash

Pinned to ESP-IDF v5.5.5 (requirement G5); `dependencies.lock` pins the managed components (for the ESP32-C6; other chips get `dependencies.lock.<chip>` on their first build, to be committed too). Each board in `boards/` has its chip, flash size, partition table and options; see [boards/README.md](boards/README.md). The C6 Super Mini's partition table (`boards/c6-supermini/partitions.csv`) needs 4 MB flash (the ESP32-C6FH4 of the Super Mini and the LCD boards): 468 KB NVS and two 1.5 MB app slots `ota_0`/`ota_1` for OTA (the image is about 1 MB). A board flashed with an older table (one app slot, or 84 KB NVS with the app at 0x20000) must have its flash erased first (`idf.py erase-flash`, or the web flasher, which always erases), since the partitions moved. Erasing takes the stored tags, layout files and settings with it: `esl-client backup ap.json` before and `esl-client restore ap.json` after keeps them (`tools/esl-client/README.md`).

```bash
cd ap
idf.py menuconfig                  # the ESP32-C6 Super Mini (boards/c6-supermini), as before
idf.py build flash monitor
python build.py --list             # the boards
python build.py c5-wroom-1u-n16r8  # one board: build-<board>/, and out/<artifact>.bin + -ota.bin
idf.py -B build-c5-wroom-1u-n16r8 -p /dev/ttyACM0 flash monitor
```

The chip comes from the board (`-DBOARD=...`, default `c6-supermini`), so `idf.py set-target` is not needed.

## Tests on the PC

```bash
cd ap/test
make          # build and run (needs cc/gcc/clang)
make SAN=1    # with AddressSanitizer and UBSan (needs libasan/libubsan; on Fedora: dnf install libasan libubsan)
```

Without gcc/clang on Windows: `pip install ziglang`, then build for Linux with `python -m ziglang cc -target x86_64-linux-musl ...` (zig's native Windows target fails in its mingw libc). Run the binary in WSL.

Covers CRC32, text conversion, content, messages and frames, the NVS format, JSON, the table (including locking and persistence), layout files (the compiler with the example files in `tag/designs/layouts/` and every error message, storage, blocks), the whole API (auth, validation, batches, limits, layouts and named content, settings, status page), WiFi setup (form decoding, the setup page and its escaping, token rules, `/api/wifi`), the status screen (every WiFi state, long text cut before the edge, odd sizes), and the radio path end to end with fake tags: new tag, same content (no redraw), new content, lost CONTENT, AP restart, tag restart, next_poll_s, per-tag settings over CONFIG (the fake tag uses the new interval, confirms with `cfg_ver`, gets them again after a tag restart but not after an AP restart; an old-firmware fake tag never gets CONFIG but still gets its interval), the run-time default interval in NVS, 24 tags in one batch (G3), frames that must be ignored, scheduled content and the clock (`test_sched.c`), the health report and the webhook body, the token lockout (also for the `/setup` form), OTA routing, the six-field limit, and layout files over the air: download in blocks then content drawn with the downloaded layout in the same round, no second download of the same version, a lost block resumed in the next round, the file changed during a download, a rejected file, a deleted file, firmware without layout files, 24 tags of both models. One test gives a tag a layout file, content and settings at once and checks the order (layout and content in one wake-up, CONFIG in the next; layout then CONFIG in one wake-up when the content did not change). The test reads `../../tag/designs/layouts/*.json`, so run it from `ap/test`. Not covered here: the ESP-IDF glue in `main/` (built in CI, run on the jig in phase 5), radio timing (must be measured on hardware, requirement A3), WiFi and coexistence (phase 4), and OTA writing and rollback on real flash.

## Open points

- **Tested on hardware (ESP32-C6 Super Mini, ESP-IDF v5.5.5, 1.54" jig tag):** the phase 5 test matrix, see "Results on hardware" in [docs/fremdriftsplan.md](../docs/fremdriftsplan.md). CONFIG and per-tag settings are not tested on hardware yet.
- **WiFi on the jig's C6 Super Mini is weak:** it was not dead but turned off by 802.15.4 until coexistence was switched on (v0.3.1). The board's chip antenna still hears the router at only -75 to -85 dBm where a PC hears -65, so place it near the router, on a USB extension cable away from the computer's USB 3 ports.
- **Battery in POLL:** the 1.54" tag reports 0 mV (= unknown), since its panel does not answer the voltage-detector reads; see issue #4.
- **Another model** only needs a new `ESL_MODEL_*` value and screen size in `shared/proto.h`, and a name in `esl_model_name()` / `esl_model_size()`.
- **Validation per model (A13):** layout files are checked against the model's screen and what the tag can draw, and named content against the tag's model. The text limit (96 bytes with the length bytes) is the same for both sizes; there is no per-field or per-line length check (the tag shrinks and truncates text that does not fit).
- **Layout transfer timing:** on hardware a 684-byte layout (8 blocks) arrived in one wake-up without retries: about 50 ms from the first LAYOUT to the last block and 80 ms to the POLL that fetches the content, the AP answering each LAYOUT_REQ in under 40 µs. The full 768 bytes (9 blocks) is not tried; the file limit of 8192 bytes JSON came first.
- **Tag settings:** the tag firmware keeps CONFIG in flash (`hal_store_*`), so a restarted tag uses its settings at once; the fake tag does not (it reports `cfg_ver` 0 after a restart and gets one extra CONFIG, which the tests rely on). The fake tag and `radio_stub.c` do not sleep per tag; the stub still polls all simulated tags every `ESL_STUB_INTERVAL_S`.
- **Development builds with layouts but without CONFIG** (the layout branch before the merge) send 0 in POLL bytes 12–13. The AP reads that as "understands CONFIG, built-in settings", so such a tag with its own settings would get CONFIG, which it drops. No such firmware was released; flash the current one.
- **WiFi setup** (setup network, `/setup`, `PUT /api/wifi`, the captive portal) is host-tested and builds, but not tried on hardware: the jig's C6 has dead WiFi.
- **OTA (A10)**: writing a slot and the restart into it work on the Super Mini (2026-10-15, both ways between 0.2.0 and 0.2.91, from the web page); the rollback of a broken image is not tried on hardware yet.
