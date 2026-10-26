# Access points

The AP firmware is built for several boards. This page compares them; how to flash an AP is in the [README](../README.md#getting-started). The ESP32-C6 alone is the simple choice and the one tried most; the others give more room for tags. Details per board, and how to add one: [ap/boards/README.md](../ap/boards/README.md).

<!-- Photos of each board go here (images/ap-*.jpg) when they are taken. -->

| Board | Chips | Tags | Antenna | Price (about) | Status |
|---|---|---|---|---|---|
| **ESP32-C6 Super Mini** | one ESP32-C6 | about 700 | on the board | 5 $ | supported, on hardware since 2026-10-14 |
| **ESP32-C6 with 0.96" screen** | one ESP32-C6 | about 700 | on the board | 10 $ | supported |
| **ESP32-C5 N16R8** (e.g. Waveshare ESP32-C5-WIFI6-KIT) | one ESP32-C5, 8 MB PSRAM | 1000 | on the module or external (u.FL) | 10–15 $ | experimental: built, not tried on a board |
| **ESP32-S3 N16R8 + ESP32-C6** | ESP32-S3 with 8 MB PSRAM as the AP, a C6 as its radio over UART | 1000 | each chip its own; u.FL with a WROOM-1U | 10 $ + a C6 | supported: tried with a C6 Super Mini and four tags, radio updates over the cable included ([checking the link](ap-radio-link.md#checking-the-link-from-the-c6)) |
| **ESP32-S3 N16R8 + ESP32-C6 with 0.96" screen** | the same, with the screen board as the radio; the screen shows the AP's status | 1000 | each chip its own; u.FL with a WROOM-1U | 10 $ + a C6 with screen | experimental: tried with a plain C6, not yet with the screen board |
| **ESP32-S3 N16R8 + wireless ESP32-C6** (`netradio`) | the same two chips, the C6 elsewhere on the WiFi network with no wire to the S3 | not decided | each chip its own | 10 $ + a C6 | experimental, not secure yet (plain TCP): tried on one S3 + C6 with two tags; read the limits in [ap-netradio.md](ap-netradio.md) |

**ESP32-C6.** One small board does WiFi and the tag radio. It is cheap, flashed from the web page, and the board OpenESL is tested on. The C6 has no PSRAM, so its 512 KB of RAM limits it to about 700 tags next to WiFi (not measured on hardware yet, see [ap/README.md](../ap/README.md)), and WiFi and the tag radio take turns on one radio (so API replies can wait a little, and a tag's poll can be missed while WiFi is busy).

**ESP32-C6 with screen.** The same, plus a status screen with the AP's address, WiFi and tag count, which is handy while setting it up. Same limits.

**ESP32-C5.** Still one chip and the same radio code as the C6, with 8 MB PSRAM for 1000 tags, and WiFi on 5 GHz if you want it, which keeps 2.4 GHz free for the tags. The radio is still shared, and the C5 is newer and less common than the C6.

**ESP32-S3 + ESP32-C6.** The S3 runs the AP with 8 MB PSRAM for 1000 tags, and the C6 is nothing but the tag radio, so WiFi never takes it from the tags. Each chip can have its own antenna. It is two boards, four wires ([wiring](ap-radio-link.md#wiring)) and two firmware images; the web page flashes both, one after the other, and since 0.8.2 the S3 updates the C6 over the cable, so only the first flash of the C6 needs its USB port. The S3 cannot reach the tags without the C6 (its radio is WiFi and Bluetooth, not 802.15.4). With flash and RAM to spare, its web page has the label designer next to the dashboard, working on the layout files stored on the AP (the C5 has it too).

**ESP32-S3 + ESP32-C6 with screen.** The same, with a C6 that has the 0.96" screen as the radio, flashed with its own radio image (`openesl-ap-radio-esp32c6-lcd`). The S3 sends its status to the C6, which shows the AP's address, WiFi and tag count like the C6 with screen does on its own. Same wiring; the screen uses other pins.

**ESP32-S3 + wireless ESP32-C6.** A separate setup for when the radio cannot sit next to the S3. Over WiFi the C6 cannot pass frames to the S3 and back in time, so it answers the tags itself from a cache the S3 fills, and the S3 learns afterwards what happened. That has real costs: changes reach the tags up to 10 s later (30 s when the tags keep it busy), the dashboard lags, images and firmware go over WiFi only in pauses and cost tag replies while they do, and every WiFi packet takes the C6's radio from the tags for about 90 ms. Read the limits in [ap-netradio.md](ap-netradio.md) before choosing it; the cable is better whenever it is possible.

![Wiring between the ESP32-S3 and the ESP32-C6: GPIO17 to GPIO17, GPIO18 to GPIO16, GND, and 5V only when the C6 has no USB cable](images/ap-s3-c6-wiring.svg)

How the S3 and the C6 talk, and how to flash the C6: [ap-radio-link.md](ap-radio-link.md).
