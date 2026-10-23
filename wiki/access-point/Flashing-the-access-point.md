# Flashing the access point

## From the web page (recommended)

1. Open the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab in Chrome or Edge on a computer.
2. Pick your board from the list ([Choosing a board](Access-point-boards)).
3. Plug the board in by USB and press the flash button. If the board is not found, hold **BOOT** while you plug it in.
4. When it is done, close the flashing window and press **Connect AP** to give it WiFi and an API token ([WiFi and API token](WiFi-and-API-token)).

The page always flashes the latest release.

## Flashing again without losing anything

The AP stores its labels, layouts, WiFi and token in flash (NVS). How you flash decides whether they survive:

| Way | Labels, layouts, WiFi, token |
|---|---|
| Flash AP tab **without** "Erase device" | **kept** |
| Over the air (Dashboard, or `esl-client ota ...-ota.bin`) | **kept** |
| `idf.py flash` | **kept** |
| A release's single `openesl-ap-...-v<version>.bin` (without `-ota`) written at 0x0 | **erased** |
| Flash AP tab **with** "Erase device" | **erased** |

So the single `.bin` file is only for a new board or one you erase anyway. Take a backup before anything that erases ([Backup and restore](Backup-and-restore)).

## ESP32-S3 + ESP32-C6

The S3 is flashed from the web page like the others. The C6 that serves as its radio is flashed separately with esptool for now. See [ESP32-S3 + C6](ESP32-S3-and-C6).

## Building it yourself

ESP-IDF v5.5.5. Each board has its own settings in [ap/boards](https://github.com/mathiasbk/openESL/blob/master/ap/boards/README.md):

```bash
cd ap
python build.py --list             # the boards
python build.py c6-supermini-lcd   # build one board
idf.py -B build-c6-supermini-lcd -p /dev/ttyACM0 flash monitor
```

More in [Building from source](Building-from-source).
