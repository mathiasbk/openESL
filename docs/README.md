# Developer documentation

OpenESL is open firmware for electronic shelf labels: the tag firmware (ZBS243), the access point (ESP32-C6 and other ESP32 boards), a web page, and tools. All code is written from scratch for this project.

This folder is for developers. To **use** OpenESL, read the [wiki](https://github.com/mathiasbk/openESL/wiki) instead.

**New here?** Start with the [code map](code-map.md): where the code for what lives, how the parts talk to each other, and where to start for a change.

## How it works

| File | Contents |
|---|---|
| [code-map.md](code-map.md) | The folders and modules, how the parts talk, "want to change X, start in Y", and which tests cover what |
| [architecture.md](architecture.md) | How the parts fit together, and the main choices |
| [protocol.md](protocol.md) | The radio protocol between tag and AP (part 1), the HTTP API (part 2) and the layout file format (part 3) |
| [openapi.yaml](openapi.yaml) | The HTTP API as OpenAPI 3.1, the reference; checked against the AP code in CI and shown in the API tab of the web page |
| [requirements.md](requirements.md) | The requirements with IDs (G1, A3, T6, ...) that code comments refer to |

## Hardware

| File | Contents |
|---|---|
| [access-points.md](access-points.md) | The boards the AP runs on (C6, C5, S3 + C6), with limits, pros and cons, and the S3 + C6 wiring |
| [ap-radio-link.md](ap-radio-link.md) | The AP on an ESP32-S3 with an ESP32-C6 as its 802.15.4 radio over UART: wiring and link protocol |
| [ap-netradio.md](ap-netradio.md) | The AP on an ESP32-S3 with a wireless ESP32-C6 radio on the same WiFi network (experimental) |
| [tag-firmware.md](tag-firmware.md) | The tag firmware's modules, tag identity, layouts, fonts and memory on the 8051 |
| [tag-ota.md](tag-ota.md) | Firmware updates over the radio for the tags: bootloader, SPI flash slots, rollback |
| [st-gm16xx2.md](st-gm16xx2.md) | The 1.54" ST-GM16XX2: panel detection, init and refresh |
| [ble-tags.md](ble-tags.md) | Bluetooth tags with their maker's firmware: how the AP will send to them, what is built so far (in progress) |
| [solum-remote.md](solum-remote.md) | The SoluM ESL remote SLT-RC004, sniffed on the AP, and why openESL does not use it yet |
| [power-budget.md](power-budget.md) | Battery life: the estimate, the model and how to measure a tag |
| [sources.md](sources.md) | Every hardware fact the code uses, and where it came from |

## Process

| File | Contents |
|---|---|
| [release.md](release.md) | How a release is made: the pull requests, the hardware checklist, rollout to the tags and what to do when a release is bad |

## User guides

Two full guides live here because the web page and the templates link to them. The wiki has the short version of each and links here for the details.

| File | Short version in the wiki |
|---|---|
| [home-assistant.md](home-assistant.md) | [Home Assistant](https://github.com/mathiasbk/openESL/wiki/Home-Assistant) |
| [data-sources.md](data-sources.md) | [Weather](https://github.com/mathiasbk/openESL/wiki/Weather) |

## History

[history/](history/) holds the plans the project started from: the [work plan](history/work-plan.md), the [risks](history/risks.md) and the [original AP plan](history/ap-firmware-plan.md). They are kept for the record and are not kept up to date.

## The main choices (short version)

1. **The AP draws the screen, the tag shows it.** The client still sends only the text fields and a layout name. The AP draws the whole screen into a 1-bit image (4736 bytes on the 2.9", 5000 on the 1.54", coded to a few hundred bytes when that is shorter) and sends it in blocks when the tag polls; the tag stores it in its SPI flash and streams it to the display controller. Layout files stay on the AP. Up to 0.4.7 the tag drew the text itself from one radio packet; that kept the radio short, but every layout had to fit the tag's flash and RAM ([architecture.md](architecture.md)).
2. **The tag asks, the AP answers.** The tag wakes every 60 seconds, sends "anything new?", listens for a few milliseconds and goes back to sleep. No synchronisation or fixed time slots.
3. **One ESP32-C6 is the whole AP.** The C6 has both WiFi and an 802.15.4 radio, so no second chip is needed. Other boards, such as an ESP32-S3 with a C6 as its radio, are in [access-points.md](access-points.md).
4. **The AP is only an API.** It has a small HTTP API (`PUT /api/tags/{mac}`, `GET /api/tags`). Clients push content to it. The client must be able to reach the AP: same network, or a VPN.
5. **OTA for the tags came early.** Tags flashed once with the bootloader (pogo pins) take new firmware over the radio through the AP (`PUT /api/tag-firmware`, `esl-client tag-ota`, [tag-ota.md](tag-ota.md)); it is on without radio security (T16), by decision. The AP itself is updated over the API (`POST /api/ota`, `esl-client ota`).

