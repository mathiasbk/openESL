# AP boards

One folder per board the AP firmware is built for. `CMakeLists.txt` picks one with
`-DBOARD=<folder>` (default `c6-supermini`), and `build.py` builds one or all of them;
CI and releases build every board here, so a new board needs no workflow change.

| Board | Chip | Flash / PSRAM | Status | Release file |
|---|---|---|---|---|
| `c6-supermini` | ESP32-C6 | 4 MB / none | supported | `openesl-ap-esp32c6-<version>.bin` |
| `c6-supermini-lcd` | ESP32-C6 | 4 MB / none, 0.96" screen | supported | `openesl-ap-esp32c6-lcd-<version>.bin` |
| `c5-wroom-1u-n16r8` | ESP32-C5 | 16 MB / 8 MB | experimental: built, not tried on a board | `openesl-ap-esp32c5-n16r8-<version>.bin` |
| `s3-n16r8-c6` | ESP32-S3 + ESP32-C6 as radio on UART | 16 MB / 8 MB | experimental: the S3 has run alone with the stub radio (WiFi, dashboard, designer, OTA), not yet with the C6 | `openesl-ap-esp32s3-n16r8-<version>.bin` and `openesl-ap-radio-esp32c6-<version>.bin` (or `-esp32c6-lcd-` for a C6 with screen) ([docs/ap-radio-link.md](../../docs/ap-radio-link.md)) |
| `s3-n16r8-netradio` | ESP32-S3 + ESP32-C6 as radio over WiFi | 16 MB / 8 MB | experimental: tried with one C6 and two tags; not secure yet | `openesl-ap-esp32s3-n16r8-netradio-<version>.bin` and `openesl-ap-netradio-esp32c6-<version>.bin` ([docs/ap-netradio.md](../../docs/ap-netradio.md)) |

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
    `CONFIG_ESL_RADIO_DRIVER_UART=y`. `net`: a radio over WiFi (`main/radio_net.c`, the radio in
    `ap/netradio/`, [docs/ap-netradio.md](../../docs/ap-netradio.md)); `CONFIG_ESL_RADIO_DRIVER_NET=y`.
  - `radio_fw` (`uart` and `net`): the radio boards in `ap/radio/boards/` (or `ap/netradio/boards/`) the radio can be
    (a name or a list: `["c6", "c6-lcd"]`). `build.py` builds each one too, once, and the
    release has it as `<release in its radio.json>-<version>.bin`.
- `sdkconfig.defaults`: read after `ap/sdkconfig.defaults` (settings for every board). Sets
  `CONFIG_IDF_TARGET`, the flash size and `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`, and
  anything else that differs.
- `partitions.csv` (unless the base board's is used): must have `nvs`, `otadata`, `ota_0`,
  `ota_1` and `tagfw` (64 KB per tag model; with fewer slots than models, the models that do
  not fit get no tag firmware updates, see `main/tagfw.c`).

## Adding a board

1. Copy the folder closest to it, rename it, and change `board.json` and `sdkconfig.defaults`.
2. `python build.py <board>` (ESP-IDF v5.5.5, with the chip's compiler: `./install.sh esp32s3`
   for the S3, which is Xtensa where the C6 and C5 are RISC-V). For a new chip the component manager writes
   `dependencies.lock.<chip>` next to `dependencies.lock`; commit it.
3. Flash `out/<artifact>.bin` at 0 with esptool, try it with a tag, and set `status` to
   `supported` when it works. A host without its tag radio yet can be tried with
   `CONFIG_ESL_RADIO_STUB=y` added to its `sdkconfig.defaults` in a local copy: simulated tags
   poll it, so WiFi, the API and the web page can be checked on the board first.

The web flasher on the GitHub page offers every board here (`web/build.py` reads `board.json`).
