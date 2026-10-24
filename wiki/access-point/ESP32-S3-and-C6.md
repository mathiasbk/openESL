# ESP32-S3 + ESP32-C6

**Status:** experimental. The S3 has run on its own (WiFi, dashboard, designer, with simulated labels); not yet tried with the C6 radio. **Labels:** 1000.

Two boards: an ESP32-S3 with 8 MB PSRAM runs the AP (WiFi, API, dashboard, drawing), and an ESP32-C6 next to it is nothing but the label radio. WiFi never takes the radio from the labels, and each chip can have its own antenna. The S3 cannot talk to the labels without the C6: its radio does WiFi and Bluetooth, not 802.15.4.

With room to spare, the S3 also serves the [label designer](Label-designer#getting-it-onto-the-ap) next to the [dashboard](Dashboard), working on the layouts stored on the AP. Its layouts may also hold more: twice the lines of text, boxes, lines and icons of a C6 ([Layout elements and limits](Layout-elements-and-limits)).

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

On the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab, pick **S3 + C6 on cable** and the C6 radio you have (plain or with screen). The page flashes:

1. **The C6** radio. Its log says `OpenESL 802.15.4 radio`.
2. **The S3**, plugged in on its port marked **USB**. That port also gives the S3 its WiFi in the next step: the S3's WiFi setup and serial API answer only there, not on the COM port.

Then it shows the wiring. The S3's log says `radio 1.0 up` when it reaches the C6, or `no answer from the radio` every two seconds when it does not: check that TX and RX are crossed and that GND is connected.

With esptool instead: `esptool --chip esp32c6 write_flash 0 openesl-ap-radio-esp32c6-<version>.bin`.

The S3 is updated over the air like any AP. The C6 radio is not updated by the S3 yet, so it is flashed again from the web page (flash the C6, then skip the S3) when it changes.

The link between the chips: [docs/ap-radio-link.md](https://github.com/mathiasbk/openESL/blob/master/docs/ap-radio-link.md).

Want the C6 somewhere else, with no wire? That is a separate setup with real limits: [ESP32-S3 with a wireless C6 radio](ESP32-S3-with-wireless-C6).
