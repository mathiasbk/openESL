# SoluM 1.54" black/white (ST-GM16XX2)

**Status:** supported. **Model in OpenESL:** `1.54` (id `solum-m2-154-bw`). **Battery:** one CR2450.

<p align="center">
  <img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/tag-154-front.jpg" alt="A 1.54-inch label running OpenESL, showing its start screen" width="260">
  &nbsp;
  <img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/tag-154-3d-printer.jpg" alt="A 1.54-inch label showing a 3D printer at 63 percent, 12 minutes left" width="260">
</p>

A small, square label. It fits a short name and a price, or a status such as a 3D printer's progress (right).

## Specifications

| | |
|---|---|
| Maker | SoluM |
| Product (on the back) | ST-GM16XX2 |
| Screen | 1.54", 200 × 200 pixels, black/white |
| Display | marked HINK-E0154A16-A0 on its cable; SSD-type controller (not confirmed) |
| Chip | ZBS243 (8051 core with an 802.15.4 radio) |
| Memory | 64 KB flash in the chip, plus 1 MB SPI flash (Winbond W25Q80) for the image and firmware updates |
| Radio | 802.15.4 on 2.4 GHz, the same as every OpenESL label |
| Battery | one CR2450 (3 V) |
| Battery reading | no, it reports 0 (unknown); see [Known issues](#known-issues) |
| Battery life | about 9 years calculated with a poll every 60 s and one new screen a day ([power budget](https://github.com/mathiasbk/openESL/blob/master/docs/strombudsjett.md)) |
| MAC | 8 bytes, the second last is `34`, e.g. `000002079575341D` |
| Updates over the air | yes ([Updating labels](Updating-labels)) |

## How to recognise it

<table>
  <tr>
    <td align="center"><img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/tag-154-back.jpg" alt="The back of a 1.54-inch label" width="240"><br><sub>The back: test pads behind the CR2450</sub></td>
    <td align="center"><img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/pcb-154.jpg" alt="The 1.54-inch circuit board" width="240"><br><sub>The board: ZBS243 and display connector</sub></td>
  </tr>
</table>

- A small, almost square white case with a barcode above the screen.
- **ST-GM16XX2** on the sticker on the back. The barcode number is not the MAC; the flasher works the MAC out for you.
- One CR2450 behind the cover on the back.

The older SoluM 1.54" **ST-GR16000** looks alike but has a 152 × 152 screen and is not supported. If the back says something else, see [Supported labels](Supported-labels#not-supported-yet).

## Flashing

Open the battery cover and take the cell out. The test pads are behind it; the [jig](Flashing-jig) holds the label on them with pogo pins. Then follow [Flashing labels](Flashing-labels) and pick the 1.54" in the first step. The page backs up the original firmware before it writes OpenESL ([Label backups](Label-backups)).

Put the cell back and the label shows its start screen with its MAC and firmware version.

## What you can show on it

- Every [template](Label-templates) has a 1.54" version. With 200 × 200 pixels there is room for less text than on the [2.9"](SoluM-2.9-ST-GM29XX2): one big number and two or three short lines.
- Your own layouts from the [Label designer](Label-designer). See [Layout elements and limits](Layout-elements-and-limits).
- Pictures from the dashboard ("A picture"), turned into black and white.

## Known issues

- **No battery reading.** The display controller on this label does not answer the battery check the 2.9" uses, so the label reports 0 and the dashboard shows the battery as unknown. A label that reports 0 never gets the `low_battery` warning ([Health and webhook](Health-and-webhook)), so change the cell on a schedule.

## Technical notes

- What we learned getting the display to draw: [docs/st-gm16xx2.md](https://github.com/mathiasbk/openESL/blob/master/docs/st-gm16xx2.md).
- Display settings: [tag/src/panels/ssd16xx-154.h](https://github.com/mathiasbk/openESL/blob/master/tag/src/panels/ssd16xx-154.h).
- Model entry: [shared/models.json](https://github.com/mathiasbk/openESL/blob/master/shared/models.json).
- The label firmware: [docs/tag-firmware.md](https://github.com/mathiasbk/openESL/blob/master/docs/tag-firmware.md), updates over the air: [docs/tag-ota.md](https://github.com/mathiasbk/openESL/blob/master/docs/tag-ota.md).

Other labels: [Supported labels](Supported-labels).
