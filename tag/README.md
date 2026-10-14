# Tag firmware (ZBS243 / SEM9110)

Own firmware for the SoluM ST-GM29XX2 (2.9", 296×128, black/white) and the ST-GM16XX2 (1.54", 200×200, black/white), one build per model (`src/model.h`). The tag asks the AP for new content, receives name, price and text in one radio packet, and draws the screen itself with built-in fonts.

Background and requirements: [docs/tag-firmware.md](../docs/tag-firmware.md), [docs/protokoll.md](../docs/protokoll.md), [docs/krav.md](../docs/krav.md). All code here is written for this project; the hardware facts it relies on are listed with their sources in [docs/kildelogg.md](../docs/kildelogg.md).

## Status

| Part | Status |
|---|---|
| Packets: POLL (with the model byte, T18), IDLE, CONTENT and the 802.15.4 frame around them (`proto.c`, `shared/proto.h`) | Done, tested on PC |
| Fonts: small (17 px), medium (24 px), price fonts large (52 px) and mid (36 px) with digits and ` ,-.:` only | Done, generated from Inter (OFL) |
| Column-by-column drawing (`draw.c`), layout interpreter with built-in layouts "price" and "text" (`layout.c`) for both models | Done, tested on PC for both models, previews in `build/preview/<model>/` |
| Layouts sent from the AP (G7/T17) | Interpreter ready (`layout_apply`); the radio format is not agreed yet |
| State machine: POLL, retry, draw only on a new `content_id`, longer interval without AP, "No AP found" screen (`app.c`) | Done, tested on PC with a fake radio |
| SSD16xx display driver over bit-banged SPI (`epd.c`) | Written, tested against fake pins and a model of the controller RAM. Commands are unconfirmed until the controller is confirmed (phase 0); the 1.54" controller is not known at all yet |
| Clock, GPIO, UART, timer, watchdog, battery, deep sleep (`hal/zbs243/`) | Stubs, waiting for register facts (phases 1 and 6) |
| Radio (`hal/zbs243/radio_zbs243.c`) | Stub, waiting for phase 3 |
| Store the last content in flash (T12) | Not started |

## Folders

```
tag/
  Makefile              thin wrapper around tools/build.py
  src/
    main.c              start-up and main loop (ZBS243 only)
    app.c/.h            the state machine, with no hardware code
    proto.c, proto_tag.h  build POLL and the frame, parse IDLE/CONTENT
    model.h             screen size per tag model (MODEL=29 or 154)
    layout.c/.h         layout elements, font fallback, line wrapping, truncation with "…"
    draw.c/.h           computes one screen column (DRAW_H/8 bytes) at a time
    font.c/.h           lookup in the font tables
    epd.c/.h            SSD16xx over bit-banged SPI
    config.h            intervals, channel, PAN ID, version
    compat.h            lets the same code compile with SDCC and on the PC
    hal/hal.h           everything the app needs from the hardware
    hal/hal_epd.h       the display pins
    hal/zbs243/         the ZBS243 implementation (stubs for now)
  fonts/
    font_*.c            generated font tables (checked in)
    preview_*.png       what the fonts look like
    src/                the Inter source fonts
    OFL.txt             the font licence
  designs/              label designs in JSON (see below)
    examples/           example data per design, drawn by tools/render_host.py
  test/                 host tests, fake hardware and preview
  tools/
    build.py            builds the firmware, runs the tests and the preview (default model)
    render_host.py      tests and previews for every model, including the JSON designs
    gen_font.py         font generator (Pillow)
    label_design.py     checks label designs and turns them into AP fields
```

`shared/proto.h` in the root folder holds the packet definitions used by both the tag and the AP.

## Build and test

Requires Python 3 with Pillow. Without `make`, run `python tools/build.py <target>` from `tag/`.

| Command | What it does |
|---|---|
| `make test` | Compiles and runs the host tests with a normal C compiler, and the Python design tests |
| `make preview` | Draws example content to `build/preview/*.png` (2.9") |
| `make host` | Runs the host tests for every model and draws the examples and the designs to `build/preview/<model>/*.png`, with everything on one sheet in `all.png` (`python tools/render_host.py [29\|154]`) |
| `make fw` | Builds the firmware with SDCC to `build/fw/openesl.ihx` and `.bin` |
| `make fonts` | Regenerates `fonts/font_*.c` |

**Tool versions (requirement G5):**
- SDCC **4.5.0** (`winget install SDCC.SDCC --version 4.5.0`). `build.py` warns if another version is used. Flags: `-mmcs51 --model-large --stack-auto --std-c99 --opt-code-size`, and the code is linked below `0xFC00` because the stock MAC is at `0xFC06`.
- Host compiler: `CC`, otherwise gcc, clang, cc, or `python -m ziglang cc`.
- The fonts are generated with Pillow 11.1.0 from Inter 3.019. The generated files are checked in, so the build is the same even if Pillow changes. The output also depends on the FreeType build inside Pillow (Windows and Linux differ by a few pixels), so regenerate only the font you change: `python tools/gen_font.py mid`. `font_mid.c` was made with Pillow 11.1.0 / FreeType 2.13.2 on Linux, the others on Windows.
- Model: `-DMODEL=29` (default) or `-DMODEL=154`. `render_host.py` builds both on the PC. `build.py fw` does not pass `MODEL` yet, so the firmware is always the 2.9".

Flash with [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher) (`write build/fw/openesl.bin`). Back up the original firmware with `read` first.

**Size now (SDCC 4.5.0, 2026-10-12):** 28,808 bytes of flash (about 14 KB of it is fonts), 680 bytes of XRAM, and 247 bytes free for the stack in internal RAM. Whether the stack is enough for the deepest call chain (POLL → layout → drawing), and whether ZBS243 has at least 680 bytes of XRAM, has not been measured yet. Both must be checked in phase 1. After that measurement the layout work added the mid price font (about 1.6 KB of flash), the layout tables (about 250 bytes) and about 60 bytes of XRAM (two more text runs and the line buffers in `layout.c`); not re-measured, because SDCC was not available.

## Label design (JSON)

Design files are in `designs/`. The format and rules are at the top of `tools/label_design.py`. A design says which layout the label uses, which fields it fills (with placeholders such as `{name}`), and the styles. `render` gives the fields in the same form as the body of `PUT /api/tags/{mac}`, so a client can send them directly.

```bash
python tools/label_design.py check designs/*.json
python tools/label_design.py render designs/pris-standard.json data.json
python -m unittest discover -s test -p "test_label_*.py"
```

The design does not change the firmware. The tag still only knows the layouts "price" and "text". The styles `price_highlight` and `frame` map to flag bits 0 and 1 and have the same names as in the API. Characters the AP replaces itself (dashes, curly quotes, "…") are allowed, and the length limit is counted after the replacement, as on the AP.

`make host` draws every design with the data in `designs/examples/<id>.json` to `build/preview/<model>/design-<id>-<n>.png`.

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

The element format is meant to also carry layouts from the AP later (G7/T17); `layout_apply()` takes the list from RAM as well as from flash. The format is documented in `src/layout.h` and is a proposal until the radio message is agreed.

**Models.** `src/model.h` holds the screen size. The 2.9" is 296 × 128 and the 1.54" (ST-GM16XX2) is 200 × 200, both black/white. The built-in layouts use the same elements on both, with box sizes taken from the model.

`make host` checks all of this on the PC and draws the results; `build/preview/<model>/epd_path.png` is rebuilt from a model of the controller RAM after `epd.c` has written it, so it also shows that the SPI stream, RAM window and address order fit together.

## Choices I have made (can be changed)

- **The "No AP found" screen** (requirement T9) is drawn after `CFG_LOST_AP_S` = 2 hours without an answer. The price then disappears from the screen. This is a deliberate choice so that faults are visible, but in some uses it is better to leave the price up. Raise `CFG_LOST_AP_S` to delay it.
- **Longer interval without AP** (T11): the first two misses keep the interval, then it doubles up to 10 minutes.
- **Retry:** once, after a random pause of 2-10 ms, as the protocol says. Within each listening window up to three frames are read, so a reply to another tag does not count as a miss.
- **No acknowledgement:** follows [protokoll.md](../docs/protokoll.md). The next POLL with the new `content_id` is the acknowledgement.
- **Ellipsis:** character `0x7F` in the fonts is "…", because Latin-1 does not have it. The AP sends "…" in content as "...", so 0x7F is only drawn by the tag itself.
- **Highlighted price:** `price_highlight` gives white digits in a black box (like the stock SoluM firmware), not just bold black. On the 2.9" the box only fits the mid font (36 px), so a highlighted price is smaller than a plain one.
- **Shrinking text:** a long name or text line changes to the small font before it is truncated. This shows more of the text, but labels side by side can end up with different font sizes.

## Missing hardware facts

Everything under `src/hal/zbs243/` is a stub marked `TODO phase N`. Every register fact that is used must go into [docs/kildelogg.md](../docs/kildelogg.md) with its source. The order follows [docs/fremdriftsplan.md](../docs/fremdriftsplan.md):

1. Phase 0: the pins between ZBS243 and the display's FPC (`hal_epd_*`), which controller the display has, the MAC byte order in flash (`hal_mac_read`), the XRAM size (`--xram-size` in `build.py`).
2. Phase 1: clock, calibrated `hal_delay_ms`, GPIO, UART TX.
3. Phase 2: confirm the commands, polarity and RAM direction in `epd.c` on a real display.
4. Phase 3: the radio.
5. Phases 6 and 8: deep sleep, battery measurement, watchdog.
