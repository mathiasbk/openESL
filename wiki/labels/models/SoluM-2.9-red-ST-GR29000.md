# SoluM 2.9" black/white/red (ST-GR29000)

> **Experimental.** Nobody has run OpenESL on this label yet. It may draw wrong, mirrored or not at all. Flash one label first and check it before you flash more; the flasher backs up the original firmware so you can go back.

**Status:** experimental, not yet tried on a label. **Model in OpenESL:** `2.9-bwr` (id `solum-29-bwr`), shown as "2.9" red (experimental)" on the web page.

The 2.9" with a third colour: the same size and chip as the black/white [2.9"](SoluM-2.9-ST-GM29XX2), with a screen that can also show red. Labels with red are common second hand.

OpenESL builds firmware for it and the AP draws red for it, but nobody has run it on a real label yet. The display settings are the black/white 2.9"'s, on the guess that the screen has a controller of the same kind. **Flash one label first and check it** before you flash more. If you have one, please report how it went ([issue #255](https://github.com/mathiasbk/openESL/issues/255)).

## Specifications

| | |
|---|---|
| Maker | SoluM |
| Product (on the back) | ST-GR29000 |
| Screen | 2.9", 296 × 128 pixels, black/white/red |
| Display controller | not known; assumed to be in the SSD1680 family like the 2.9" black/white |
| Chip | ZBS243 (8051 core with an 802.15.4 radio) |
| Radio | 802.15.4 on 2.4 GHz, the same as every OpenESL label |
| MAC | 8 bytes; the flasher puts `3C` second last (OpenESL's code for this model) |
| A new screen | slower than black/white: a refresh with red takes about 15-20 s. One new screen a day still gives about 14 years on two CR2450, against 16.5 on the black/white 2.9" (calculated, [power budget](https://github.com/mathiasbk/openESL/blob/master/docs/strombudsjett.md)); many a day cost about six times as much as on black/white |
| Updates over the air | yes, the same as the other labels (not tried yet) |

## How to recognise it

- Looks like the black/white 2.9", with **ST-GR29000** on the sticker on the back.
- The same product code can come with different displays inside. Before flashing, note what is printed on the display's ribbon cable, and put it in the issue.

## Flashing

As the [2.9"](SoluM-2.9-ST-GM29XX2#flashing): the [jig](Flashing-jig) and [Flashing labels](Flashing-labels), and pick the 2.9" black/white/red in the first step. The page backs up the original firmware first ([Label backups](Label-backups)), so a label that does not draw right can be put back.

What to check on the first label:

- The start screen is the right way round, not mirrored.
- A layout with red (the example layouts have a red band at the top) shows red, not black or white.
- How long a new screen takes, and that the label reports it as shown.
- The battery reading on the [Dashboard](Dashboard).

## What you can show on it

- The example layouts for `2.9-bwr` are the 2.9" ones with the bands in red.
- Your own layouts from the [Label designer](Label-designer), 296 × 128 pixels, with the colours `red` and `red_invert` (white text in a red box) as well. See [Layout elements and limits](Layout-elements-and-limits).
- Every [template](Label-templates): the retail ones with a red band, like the example layouts, the others as on the black/white 2.9".

## Technical notes

- Display settings: [tag/src/panels/ssd16xx-29-bwr.h](https://github.com/mathiasbk/openESL/blob/master/tag/src/panels/ssd16xx-29-bwr.h), with what to check on the board.
- Model entry: [shared/models.json](https://github.com/mathiasbk/openESL/blob/master/shared/models.json).
- The image with red is twice the size of a black/white one (9472 bytes), sent and stored the same way.

Other labels: [Supported labels](Supported-labels).
