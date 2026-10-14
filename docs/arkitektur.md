# Architecture

```
 Client (any software)                    ESP32-C6 (AP)                          Tags (ZBS243)
 ┌──────────────────────┐  HTTP + token  ┌──────────────────────────┐  802.15.4  ┌──────────────┐
 │ script, web shop,    │───────────────►│ HTTP API                 │  2.4 GHz   │ wakes every  │
 │ home automation, …   │  on change     │ content table in NVS     │◄──────────►│ 60 s         │
 │                      │◄───────────────│ 802.15.4 radio           │            │ POLL → reply │
 └──────────────────────┘  status        └──────────────────────────┘            │ draws text   │
                                                                                 └──────────────┘
```

## Data flow for one price change

1. The client sends `PUT /api/tags/{mac}` to the AP with the new text fields.
2. The AP calculates a new `content_id` (CRC32 of the content), stores it in NVS and replies immediately.
3. The next time the tag wakes (within 60 s) it sends `POLL` with its current `content_id`.
4. The AP sees that it is out of date and replies with `CONTENT` (all text fields in one packet).
5. The tag draws the screen (about 15–20 s for black/white/red) and goes back to sleep. There is no separate acknowledgement: the next POLL with the new `content_id` is the acknowledgement (see [protokoll.md](protokoll.md)).
6. The next time the client reads `GET /api/tags`, `shown_content_id` shows that the tag has the new content.

Worst case from change to finished screen: about 1.5 minutes (60 s poll + drawing), within requirement G1.

## Choices and rationale

### The tag draws itself (not images)
- One radio packet instead of about 80 packets per image: no block transfer, no resending of lost pieces, no EEPROM storage.
- Less radio time gives better battery life.
- The first step has the layouts "Price" and "Text" fixed in the firmware. They will be replaced by layout files from the design tool (see below). Images may come in version 2 with a separate message type.
- The tag does not need a whole image in RAM: it calculates one pixel column at a time and streams it to the display controller, which has its own image memory.

### Layout from the design tool (requirement G7)
- A designer makes the screen design in a separate design tool (not built yet). The tool exports one JSON file per layout and tag model: fields, positions, fonts, colours, alignment and truncation.
- Open: whether conversion to a compact binary format happens on the PC (before upload) or on the AP, and whether the tag ends up reading JSON or the binary format.
- Flow: design tool → layout file → AP (`PUT /api/layouts/{model}` or similar) → AP converts the file to a compact binary format → the tag fetches it over radio when the layout version has changed, and stores it in flash.
- `layout` in `PUT /api/tags` then refers to a layout name from the file, instead of the fixed `price`/`text`.
- This needs a new message type and block transfer in the radio protocol (a layout is larger than 110 bytes), and a fixed file format with a size limit. To be agreed between the tag and AP before it is specified in [protokoll.md](protokoll.md).
- The tag still draws the screen itself, so text changes are still one radio packet. A layout is only sent when it changes.
- The layout file belongs to the tag model, not to the individual tag. All tags of the same model use the same layout file, with content from the client.

### Two tag sizes: 2.9" and 1.54" (requirement G6)
- SoluM 2.9" (296×128, black/white/red) and SoluM 1.54" (152×152, black/white, red not confirmed) are both supported by the same AP, the same radio protocol, the same API and the same code.
- Tag: display driver, resolution, font choice and default layouts are parameterised per model. One firmware is built per size from the same source code (build option, e.g. `make fw MODEL=154`).
- AP: stores the model per tag and selects layout file and text limits by model. The model comes from the tag (field in POLL, requirement T18).
- Which display controller and colours the 1.54" tag has must be confirmed in phase 0, as for the 2.9".

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
    src/          main.c  app.c  proto.c  epd.c  font.c  draw.c  layout.c  hal/…
    fonts/        generated font tables + licence file for the source font
    designs/      label designs (JSON)
    tools/        build script, font generator, label design tool (Python)
  ap/             ESP-IDF project for ESP32-C6
    components/esl_core/  plain C core (content, protocol, store, API), tested on the PC
    main/         ESP-IDF glue: WiFi, HTTP server, radio, NVS
  shared/
    proto.h       shared packet definitions, used by both tag and AP
  docs/           these documents
```

`shared/proto.h` is the only file that is shared. It must compile with both SDCC (8051) and GCC (RISC-V), so use fixed-width types (`uint8_t` etc.), no bit-fields, and explicit little-endian.
