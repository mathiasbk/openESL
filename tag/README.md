# Tag firmware (ZBS243 / SEM9110)

Own firmware for the SoluM ST-GM29XX2 (2.9", 296×128, black/white) and the ST-GM16XX2 (1.54", 200×200, black/white), one build per model (`src/model.h`). The tag asks the AP for new content, receives name, price and text in one radio packet, and draws the screen itself with built-in fonts.

Background and requirements: [docs/tag-firmware.md](../docs/tag-firmware.md), [docs/protokoll.md](../docs/protokoll.md), [docs/krav.md](../docs/krav.md). All code here is written for this project; the hardware facts it relies on are listed with their sources in [docs/kildelogg.md](../docs/kildelogg.md).

## Status

| Part | Status |
|---|---|
| Packets: POLL (with the model byte, T18, `cfg_ver`, and the layout version, T17), IDLE, CONTENT, LAYOUT, LAYOUT_REQ, CONFIG and the 802.15.4 frame around them (`proto.c`, `shared/proto.h`) | Done, tested on PC |
| Settings from the AP (CONFIG: interval, backoff cap, "No AP found" delay), kept in flash through `hal_store_*` (`app.c`) | Done, tested on PC (fake flash); the flash store itself is tested on the tag by `hwtest`, not yet with CONFIG on the air |
| Fonts: small (17 px), medium (24 px), price fonts large (52 px) and mid (36 px) with digits and ` ,-.:` only | Done, generated from Inter (OFL) |
| Column-by-column drawing (`draw.c`), layout interpreter with built-in layouts "price" and "text" (`layout.c`) for both models | Done, tested on PC for both models, previews in `build/preview/<model>/` |
| Layouts sent from the AP (G7/T17): block download, CRC-32 and structure check, drawing from XRAM (`app.c`, `layout.c`, [protokoll.md](../docs/protokoll.md) part 1 "Layout transfer") | Done, tested on PC for both models (fake radio); tried on the air (PR #7). Kept in flash too, so a restart needs no new download (see T12 below) |
| State machine: POLL, retry, draw only on a new `content_id`, longer interval without AP, "No AP found" screen (`app.c`) | Done, tested on PC with a fake radio |
| SSD16xx display driver over bit-banged SPI (`epd.c`) | Written, tested against fake pins and a model of the controller RAM. Draws correctly on the 1.54" (RAM layout and direction confirmed on the tag, [kildelogg.md](../docs/kildelogg.md)); the commands are not checked against a datasheet, and the 2.9" is not tried yet |
| Clock, GPIO, UART, timer, watchdog, temperature, flash, deep sleep (`hal/zbs243/`), and `hwtest.c` to check them | Done and running on the tag; facts in [kildelogg.md](../docs/kildelogg.md). Battery voltage reads 0 mV (unknown) on the ST-GM16XX2 (issue #4) |
| Radio (`hal/zbs243/radio_zbs243.c`) | Done, sends and receives with the C6 (phases 3 and 5) |
| Barcodes (Code 128, EAN-13) as a layout element (`barcode.c`, [protokoll.md](../docs/protokoll.md) part 1): drawn column by column like the rest, checked by reading the drawn screens back with a decoder (`test/test_label_barcode.py`, zxing-cpp) | Done, tested on PC for both models; not scanned from a real tag yet |
| Last content and the layout file in flash (T12): drawn again at start-up without the AP (`app.c`, `hal_layout_store_*`) | Done, tested on PC (fake flash); not tried on the tag yet. `hwtest` writes and reads back the 768-byte layout page |

## Folders

```
tag/
  Makefile              thin wrapper around tools/build.py
  src/
    main.c              start-up and main loop (ZBS243 only)
    app.c/.h            the state machine, with no hardware code
    proto.c, proto_tag.h  build POLL, LAYOUT_REQ and the frame, parse IDLE/CONTENT/CONFIG/LAYOUT
    model.h             screen size per tag model (MODEL=29 or 154)
    layout.c/.h         layout elements, font fallback, line wrapping, truncation with "…"
    barcode.c/.h        Code 128 and EAN-13 as bars, for the barcode element
    draw.c/.h           computes one screen column (DRAW_H/8 bytes) at a time
    font.c/.h           lookup in the font tables
    epd.c/.h            SSD16xx over bit-banged SPI, driven by the model's panel header
    panels/             one header per panel: RAM direction, update sequence, init commands
    config.h            intervals, channel, PAN ID (the version comes from ../VERSION)
    compat.h            lets the same code compile with SDCC and on the PC
    hal/hal.h           everything the app needs from the hardware
    hal/hal_epd.h       the display pins
    hal/zbs243/         the ZBS243 implementation (HAL, radio, hwtest.c)
  fonts/
    font_*.c            generated font tables (checked in)
    preview_*.png       what the fonts look like
    src/                the Inter source fonts
    OFL.txt             the font licence
  designs/              label designs in JSON (see below)
    examples/           example data per design, drawn by tools/render_host.py
    layouts/            example layout files per model (2.9.json, 1.54.json), protokoll.md part 3
  test/                 host tests, fake hardware, preview, layout_compile (the AP's layout
                        compiler as a host tool, for the previews)
  tools/
    build.py            builds the firmware, runs the tests and the preview (default model)
    render_host.py      tests and previews for every model, including the JSON designs
    gen_font.py         font generator (freetype-py + Pillow)
    label_design.py     checks label designs and turns them into AP fields
```

`shared/proto.h` in the root folder holds the packet definitions used by both the tag and the AP.

## Build and test

Requires Python 3 with Pillow (and freetype-py to regenerate the fonts). Without `make`, run `python tools/build.py <target>` from `tag/`.

| Command | What it does |
|---|---|
| `make test` | Compiles and runs the host tests with a normal C compiler, and the Python design tests |
| `make preview` | Draws example content to `build/preview/*.png` (2.9") |
| `make host` | Runs the host tests for every model and draws the examples, the layouts in `designs/layouts/<model>.json` (`layout-<name>.png`) and the designs to `build/preview/<model>/*.png`, with everything on one sheet in `all.png` (`python tools/render_host.py [29\|154]`) |
| `make fw` | Builds the bootloader and the application with SDCC: `build/fw/openesl.bin` (both, for the flasher) and `build/fw/openesl-ota.bin` (the application with its header, for `esl-client tag-ota`) |
| `make fonts` | Regenerates `fonts/font_*.c` |

**Tool versions (requirement G5):**
- SDCC **4.5.0** (`winget install SDCC.SDCC --version 4.5.0`). `build.py` warns if another version is used. Flags: `-mmcs51 --model-large --stack-auto --std-c99 --opt-code-size`, and the code is linked below `0xF400` (`CODE_SIZE`): 0x0000-0x03FF holds the bootloader (`boot/`, built with `--model-small`, must stay under 1 KB), the application starts at 0x0400, 0xF400-0xF7FF holds the layout file, 0xF800-0xFBFF the record, and the stock MAC is at `0xFC06`.
- Host compiler: `CC`, otherwise gcc, clang, cc, or `python -m ziglang cc`.
- The fonts are generated from Inter 3.019 with freetype-py 2.5.1 (FreeType 2.13.2): FreeType's auto-hinter, light target, thresholded from grey. Inter has no hinting of its own, and plain monochrome rendering gave ragged small text. Size, height and baseline are fixed in `tools/gen_font.py`, so the metrics are the same on every machine; the generated files are checked in. Regenerate with `pip install freetype-py pillow` and `python tools/gen_font.py`.
- Model: `-DMODEL=29` (default) or `-DMODEL=154`. `render_host.py` builds both on the PC. `MODEL=154 python tools/build.py fw` builds the 1.54" to `build/fw154/openesl.bin`; the default is the 2.9" in `build/fw/`.

Flash with [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher) (`write build/fw/openesl.bin`). Back up the original firmware with `read` first. After that, new firmware can go over the radio: `esl-client tag-ota build/fw/openesl-ota.bin` hands it to the AP, which sends it to the tags ([docs/tag-ota.md](../docs/tag-ota.md)). Tags flashed before 0.3.0 have no bootloader and need one more pass with the flasher.

**Size now (SDCC 4.5.0, 2026-10-15, `build.py fw`, with barcodes, fixed texts and OTA):** the bootloader is 994 of 1,024 bytes; the application is 53,437 bytes for the 2.9" and 53,816 for the 1.54" (of 61,440 from 0x0400 to `CODE_SIZE`), 2,710 (2.9") and 2,916 (1.54") bytes of XRAM from 0xE300 and 223 bytes free for the stack in internal RAM. OTA added about 4.5 KB of code and 300 bytes of XRAM. The 1.54" firmware with layout download has run on the tag (phase 5), so the stack and XRAM have been enough so far; the deepest call chain has not been measured.

## Label design (JSON)

Design files are in `designs/`. The format and rules are at the top of `tools/label_design.py`. A design says which layout the label uses, which fields it fills (with placeholders such as `{name}`), and the styles. `render` gives the fields in the same form as the body of `PUT /api/tags/{mac}`, so a client can send them directly.

```bash
python tools/label_design.py check designs/*.json
python tools/label_design.py render designs/pris-standard.json data.json
python -m unittest discover -s test -p "test_label_*.py"
```

The design does not change the firmware. Its layout is one of the built-in layouts "price" and "text", or a layout from a layout file (`designs/layouts/<model>.json`, uploaded to the AP and downloaded by the tags); the field names then come from that file (`render <design> <data> designs/layouts/2.9.json`). The styles `price_highlight` and `frame` map to flag bits 0 and 1 and have the same names as in the API. Characters the AP replaces itself (dashes, curly quotes, "…") are allowed, and the length limit is counted after the replacement, as on the AP.

The example layout files hold `shelf` and `sign`, and four product labels: `vare` (brand in a black band, name, article number/EAN, price), `vare-mva` (brand, name, a small note such as the price inc. VAT, and the highlighted price), `vare-ean` (brand, name, price, and the EAN-13 as a barcode with the digits under it), and `vare-priser` (brand, name, a note, the price ex. VAT and the highlighted price inc. VAT, fields `price` and `price_ex`; the client sends both, so each product can have its own VAT rate, as in `designs/vare-priser.json`). A layout takes at most six fields from the content, but fixed texts such as "inkl. mva" can live in the layout file itself (`"text": "inkl. mva"` instead of `"field"`, as in `vare-ean`), so they cost nothing in the radio packet. A label text that changes with the content still comes from the design, as in `designs/vare-eks-mva.json` (`"note": "inkl. mva {price_inc}"`).

`make host` draws every design with the data in `designs/examples/<id>.json` to `build/preview/<model>/design-<id>-<n>.png`. A design for a file layout is drawn for every model whose layout file has that layout: `render_host.py` compiles `designs/layouts/<model>.json` with the AP's own compiler (built from `ap/components/esl_core/` as `build/host/layout_compile`) into `build/preview/<model>/layouts.bin`, and the preview tool installs that file with the tag's own check (`layout_file_install`), as after a download. An error in a layout file stops the run with the same message the AP would answer with.

## How the drawing works

ZBS243 does not have RAM for a whole image (296 × 128 = 4736 bytes on the 2.9", 5000 on the 1.54"). The display controller has its own image memory, so `epd.c` asks `draw_column(x)` for one column at a time (128 pixels = 16 bytes on the 2.9") and writes it straight in. Both tags are black/white, so only the black plane is sent (`MODEL_HAS_RED` = 0 for both).

- `layout.c` turns a CONTENT packet into a list of text runs (font, colour, position, pointer to the text) and filled rectangles (the frame, the black box behind an inverted field). It does this by going through a list of layout elements (`layout_elem_t`): a text element puts one field in a box, a rectangle element fills a box.
- `draw.c` goes through the runs for each column. Each text run has a cursor that moves forward as x increases, so it does not search the text for every column.
- The fonts are stored column by column, so one font column can be ORed straight into the screen column.
- Highlighting is black/white: rectangles are filled first, then the text runs; a white run clears pixels, so white text on a black rectangle gives an inverted field.

**Text that is too long.** Each text element has a list of up to three fonts. The first font in which every character exists and the text fits on the element's lines, without truncation and without breaking a word, is used. If none fits, the last font is used, wrapped, and the last line ends with "…". Lines break at the last space that fits; a lone unit like "g" at the end goes down together with the number before it.

Layout 1, "price": the name at the top, in the medium font on up to two lines (three on the 1.54"), or in the small font on up to three lines. The price right-aligned in the large font, then the mid font, then the medium font (e.g. "2 for 30", which the price fonts lack); all three sit on the same baseline. If flag bit 0 is set the price is inverted: white digits in a black box, `LAYOUT_INVERT_PAD` pixels around the font cell, right-aligned at the margin. The box must hold the whole font cell, so on the 2.9" a highlighted price uses the mid font; the 1.54" has room for the large one. The subtitle in the small font at the bottom.
Layout 2, "text": four lines in the medium font, or the small font for a line that does not fit. On the 1.54" each line may use two text lines.
Flag bit 1 gives a black frame around the whole screen.

Layouts from the AP (G7/T17) use the same elements, 14 bytes each in the wire format in `shared/proto.h`. The AP announces a new version in its answer to POLL; `app.c` fetches the file block by block in the same wake-up into `layout_file[]` (768 bytes of XRAM), `layout_file_install()` checks the CRC-32, the model and every element, and CONTENT with layout 0x10 + n is then drawn with layout n of the file (`layout_apply_bin()`, decoding one element at a time into a 14-byte buffer, so no second copy). See [protokoll.md](../docs/protokoll.md) part 1, "Layout transfer".

**Models.** `src/model.h` holds the screen size. The 2.9" is 296 × 128 and the 1.54" (ST-GM16XX2) is 200 × 200, both black/white. The built-in layouts use the same elements on both, with box sizes taken from the model.

`make host` checks all of this on the PC and draws the results; `build/preview/<model>/epd_path.png` is rebuilt from a model of the controller RAM after `epd.c` has written it, so it also shows that the SPI stream, RAM window and address order fit together.

## Choices I have made (can be changed)

- **The "No AP found" screen** (requirement T9) is drawn after `CFG_LOST_AP_S` = 2 hours without an answer. The price then disappears from the screen. This is a deliberate choice so that faults are visible, but in some uses it is better to leave the price up. Raise `CFG_LOST_AP_S` to delay it, or set `lost_ap_s` per tag through the AP (65535 = never).
- **Longer interval without AP** (T11): the first two misses keep the interval, then it doubles up to 10 minutes (`CFG_MAX_POLL_S`, or `max_poll_s` from the AP).
- **One layout buffer:** the download overwrites the installed layout file, so while a download is unfinished the tag has no file (it reports version 0) and content for a file layout waits. Two buffers would keep the old file usable, at the cost of another 768 bytes of XRAM.
- **A rejected layout file** (bad CRC, wrong model, an element the tag cannot draw) is not tried again until the AP offers another version; the tag reports it with POLL flag bit2 and keeps drawing built-in layouts.
- **Settings from the AP:** `config.h` holds the built-in values. A CONFIG message from the AP (protokoll.md part 1) replaces `poll_s`, `max_poll_s` and `lost_ap_s` (0 = built-in), and the tag sends the message's `cfg_ver` back in every POLL as the acknowledgement. Values below the minimum (10 s, 60 s for `lost_ap_s`) are raised to it. The settings live in `app.cfg` and are kept in flash (T12) in the record described below. A missing or foreign record means the built-in values. `next_poll_s` in a reply still sets the interval as before.
- **Flash (T12):** two pages below the stock MAC, kept out of the firmware by `CODE_SIZE` (0xF400) in `build.py`:
  - 0xF800 (`hal_store_*`): one record with the settings and the last content: format 2, the settings (8 bytes), the content length, the layout file version the content was drawn with, and the CONTENT payload as it came from the AP (at most 110 bytes; the whole record is at most 124). Written after a CONFIG that changes the settings and after new content is drawn, always with the radio off. Format 1 records (settings only, from older firmware) are still read.
  - 0xF400 (`hal_layout_store_*`): the layout file, raw. It is written when the file in XRAM changes (installed or dropped), after the radio is off. At start-up the tag reads the page, finds the length from the file's structure and installs it with its CRC-32 as the version, with the same checks as a download. An erased page, or a write cut off half way, is not a valid file.
  - At start-up (`app_init()`) the tag draws the stored content instead of the start screen (T9), and reports its `content_id` and the layout version in the first POLL, so the AP answers IDLE and nothing is redrawn. Content drawn with a layout from the file is drawn again only if the same file version came back from flash; otherwise the tag shows the start screen and the AP sends the file and the content as for a new tag. A tag with nothing stored shows the start screen as before.
  - The pages are rewritten only when something changes, so wear follows how often the content changes (the ZBS243 flash endurance is not measured; at 10,000 erase cycles that is 27 years at one change a day). `hwtest` keeps a counter in the record page and a test pattern in the layout page, so run the firmware after `hwtest` to start clean.
- **Firmware over the air (T15)** ([docs/tag-ota.md](../docs/tag-ota.md)): `ota.c` downloads an image into the SPI flash (W25Q80, `hal/zbs243/xflash_zbs243.c`) a burst of blocks per wake-up, checks it, copies the running application to a rollback slot and restarts; the bootloader in page 0 (`boot/`) copies it into the chip. It is on whenever the SPI flash answers at start-up. Radio security (T16) is not done, so anyone in range who speaks the protocol could send firmware too; that was a deliberate choice.
- **No refresh for the same image:** `epd_draw` always writes the image into the display's RAM, but runs the refresh (the 2.9 s update that costs about as much as 280 polls and makes the screen flash) only when the image differs from the one the panel shows, compared with a CRC-32 computed while streaming (`epd_shown_crc`). This matters when the layout file changes: the AP then sends every tag its content again, and a tag whose own layout did not change draws it without a refresh. The CRC lives in RAM only: after a restart the first draw always refreshes, since the panel may show anything.
- **Retry:** once, after a random pause of 2-10 ms, as the protocol says. Within each listening window up to three frames are read, so a reply to another tag does not count as a miss.
- **No acknowledgement:** follows [protokoll.md](../docs/protokoll.md). The next POLL with the new `content_id` is the acknowledgement.
- **Ellipsis:** character `0x7F` in the fonts is "…", because Latin-1 does not have it. The AP sends "…" in content as "...", so 0x7F is only drawn by the tag itself.
- **Highlighted price:** `price_highlight` gives white digits in a black box (like the stock SoluM firmware), not just bold black. On the 2.9" the box only fits the mid font (36 px), so a highlighted price is smaller than a plain one.
- **Shrinking text:** a long name or text line changes to the small font before it is truncated. This shows more of the text, but labels side by side can end up with different font sizes.

## Missing hardware facts

The ZBS243 HAL is written and runs on the tag. What is still open: the display commands checked against a datasheet, battery voltage on the ST-GM16XX2 (issue #4), and the sleep current measured (phase 6). Every register fact that is used must go into [docs/kildelogg.md](../docs/kildelogg.md) with its source. The order follows [docs/fremdriftsplan.md](../docs/fremdriftsplan.md):

The original order, now done except as noted above:

1. Phase 0: the pins between ZBS243 and the display's FPC (`hal_epd_*`), which controller the display has, the MAC byte order in flash (`hal_mac_read`), the XRAM size (`--xram-size` in `build.py`).
2. Phase 1: clock, calibrated `hal_delay_ms`, GPIO, UART TX.
3. Phase 2: confirm the commands, polarity and RAM direction in `epd.c` on a real display.
4. Phase 3: the radio.
5. Phases 6 and 8: deep sleep, battery measurement, watchdog.
