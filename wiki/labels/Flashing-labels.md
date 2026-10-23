# Flashing labels

Each label is flashed once over its test pads, in the [jig](Flashing-jig). After that it takes new firmware over the air.

## What you need

- The jig with a D1 mini running ZBS_Flasher, plugged in by USB.
- Chrome or Edge on a computer.
- The label with its battery taken out, so the test pads are free.

## From the web page

1. Open the [Flash tag](https://mathiasbk.github.io/openESL/#tag) tab and pick the label model from the photos.
2. Put the label in the jig and press **Flash**.
3. The page reads the label's MAC, **backs up the whole original firmware** in the browser ([Label backups](Label-backups)), writes the MAC to the info page if needed, then writes OpenESL and reads it back to check it.
4. Take the label out, put the battery back, and it shows its start screen with its MAC and firmware version.

With **Keep going** on, the page waits for the next label after each one, so you can do a stack in a row.

## A label with no MAC

If an earlier build wiped the label's MAC, the page asks for the barcode on its sticker or its MAC, and flashes it again with it. To get a label's MAC from its barcode number: `esl_client.py mac 020663461013`.

## From the terminal

[flash-tui](https://github.com/mathiasbk/openESL/blob/master/tools/flash-tui/README.md) does the same steps in a terminal, with auto-detect: it flashes as soon as a label answers in the jig.

```sh
python tools/flash-tui/flash_tui.py
```

## After flashing

The label looks for the AP and polls it about once a minute. It turns up on the [Dashboard](Dashboard) at its first poll. If it finds no AP it shows "No AP found" and tries again less often to save battery.

Labels flashed with very old firmware (before 0.3.0) have no bootloader for updates over the air and need one more pass in the jig.
