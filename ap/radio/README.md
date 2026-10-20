# 802.15.4 radio coprocessor (ESP32-C6)

Firmware for an ESP32-C6 that is only the 802.15.4 radio of an AP running on another chip
(board `s3-n16r8-c6`: an ESP32-S3 with PSRAM). It forwards every frame it receives to the
host over UART and sends the replies the host gives it. See
[docs/ap-radio-link.md](../../docs/ap-radio-link.md) for the wiring and the protocol.

There is one image per radio board, in `boards/<id>/`:

| Radio board | For | Release image |
|---|---|---|
| `c6` | any ESP32-C6 (Super Mini) | `openesl-ap-radio-esp32c6-<version>.bin` |
| `c6-lcd` | an ESP32-C6 with a 0.96" ST7789 screen; shows the status the host sends | `openesl-ap-radio-esp32c6-lcd-<version>.bin` |

`radio.json` names the image, `sdkconfig.defaults` adds to `ap/radio/sdkconfig.defaults` (the
screen and its pins for `c6-lcd`, Kconfig in `ap/components/esl_lcd`).

    cd ap/radio && idf.py -B build-c6-lcd -D RADIO_BOARD=c6-lcd build
    cd ap && python build.py s3-n16r8-c6     # the S3 and every radio board it names

`ap/build.py` builds the radio boards a board names in `radio_fw` (`"radio": "uart"` in board.json),
into `out/openesl-ap-radio-<id>.bin` (written at 0). There is no `-ota.bin`: nothing updates
the radio over the air yet, and the AP refuses its app image (`main/ota.c` checks the project name).
