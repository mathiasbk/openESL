# Updating the access point

The AP is updated over the air. It keeps its labels, layouts and settings.

**Take a backup first:** `esl_client.py backup ap.json` ([Backup and restore](Backup-and-restore)).

## From the dashboard

When the web page has a newer release than the AP runs, an arrow next to the AP's version on the [Dashboard](Dashboard) sends the right image for your board. The dashboard warns if the image is the same or older, or made for another board.

## From the terminal

```sh
python3 esl_client.py ota openesl-ap-<board>-<version>-ota.bin --wait
```

Use the `-ota.bin` file from the [release](https://github.com/mathiasbk/openESL/releases), not the single `.bin`.

## The C6 radio of an ESP32-S3 + C6

An [ESP32-S3 with a C6 on a cable](ESP32-S3-and-C6#updating) has a second chip to update: the C6 radio. Since 0.8.2 the S3 passes the radio's firmware on over the cable, from the dashboard (the **Radio** line under Firmware) or with `esl_client.py ota openesl-ap-radio-esp32c6-<version>-ota.bin --wait`. Update the S3 first, then the radio. A C6 flashed before 0.8.2 has to be flashed once more over USB first.

## If something goes wrong

If the new firmware does not get onto WiFi or answer the API within three minutes, or crashes at start, the AP goes back to the old version by itself.

To go back on purpose, send the older release's `-ota.bin` the same way. If the newer release changed how the AP stores its data, restore from the backup afterwards.

## Over USB instead

The [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab also keeps everything on an AP that runs OpenESL ([Flashing the AP](Flashing-the-access-point#flashing-again-without-losing-anything)). Some releases need this once, for example to get room for more label models.
