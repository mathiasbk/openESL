# OpenESL: open firmware for the tag (ZBS243) and the AP (ESP32-C6)

Goal: show **price and text** on SoluM ST-GM29XX2 tags (2.9", 296×128, black/white) and ST-GM16XX2 tags (1.54", 200×200, black/white), sent from any software through a small HTTP API on the AP. All code is written from scratch for this project.

Principle: **as simple as possible, but finished and reliable in operation.** Everything not needed for "price and text through the API" is deferred to version 2.

## The documents

| File | Contents |
|---|---|
| [krav.md](krav.md) | Requirements with IDs (must / should / later) |
| [arkitektur.md](arkitektur.md) | How the parts fit together, and the main choices |
| [protokoll.md](protokoll.md) | Radio protocol between tag and AP, and the HTTP API on the AP |
| [tag-firmware.md](tag-firmware.md) | Modules, order of work, and what has to be found out about the ZBS243 |
| [ap-firmware.md](ap-firmware.md) | The AP on ESP32-C6 with ESP-IDF |
| [fremdriftsplan.md](fremdriftsplan.md) | Phases, milestones, estimates and checkpoints |
| [risiko.md](risiko.md) | Risks and what to do if they occur |
| [strombudsjett.md](strombudsjett.md) | Power budget for requirement T6: estimate, model and what to measure |
| [kildelogg.md](kildelogg.md) | Hardware facts the code uses, and where each one came from |

## The main choices (short version)

1. **The tag draws the screen itself.** The AP sends only the text fields (name, price, subtitle) in one radio packet of about 100 bytes. The tag has a built-in font and draws directly into the display controller's memory. That avoids image transfer in many blocks, storage in EEPROM and resending lost parts, which is the most complicated part of similar designs.
2. **The tag asks, the AP answers.** The tag wakes every 60 seconds, sends "anything new?", listens for a few milliseconds and goes back to sleep. No synchronisation or fixed time slots.
3. **One ESP32-C6 is the whole AP.** The C6 has both WiFi and an 802.15.4 radio, so no second chip is needed.
4. **The AP is only an API.** It has a small HTTP API (`PUT /api/tags/{mac}`, `GET /api/tags`). Clients push content to it. The client must be able to reach the AP: same network, or a VPN.
5. **No OTA in version 1.** New tag firmware is flashed with pogo pins.

## Equipment

- A ZBS243 flasher, for example [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher) on a D1 mini (ESP8266), with pogo pins to the tag's debug pads.
- One ESP32-C6 for the AP, and preferably a second one as a sniffer and fake tag during development.
- A few tags only for development, and a fixed setup (pogo jig) so you do not have to hold the pins.
- **Logic analyser** (8 channels, 24 MHz, a cheap "Saleae clone" works with PulseView). Almost essential for debugging SPI to the display and radio timing.
- **Current measurement in µA.** A multimeter with a µA range is enough for a rough check. A Nordic PPK2 makes sleep optimisation much easier. Can wait until phase 6.

## Status

The protocol, the AP core and HTTP API, and the drawing and state machine on the tag are written and tested on a PC. The ZBS243 hardware layer is still stubs. The next step is phase 0 in [fremdriftsplan.md](fremdriftsplan.md).
