# 802.15.4 radio coprocessor (ESP32-C6)

Firmware for an ESP32-C6 that is only the 802.15.4 radio of an AP running on another chip
(board `s3-n16r8-c6`: an ESP32-S3 with PSRAM). It forwards every frame it receives to the
host over UART and sends the replies the host gives it. See
[docs/ap-radio-link.md](../../docs/ap-radio-link.md) for the wiring and the protocol.

    cd ap/radio && idf.py build          # or: cd ap && python build.py s3-n16r8-c6

`ap/build.py` builds it with every board that names it (`"radio": "uart", "radio_fw": "radio"` in board.json), into
`out/openesl-ap-radio-c6.bin` (written at 0) and `-ota.bin`.
