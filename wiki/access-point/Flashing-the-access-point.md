# Flashing the access point

## From the web page (recommended)

The [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab is a guide in three steps. It needs Chrome or Edge on a computer.

1. **Choose AP.** Pick the card for your setup ([Choosing a board](Access-point-boards)), and say whether the board is **New** or **Already OpenESL** (keeps its data).
2. **Flash.** Plug in each board when the page asks and press **Connect and flash**. With a C6 radio, the C6 comes first, then the S3 on its port marked **USB**. If a board is not found, hold **BOOT** while you plug it in. The page checks that the chip is the one the step is for.
3. **WiFi and token.** Over the same USB cable, give the AP your WiFi, a name and an API token ([WiFi and API token](WiFi-and-API-token)). **Keep a copy of the token.** With the radio over WiFi, the page then gives the C6 the same network and the S3's address.

The last step shows the AP's address and opens its dashboard. The page always flashes the latest release.

## Flashing again without losing anything

The AP stores its labels, layouts, WiFi and token in flash (NVS). How you flash decides whether they survive:

| Way | Labels, layouts, WiFi, token |
|---|---|
| Flash AP tab, **Already OpenESL** | **kept** |
| Over the air (Dashboard, or `esl-client ota ...-ota.bin`) | **kept** |
| `idf.py flash` | **kept** |
| A release's single `openesl-ap-...-v<version>.bin` (without `-ota`) written at 0x0 | **erased** |
| Flash AP tab, **New** | **erased** |

So the single `.bin` file is only for a new board or one you erase anyway. Take a backup before anything that erases ([Backup and restore](Backup-and-restore)).

With **Already OpenESL**, the page first reads the partition table on the board. If it is not the one the new firmware uses (OpenESL from before 0.3.0, or other firmware), it stops and offers to erase and flash it as new instead.

## ESP32-S3 + ESP32-C6

Both boards are flashed from the web page: the C6 radio first (plain or with screen), then the S3. See [ESP32-S3 + C6](ESP32-S3-and-C6) and [ESP32-S3 with a wireless C6 radio](ESP32-S3-with-wireless-C6).

## Building it yourself

ESP-IDF v5.5.5. Each board has its own settings in [ap/boards](https://github.com/mathiasbk/openESL/blob/master/ap/boards/README.md):

```bash
cd ap
python build.py --list             # the boards
python build.py c6-supermini-lcd   # build one board
idf.py -B build-c6-supermini-lcd -p /dev/ttyACM0 flash monitor
```

More in [Building from source](Building-from-source).
