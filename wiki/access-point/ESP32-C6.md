# ESP32-C6

**Status:** supported, on hardware since 2026-10-14. **Labels:** about 500. **Release file:** `openesl-ap-esp32c6-<version>.bin`

The simple choice. One small board (the "ESP32-C6 Super Mini" with 4 MB flash) does both WiFi and the 802.15.4 radio to the labels, with the antenna on the board. It costs about 5 $.

<table>
  <tr>
    <td align="center"><img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/ap-c6-supermini.jpg" alt="An ESP32-C6 Super Mini board on a USB-C cable" width="220"><br><sub>The ESP32-C6 Super Mini, powered over USB-C</sub></td>
    <td align="center"><img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/ap-c6-case.jpg" alt="The same board in a small black 3D-printed case" width="260"><br><sub>In a small 3D-printed case</sub></td>
  </tr>
</table>

## Good to know

- **Labels:** the C6 has 512 KB of RAM and no PSRAM, which limits it to about 500 labels next to WiFi (an estimate, not yet measured on hardware). When the table is full, the dashboard says so.
- **One radio, two jobs:** WiFi and the label radio take turns. An API reply can wait a little, and a label's poll can now and then be missed while WiFi is busy. The label just tries again.
- **No weather by itself:** the C6 has no memory to spare for fetching the [weather](Weather); send it from Home Assistant instead.
- **Flashing:** from the [web page](https://mathiasbk.github.io/openESL/#ap), the plain ESP32-C6 board. See [Flashing the AP](Flashing-the-access-point).
- **Updates** go over the air from the dashboard ([Updating the AP](Updating-the-access-point)).

Next: [Flashing the AP](Flashing-the-access-point).
