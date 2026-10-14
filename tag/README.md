# Tag firmware (ZBS243 / SEM9110)

Own firmware for the SoluM ST-GM29XX2 (2.9", 296×128, black/white/red). The tag asks the AP for new content, receives name, price and text in one radio packet, and draws the screen itself with built-in fonts.

Background and requirements: [docs/tag-firmware.md](../docs/tag-firmware.md), [docs/protokoll.md](../docs/protokoll.md), [docs/krav.md](../docs/krav.md). All code here is written for this project; the hardware facts it relies on are listed with their sources in [docs/kildelogg.md](../docs/kildelogg.md).

## Status

| Part | Status |
|---|---|
| Packets: POLL, IDLE, CONTENT and the 802.15.4 frame around them (`proto.c`, `shared/proto.h`) | Done, tested on PC |
| Fonts: small (17 px), medium (24 px), large price font (52 px, digits and `,-.:` only) | Done, generated from Inter (OFL) |
| Column-by-column drawing (`draw.c`) and layouts "price" and "text" (`layout.c`) | Done, tested on PC |
| State machine: POLL, retry, draw only on a new `content_id`, longer interval without AP, "No AP found" screen (`app.c`) | Done, tested on PC with a fake radio |
| SSD16xx display driver over bit-banged SPI (`epd.c`) | Written, tested against fake pins. Commands are unconfirmed until the controller is confirmed (phase 0) |
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
    layout.c/.h         field placement, line wrapping, truncation with "…"
    draw.c/.h           computes one screen column (16 bytes per colour) at a time
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
  test/                 host tests, fake hardware and preview
  tools/
    build.py            builds the firmware, runs the tests and the preview
    gen_font.py         font generator (Pillow)
    label_design.py     checks label designs and turns them into AP fields
```

`shared/proto.h` in the root folder holds the packet definitions used by both the tag and the AP.

## Build and test

Requires Python 3 with Pillow. Without `make`, run `python tools/build.py <target>` from `tag/`.

| Command | What it does |
|---|---|
| `make test` | Compiles and runs the host tests with a normal C compiler, and the Python design tests |
| `make preview` | Draws example content to `build/preview/*.png` |
| `make fw` | Builds the firmware with SDCC to `build/fw/openesl.ihx` and `.bin` |
| `make fonts` | Regenerates `fonts/font_*.c` |

**Tool versions (requirement G5):**
- SDCC **4.5.0** (`winget install SDCC.SDCC --version 4.5.0`). `build.py` warns if another version is used. Flags: `-mmcs51 --model-large --stack-auto --std-c99 --opt-code-size`, and the code is linked below `0xFC00` because the stock MAC is at `0xFC06`.
- Host compiler: `CC`, otherwise gcc, clang, cc, or `python -m ziglang cc`.
- The fonts are generated with Pillow 11.1.0 from Inter 3.019. The generated files are checked in, so the build is the same even if Pillow changes.

Flash with [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher) (`write build/fw/openesl.bin`). Back up the original firmware with `read` first.

**Size now (SDCC 4.5.0, 2026-10-12):** 28,808 bytes of flash (about 14 KB of it is fonts), 680 bytes of XRAM, and 247 bytes free for the stack in internal RAM. Whether the stack is enough for the deepest call chain (POLL → layout → drawing), and whether ZBS243 has at least 680 bytes of XRAM, has not been measured yet. Both must be checked in phase 1.

## Label design (JSON)

Design files are in `designs/`. The format and rules are at the top of `tools/label_design.py`. A design says which layout the label uses, which fields it fills (with placeholders such as `{name}`), and the styles. `render` gives the fields in the same form as the body of `PUT /api/tags/{mac}`, so a client can send them directly.

```bash
python tools/label_design.py check designs/*.json
python tools/label_design.py render designs/pris-standard.json data.json
python -m unittest discover -s test -p "test_label_*.py"
```

The design does not change the firmware. The tag still only knows the layouts "price" and "text". The styles `price_red` and `frame_red` map to flag bits 0 and 1 and have the same names as in the API.

## How the drawing works

ZBS243 does not have RAM for a whole image (296 × 128 × 2 colours = 9472 bytes). The display controller has its own image memory, so `epd.c` asks `draw_column(x)` for one column at a time (128 pixels = 16 bytes per colour) and writes it straight in. The black plane is sent first, then red, which means everything is computed twice. That costs some CPU, but no RAM.

- `layout.c` turns a CONTENT packet into a list of text runs (font, colour, position, pointer to the text) and filled rectangles (the red frame).
- `draw.c` goes through the runs for each column. Each text run has a cursor that moves forward as x increases, so it does not search the text for every column.
- The fonts are stored column by column, so one font column can be ORed straight into the screen column.
- Where black and red overlap, red wins.

Layout 1, "price": name in the medium font on up to two lines at the top, price right-aligned in the large font (red if flag bit 0 is set), subtitle in the small font at the bottom. The price falls back to the medium font if the large font lacks a character (e.g. "2 for 30") or the price is too wide.
Layout 2, "text": four lines in the medium font.
Flag bit 1 gives a red frame around the whole screen. Long text is truncated with "…".

Run `make preview` to see what the example content looks like.

## Choices I have made (can be changed)

- **The "No AP found" screen** (requirement T9) is drawn after `CFG_LOST_AP_S` = 2 hours without an answer. The price then disappears from the screen. This is a deliberate choice so that faults are visible, but in some uses it is better to leave the price up. Raise `CFG_LOST_AP_S` to delay it.
- **Longer interval without AP** (T11): the first two misses keep the interval, then it doubles up to 10 minutes.
- **Retry:** once, after a random pause of 2-10 ms, as the protocol says. Within each listening window up to three frames are read, so a reply to another tag does not count as a miss.
- **No acknowledgement:** follows [protokoll.md](../docs/protokoll.md). The next POLL with the new `content_id` is the acknowledgement.
- **Ellipsis:** character `0x7F` in the fonts is "…", because Latin-1 does not have it.

## Missing hardware facts

Everything under `src/hal/zbs243/` is a stub marked `TODO phase N`. Every register fact that is used must go into [docs/kildelogg.md](../docs/kildelogg.md) with its source. The order follows [docs/fremdriftsplan.md](../docs/fremdriftsplan.md):

1. Phase 0: the pins between ZBS243 and the display's FPC (`hal_epd_*`), which controller the display has, the MAC byte order in flash (`hal_mac_read`), the XRAM size (`--xram-size` in `build.py`).
2. Phase 1: clock, calibrated `hal_delay_ms`, GPIO, UART TX.
3. Phase 2: confirm the commands, polarity and RAM direction in `epd.c` on a real display.
4. Phase 3: the radio.
5. Phases 6 and 8: deep sleep, battery measurement, watchdog.
