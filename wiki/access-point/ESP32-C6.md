# ESP32-C6

**Status:** supported, on hardware since 2026-10-14. **Labels:** about 700. **Release file:** `openesl-ap-esp32c6-<version>.bin`

The simple choice. One small board (the "ESP32-C6 Super Mini" with 4 MB flash) does both WiFi and the 802.15.4 radio to the labels, with the antenna on the board. It costs about 5 $.

## Good to know

- **Labels:** the C6 has 512 KB of RAM and no PSRAM, which limits it to about 700 labels next to WiFi (an estimate, not yet measured on hardware). When the table is full, the dashboard says so.
- **One radio, two jobs:** WiFi and the label radio take turns. An API reply can wait a little, and a label's poll can now and then be missed while WiFi is busy. The label just tries again.
- **Flashing:** from the [web page](https://mathiasbk.github.io/openESL/#ap), the plain ESP32-C6 board. See [Flashing the AP](Flashing-the-access-point).
- **Updates** go over the air from the dashboard ([Updating the AP](Updating-the-access-point)).

Next: [Flashing the AP](Flashing-the-access-point).
