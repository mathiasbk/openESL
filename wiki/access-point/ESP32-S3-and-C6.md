# ESP32-S3 + ESP32-C6

**Status:** experimental (built, not tried on hardware). **Labels:** 1000.

Two boards: an ESP32-S3 with 8 MB PSRAM runs the AP (WiFi, API, dashboard, drawing), and an ESP32-C6 next to it is nothing but the label radio. WiFi never takes the radio from the labels, and each chip can have its own antenna.

| Chip | Release file |
|---|---|
| ESP32-S3 N16R8 (the AP) | `openesl-ap-esp32s3-n16r8-<version>.bin` |
| ESP32-C6 (the radio) | `openesl-ap-radio-esp32c6-<version>.bin` |
| or ESP32-C6 with 0.96" screen (the radio) | `openesl-ap-radio-esp32c6-lcd-<version>.bin`; the screen shows the AP's status like the [C6 with screen](ESP32-C6-with-screen) does |

## Wiring

![Wiring between the ESP32-S3 and the ESP32-C6](https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/ap-s3-c6-wiring.svg)

| ESP32-S3 | ESP32-C6 |
|---|---|
| GPIO17 (TX) | GPIO17 (RX) |
| GPIO18 (RX) | GPIO16 (TX) |
| GND | GND |
| 5V | 5V, only when the C6 has no USB cable of its own |

The C6 with screen is wired the same way.

## Flashing

1. **The C6**, with esptool for now (the web page does not offer it yet):
   `esptool --chip esp32c6 write_flash 0 openesl-ap-radio-esp32c6-<version>.bin`
   Its log says `OpenESL 802.15.4 radio`.
2. **The S3**, from the [web page](https://mathiasbk.github.io/openESL/#ap), the ESP32-S3 + ESP32-C6 board. Its log says `radio 1.0 up` when it reaches the C6, or `no answer from the radio` every two seconds when it does not: check that TX and RX are crossed and that GND is connected.

The S3 is updated over the air like any AP. The C6 radio is not updated by the S3 yet, so it is flashed with esptool when it changes.

The link between the chips: [docs/ap-radio-link.md](https://github.com/mathiasbk/openESL/blob/master/docs/ap-radio-link.md).
