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
| [access-points.md](access-points.md) | The boards the AP runs on (C6, C5, S3 + C6), with limits, pros and cons, and the S3 + C6 wiring |
| [ap-radio-link.md](ap-radio-link.md) | The AP on an ESP32-S3 with an ESP32-C6 as its 802.15.4 radio over UART: wiring and link protocol (experimental) |
| [ap-netradio.md](ap-netradio.md) | The AP on an ESP32-S3 with a wireless ESP32-C6 radio on the same WiFi network: how it works, its limits and the measurements behind them (planned) |
| [fremdriftsplan.md](fremdriftsplan.md) | Phases, milestones, estimates and checkpoints |
| [risiko.md](risiko.md) | Risks and what to do if they occur |
| [strombudsjett.md](strombudsjett.md) | Power budget for requirement T6: estimate, model and what to measure |
| [tag-ota.md](tag-ota.md) | Firmware update over radio for the tags (T15): bootloader, SPI flash slots, rollback, and the SPI flash probe |
| [home-assistant.md](home-assistant.md) | Home Assistant as a client: 3D printer status on a tag with a `rest_command` and an automation, and a layout with a progress bar |
| [openapi.yaml](openapi.yaml) | The HTTP API as OpenAPI 3.1, the reference; checked against the AP code in CI and shown in the API tab of the web page |
| [release.md](release.md) | How a release is made: the pull requests, the hardware checklist, rollout to the tags and what to do when a release is bad |
| [kildelogg.md](kildelogg.md) | Hardware facts the code uses, and where each one came from |
| [st-gm16xx2.md](st-gm16xx2.md) | The 1.6" ST-GM16XX2 (200×200 BW): panel detection, init and refresh, and what its controller does not do |
| [solum-remote.md](solum-remote.md) | The SoluM ESL remote SLT-RC004: its radio frames and button codes, sniffed on the AP, and why openESL does not use it yet |

## The main choices (short version)

1. **The AP draws the screen, the tag shows it.** The client still sends only the text fields and a layout name. The AP draws the whole screen into a 1-bit image (4736 bytes on the 2.9", 5000 on the 1.54", run-length coded when that is shorter) and sends it in blocks when the tag polls; the tag stores it in its SPI flash and streams it to the display controller. Layout files stay on the AP. Up to 0.4.7 the tag drew the text itself from one radio packet; that kept the radio short, but every layout had to fit the tag's flash and RAM ([arkitektur.md](arkitektur.md)).
2. **The tag asks, the AP answers.** The tag wakes every 60 seconds, sends "anything new?", listens for a few milliseconds and goes back to sleep. No synchronisation or fixed time slots.
3. **One ESP32-C6 is the whole AP.** The C6 has both WiFi and an 802.15.4 radio, so no second chip is needed.
4. **The AP is only an API.** It has a small HTTP API (`PUT /api/tags/{mac}`, `GET /api/tags`). Clients push content to it. The client must be able to reach the AP: same network, or a VPN.
5. **OTA for the tags came early.** Tags flashed once with the bootloader (pogo pins) take new firmware over the radio through the AP (`PUT /api/tag-firmware`, `esl-client tag-ota`, [tag-ota.md](tag-ota.md)); it is on without radio security (T16), by decision. The AP itself is updated over the API (`POST /api/ota`, `esl-client ota`).

## Equipment

- A ZBS243 flasher, for example [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher) on a D1 mini (ESP8266), with pogo pins to the tag's debug pads.
- One ESP32-C6 for the AP, and preferably a second one as a sniffer and fake tag during development.
- A few tags only for development, and a fixed setup (pogo jig) so you do not have to hold the pins.
- **Logic analyser** (8 channels, 24 MHz, a cheap "Saleae clone" works with PulseView). Almost essential for debugging SPI to the display and radio timing.
- **Current measurement in µA.** A multimeter with a µA range is enough for a rough check. A Nordic PPK2 makes sleep optimisation much easier. Can wait until phase 6.

## Status

The tag firmware runs on the SoluM 1.54" and 2.9" and the ESP32-C6 AP serves them over the API; the first release is v0.1.0. The open rows in [fremdriftsplan.md](fremdriftsplan.md) say what is left, most of it checks on hardware.
