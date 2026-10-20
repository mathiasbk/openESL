# AP boards

One folder per board the AP firmware is built for. `CMakeLists.txt` picks one with
`-DBOARD=<folder>` (default `c6-supermini`), and `build.py` builds one or all of them;
CI and releases build every board here, so a new board needs no workflow change.

| Board | Chip | Flash / PSRAM | Status | Release file |
|---|---|---|---|---|
| `c6-supermini` | ESP32-C6 | 4 MB / none | supported | `openesl-ap-esp32c6-<version>.bin` |
| `c6-supermini-lcd` | ESP32-C6 | 4 MB / none, 0.96" screen | supported | `openesl-ap-esp32c6-lcd-<version>.bin` |
| `c5-wroom-1u-n16r8` | ESP32-C5 | 16 MB / 8 MB | experimental: built, not tried on a board | `openesl-ap-esp32c5-n16r8-<version>.bin` |
| `s3-n16r8-c6` | ESP32-S3 + ESP32-C6 as radio on UART | 16 MB / 8 MB | experimental: built, not tried on a board | `openesl-ap-esp32s3-n16r8-<version>.bin` and `openesl-ap-radio-esp32c6-<version>.bin` ([docs/ap-radio-link.md](../../docs/ap-radio-link.md)) |

Each folder has:

- `board.json`
  - `id`: the folder name
  - `base`: another board whose `sdkconfig.defaults` is read first (optional; the LCD board is
    the Super Mini plus a screen)
  - `target`: the ESP-IDF chip (`esp32c6`, `esp32c5`, ...); the build takes the chip from here
  - `chip_family`: the chip as esp-web-tools names it (`ESP32-C6`)
  - `flash_mb`, `psram_mb`, `antenna`, `label`: what the board is, for people
  - `artifact`: the file names `build.py` writes in `out/` (`<artifact>.bin`, `<artifact>-ota.bin`)
  - `release`: the file names in a GitHub release (`<release>-<version>.bin`, `...-ota.bin`)
  - `status`: `supported` (tried on hardware) or `experimental`
  - `radio`: where the 802.15.4 radio is. `native`: on the AP chip itself (C6, C5;
    `main/radio_ieee802154.c`). `uart`: a radio coprocessor on UART (`main/radio_uart.c`,
    [docs/ap-radio-link.md](../../docs/ap-radio-link.md)); its `sdkconfig.defaults` then sets
    `CONFIG_ESL_RADIO_UART=y`.
  - `radio_fw` (`uart` only): the folder of the coprocessor firmware (`radio`: `ap/radio/`, an
    ESP32-C6). `build.py` builds it too, once, and the release has it as
    `<release in radio.json>-<version>.bin`.
- `sdkconfig.defaults`: read after `ap/sdkconfig.defaults` (settings for every board). Sets
  `CONFIG_IDF_TARGET`, the flash size and `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`, and
  anything else that differs.
- `partitions.csv` (unless the base board's is used): must have `nvs`, `otadata`, `ota_0`,
  `ota_1` and `tagfw` (64 KB per tag model; with fewer slots than models, the models that do
  not fit get no tag firmware updates, see `main/tagfw.c`).

## Adding a board

1. Copy the folder closest to it, rename it, and change `board.json` and `sdkconfig.defaults`.
2. `python build.py <board>` (ESP-IDF v5.5.5). For a new chip the component manager writes
   `dependencies.lock.<chip>` next to `dependencies.lock`; commit it.
3. Flash `out/<artifact>.bin` at 0 with esptool, try it with a tag, and set `status` to
   `supported` when it works.

The web flasher on the GitHub page offers only the C6 builds so far.
