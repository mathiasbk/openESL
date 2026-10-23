# Adding hardware

OpenESL is meant to grow to more label models, other makers than SoluM, and other access point boards.

## A new label model

**Just have the label?** [Suggest it in an issue](https://github.com/mathiasbk/openESL/issues/new?template=hardware.yml) with:

- photos of both sides of the board,
- what is written on the chip and the display,
- the model number on the back, and
- if you can, a backup of its flash.

**Want to add it yourself?** A label model is described in one place, [shared/models.json](https://github.com/mathiasbk/openESL/blob/master/shared/models.json): its maker, chip, radio, screen, colours and the image format the AP draws for it. Adding a model is an entry there and a few files next to it; see [shared/README.md](https://github.com/mathiasbk/openESL/blob/master/shared/README.md). A label on the ZBS243 then needs its display driver settings in the label firmware ([tag/README.md](https://github.com/mathiasbk/openESL/blob/master/tag/README.md)). A label with another chip needs its own firmware.

## A new access point board

Each board is a folder in [ap/boards](https://github.com/mathiasbk/openESL/blob/master/ap/boards/README.md):

1. Copy the folder closest to it, rename it, and change `board.json` and `sdkconfig.defaults`.
2. `python build.py <board>` with ESP-IDF v5.5.5.
3. Flash it, try it with a label, and set `status` to `supported` when it works.

CI, releases and the web page's flasher pick up the new board by themselves.

## A new flasher

Anything that runs ZBS_Flasher and has a serial port should work with the web page and flash-tui. If you build a jig for another board or label, photos and files are welcome in [Jig/](https://github.com/mathiasbk/openESL/blob/master/Jig/Hardware/README.md).
