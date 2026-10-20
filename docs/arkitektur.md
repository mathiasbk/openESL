# Architecture

```
 Client (any software)                    ESP32-C6 (AP)                          Tags (ZBS243)
 ┌──────────────────────┐  HTTP + token  ┌──────────────────────────┐  802.15.4  ┌──────────────┐
 │ script, web shop,    │───────────────►│ HTTP API                 │  2.4 GHz   │ wakes every  │
 │ home automation, …   │  on change     │ content table in NVS     │◄──────────►│ 60 s         │
 │                      │◄───────────────│ draws screens, 802.15.4  │            │ POLL → reply │
 └──────────────────────┘  status        └──────────────────────────┘            │ shows image  │
                                                                                 └──────────────┘
```

## Data flow for one price change

1. The client sends `PUT /api/tags/{mac}` to the AP with the new text fields.
2. The AP calculates a new `content_id` (CRC32 of the content), stores it in NVS and replies immediately.
3. The next time the tag wakes (within 60 s) it sends `POLL` with the id of the image it shows.
4. The AP sees that it is out of date, draws the screen into a 1-bit image and replies with the first `IMG` block; the tag fetches the rest with `IMG_REQ` in the same wake-up (52 blocks raw on the 2.9", fewer when run-length coded).
5. The tag checks the image, stores it in its SPI flash, streams it to the display (a few seconds for a black/white full update; measured in phase 2) and goes back to sleep. There is no separate acknowledgement: the next POLL with the new image id is the acknowledgement (see [protokoll.md](protokoll.md)).
6. The next time the client reads `GET /api/tags`, `shown_content_id` shows that the tag has the new content.

The client can also send the content ahead of time with `at` (Unix time): the AP stores it and applies it itself at that time, also after a restart (`/api/schedule`, `/api/time`).

Worst case from change to finished screen: about 1.5 minutes (60 s poll + drawing), within requirement G1.

## Choices and rationale

### The AP draws, the tag shows an image
- Up to tag firmware 0.4.7 the tag drew the text itself from one CONTENT packet. That kept the radio short, but every layout, font and barcode had to fit the tag's 64 KB flash and its RAM.
- Now the AP draws the whole screen (`ap/components/esl_core/esl_render.c`, with the code the tag used) and sends it as a 1-bit image in IMG blocks, run-length coded when that is shorter. The tag stores it in its SPI flash (the same chip as the OTA slots) and streams it to the display controller a column at a time, so it never needs the whole image in RAM.
- The cost is radio time: about 50 blocks per new screen instead of one packet. Not measured yet; the estimate is a few mC per new screen, next to 22 mC for the display refresh ([strombudsjett.md](strombudsjett.md)).
- The image id is the CRC-32 of the image, so the same content gives the same image and is never sent twice, also after an AP restart. The AP keeps no images: it draws one when a tag needs it.
- The tag draws only its start-up and "No AP found" screens itself.

### Layout from the design tool (requirement G7)
- A designer makes the screen design in the label designer on the web page (`web/`, the Design tab), which can also send the file straight to the AP. It is one JSON layout file per tag model with up to 16 named layouts: up to 6 fields, positions, fonts, colours, alignment, fixed texts and one barcode, with up to 16 text lines and 10 boxes per screen ([protokoll.md](protokoll.md) part 3).
- Flow: layout file → AP (`PUT /api/layouts/{model}`) → the AP checks it against the model and compiles it to a compact binary (14 bytes per element, at most 768 bytes) → `esl_render.c` draws from it. The file never goes to the tags; they get the picture ([protokoll.md](protokoll.md) part 1, "Image transfer").
- **Conversion on the AP**, not on the PC: clients upload the same JSON the tool makes, and the AP can answer `422` with the exact place of an error. The designer on the web page and the previews on the PC (`tag/tools/render_host.py`) run the AP's compiler and drawing code, so they show exactly what a tag gets.
- `layout` in `PUT /api/tags` is `price`, `text` or a layout name from the file. The AP stores the name and looks it up in the file for each tag's model when it draws the screen; the same name can exist in both models' files.
- The limits (768 bytes, 16 layouts, 6 fields, 16 text lines and 10 boxes per screen) come from the tag's old drawing code and are still checked by the compiler.
- The layout file belongs to the tag model, not to the individual tag. All tags of the same model use the same layout file, with content from the client. A new file draws the screens of that model again.
- The tag keeps the last image in its SPI flash (requirement T12), so a restart shows it again and costs no new download.

### Two tag sizes: 2.9" and 1.54" (requirement G6)
- SoluM 2.9" (ST-GM29XX2, 296×128, black/white) and SoluM 1.54" (ST-GM16XX2, 200×200, black/white) are both supported by the same AP, the same radio protocol, the same API and the same code.
- Tag: display driver and resolution are parameterised per model. One firmware is built per size from the same source code (build option, e.g. `make fw MODEL=154`).
- AP: stores the model per tag, selects layout file and text limits by model, and draws the screen in the model's size. The model comes from the tag (field in POLL, requirement T18).
- The 1.54" display controller was confirmed in phase 0. Neither tag has red.

### The tag initiates all communication
- The tag has its radio off almost all the time. It can only receive for the few milliseconds after it has sent something itself.
- The AP has its radio on all the time (it is powered by USB).
- No clock synchronisation, no beacon. The simplest protocol that works.

### One ESP32-C6
- ESP-IDF has drivers for 802.15.4 (`esp_ieee802154`) and WiFi on the same chip. Both share the radio by time-division (coexistence).
- Fallback plan if WiFi and 802.15.4 interfere too much: use a second C6 as a pure radio connected by UART. See [risiko.md](risiko.md).

### The AP is only an API
- The AP has a simple HTTP API, and clients call it. Changes go out immediately instead of waiting for the next fetch.
- The AP is only an intermediate store: it does not know products or prices, only text fields per MAC. All business logic stays in the client.
- The AP compares content itself, so a client can resend everything (e.g. a daily full send) without the tags drawing unnecessarily.
- The client must be able to reach the AP: same local network with a fixed IP or the mDNS name `openesl.local`, otherwise a VPN. Do not expose the AP directly to the internet.

## Code structure

```
openesl/
  tag/            SDCC project for ZBS243
    Makefile
    src/          main.c  app.c  proto.c  image.c  epd.c  status.c  draw.c  font.c  ota.c  hal/…
    fonts/        generated font tables + licence file for the source font
    designs/      label designs (JSON)
    tools/        build script, font generator, label design tool (Python)
  ap/             ESP-IDF project for ESP32-C6
    components/esl_core/  plain C core (content, drawing, protocol, store, API), tested on the PC
    main/         ESP-IDF glue: WiFi and setup network, HTTP server, API over USB, radio, NVS,
                  scheduled content and NTP, webhook, OTA, LCD status screen
  shared/
    proto.h       shared packet definitions, used by both tag and AP
  sim/            tag firmware and AP core together on the PC, end-to-end tests in CI
  tools/          esl-client (example client), flash-tui (flashing in a jig), wemos-debug, power-budget
  web/            the web page: label designer, flashers, setup guide, API docs
  docs/           these documents
```

`shared/proto.h` is the only file that is shared. It must compile with both SDCC (8051) and GCC (RISC-V), so use fixed-width types (`uint8_t` etc.), no bit-fields, and explicit little-endian.
