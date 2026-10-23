# Supported labels

| Maker | Label | Product | Screen | Colours | Chip | Battery |
|---|---|---|---|---|---|---|
| SoluM | 2.9" black/white | ST-GM29XX2 | 296 × 128 | black/white | ZBS243 | two |
| SoluM | 1.54" black/white | ST-GM16XX2 | 200 × 200 | black/white | ZBS243 | one CR2450 |

The current list is always in the [README](https://github.com/mathiasbk/openESL/blob/master/README.md#supported-tags), made from [shared/models.json](https://github.com/mathiasbk/openESL/blob/master/shared/models.json).

<table>
  <tr>
    <td align="center"><img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/pcb-154.jpg" alt="The 1.54-inch circuit board" width="240"><br><sub>1.54" board: ZBS243 and display connector</sub></td>
    <td align="center"><img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/tag-154-back.jpg" alt="The back of a 1.54-inch label" width="240"><br><sub>1.54" back: test pads behind the CR2450</sub></td>
    <td align="center"><img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/tag-29-back.jpg" alt="The back of a 2.9-inch label" width="140"><br><sub>2.9" back: two batteries, test pads</sub></td>
  </tr>
</table>

## Known limits

- The 1.54" cannot measure its battery voltage yet, so it reports 0 (unknown).
- Hardware notes for the 1.54": [docs/st-gm16xx2.md](https://github.com/mathiasbk/openESL/blob/master/docs/st-gm16xx2.md).

## Have a label that is not on the list?

OpenESL is meant to grow to more models and other makers than SoluM, also labels with other chips and radios. [Suggest it in an issue](https://github.com/mathiasbk/openESL/issues/new?template=hardware.yml) with photos of the board and what is written on the chip and display. See [Adding hardware](Adding-hardware).
