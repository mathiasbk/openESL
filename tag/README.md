# Tag firmware (ZBS243 / SEM9110)

Own firmware for the SoluM ST-GM29XX2 (2.9", 296×128, black/white) and the ST-GM16XX2 (1.54", 200×200, black/white), and experimental for the ST-GR29000 (2.9", black/white/red, not tried on a label yet), one build per model (`src/model.h`). The tag asks the AP for news, downloads the screen the AP has drawn (a 1-bit image, IMG blocks) into its SPI flash and streams it to the display. It draws only its own status screens (start-up, "No AP found") with built-in fonts.

Background and requirements: [docs/tag-firmware.md](../docs/tag-firmware.md), [docs/protokoll.md](../docs/protokoll.md), [docs/krav.md](../docs/krav.md). All code here is written for this project; the hardware facts it relies on are listed with their sources in [docs/kildelogg.md](../docs/kildelogg.md).

## Status

| Part | Status |
|---|---|
| Packets: POLL (with the model byte, T18, `cfg_ver`, and the link block with RSSI/LQI of the last answer, lost rounds and a retry flag), IDLE, CONFIG, IMG, IMG_REQ, FW, FW_REQ and the 802.15.4 frame around them (`proto.c`, `shared/proto.h`) | Done, tested on PC |
| Settings from the AP (CONFIG: interval, backoff cap, "No AP found" delay, long backoff), kept in flash through `hal_store_*` (`app.c`) | Done, tested on PC (fake flash); the flash store itself is tested on the tag by `hwtest`, not yet with CONFIG on the air |
| Screen images from the AP ([protokoll.md](../docs/protokoll.md) part 1, "Image transfer"): download into the SPI flash slot, CRC-32 check, the 2D and RLE codes, streamed to the panel (`image.c`, `app.c`); the slot is kept, so a restart shows the last screen without the AP (T12) | Done, tested on PC and on the 1.54" (hardware test C) |
| Fonts: small (17 px) and medium (24 px) for the status screens; the price fonts large (52 px) and mid (36 px), digits and ` ,-.:` only, are drawn by the AP | Done, generated from Inter (OFL) |
| Column-by-column drawing (`draw.c`) of the status screens (`status.c`) for both models | Done, tested on PC for both models |
| State machine: POLL, retry, draw only a new image, longer interval without AP, once an hour after a day without AP, "No AP found" screen (`app.c`) | Done, tested on PC with a fake radio; the long backoff not yet seen on hardware |
| SSD16xx display driver over bit-banged SPI (`epd.c`) | Written, tested against fake pins and a model of the controller RAM. Draws correctly on the 1.54" and the 2.9" (RAM layout and direction confirmed on the tags, [kildelogg.md](../docs/kildelogg.md), the 2.9" mirror fix in #71); the commands are not checked against a datasheet. The core stays awake while the panel refreshes: sleeping through it (0.9.0) left the image half drawn |
| Clock, GPIO, UART, timer, watchdog, temperature, flash, deep sleep (`hal/zbs243/`), and `hwtest.c` to check them | Done and running on the tag; facts in [kildelogg.md](../docs/kildelogg.md). Battery voltage reads 0 mV (unknown) on the ST-GM16XX2 (issue #4); it is read every 240th POLL (4 hours at 60 s) |
| Radio (`hal/zbs243/radio_zbs243.c`) | Done, sends and receives with the C6 (phases 3 and 5) |

## Folders

```
tag/
  Makefile              thin wrapper around tools/build.py
  src/
    main.c              start-up and main loop (ZBS243 only)
    app.c/.h            the state machine, with no hardware code
    proto.c, proto_tag.h  build POLL, IMG_REQ, FW_REQ and the frame, parse IDLE/CONFIG/IMG/FW
    model.h             screen size per tag model (MODEL=29 or 154)
    status.c/.h         the status screens: up to four lines, font fallback, wrapping, "…"
    image.c/.h          the screen image from the AP in the SPI flash slot
    barcode.c/.h        Code 128 and EAN-13 as bars (used by the AP's drawing, esl_render.c)
    draw.c/.h           computes one screen column (DRAW_H/8 bytes) at a time
    font.c/.h           lookup in the font tables
    epd.c/.h            SSD16xx over bit-banged SPI, driven by the model's panel header
    panels/             one header per panel: RAM direction, update sequence, init commands
    config.h            intervals, channel, PAN ID (the version comes from ../tools/fw_version.py)
    compat.h            lets the same code compile with SDCC and on the PC
    hal/hal.h           everything the app needs from the hardware
    hal/hal_epd.h       the display pins
    hal/zbs243/         the ZBS243 implementation (HAL, radio, hwtest.c)
  boot/
    boot_core.c/.h      the bootloader's decisions, the same for every chip
    zbs243/             the ZBS243's half of the bootloader (flash and SPI flash access)
  fonts/
    font_*.c            generated font tables (checked in)
    preview_*.png       what the fonts look like
    src/                the Inter source fonts
    OFL.txt             the font licence
  designs/              label designs in JSON (see below)
    examples/           example data per design, drawn by tools/render_host.py
    layouts/            example layout files per model (2.9.json, 1.54.json), protokoll.md part 3
  test/                 host tests, fake hardware
  tools/
    build.py            builds the firmware and runs the tests (default model)
    render_host.py      tests and previews for every model, including the JSON designs
    gen_font.py         font generator (freetype-py + Pillow)
    label_design.py     checks label designs and turns them into AP fields
```

`shared/proto.h` in the root folder holds the packet definitions used by both the tag and the AP.

### Another chip

Only `src/hal/<chip>/`, `boot/<chip>/` and `src/main.c` know the chip; the rest of `src/` is
plain C99 that also runs in the host tests. A tag with another chip (another maker's tag, say)
needs:

1. `src/hal/<chip>/`: what `hal/hal.h` and `hal/hal_epd.h` declare (clock, watchdog, MAC,
   battery, the settings record, SPI flash, radio, display pins, sleep).
2. `boot/<chip>/`: the hardware functions in `boot/boot_core.h`, if the chip takes firmware
   updates over the air.
3. An entry in `CHIPS` in `tools/build.py`: its sources and how it is compiled (today only SDCC
   for the 8051 core). The application's start and end must be `ESL_FW_APP_BASE` and
   `ESL_FW_APP_END` in `shared/proto.h`, which the bootloader, `src/ota.c` and the AP use for
   every chip; `build.py` checks it. The shared code uses SDCC's `__code` and `__xdata`, which
   `src/compat.h` defines away on the PC; another compiler needs the same there.
4. A panel header in `src/panels/` if its display is driven unlike the ones there.
5. The model in `shared/models.json` with that `chip` (`tools/gen_models.py` refuses a chip
   `CHIPS` does not have).

Every hardware fact the new code relies on goes in `docs/kildelogg.md`, as for the ZBS243.

## Build and test

Requires Python 3 with Pillow (and freetype-py to regenerate the fonts). Without `make`, run `python tools/build.py <target>` from `tag/`.

| Command | What it does |
|---|---|
| `make test` | Compiles and runs the host tests with a normal C compiler, and the Python design tests |
| `make host` | Runs the host tests for every model and draws the examples, the layouts in `designs/layouts/<model>.json` (`layout-<name>.png`) and the designs to `build/preview/<model>/*.png`, with everything on one sheet in `all.png` (`python tools/render_host.py [29\|154]`) |
| `make fw` | Builds the bootloader and the application with SDCC: `build/fw/openesl.bin` (both, for the flasher) and `build/fw/openesl-ota.bin` (the application with its header, for `esl-client tag-ota`) |
| `make fonts` | Regenerates `fonts/font_*.c` |

**Tool versions (requirement G5):**
- SDCC **4.5.0** (`winget install SDCC.SDCC --version 4.5.0`). `build.py` warns if another version is used. Flags: `-mmcs51 --model-large --stack-auto --std-c99 --opt-code-size`, and the code is linked below `0xF400` (`CODE_SIZE`): 0x0000-0x03FF holds the bootloader (`boot/`, built with `--model-small`, must stay under 1 KB), the application starts at 0x0400, 0xF400-0xF7FF is free, 0xF800-0xFBFF holds the record, and the stock MAC is at `0xFC06`.
- Host compiler: `CC`, otherwise gcc, clang, cc, or `python -m ziglang cc`.
- The fonts are generated from Inter 3.019 with freetype-py 2.5.1 (FreeType 2.13.2): FreeType's auto-hinter, light target, thresholded from grey. Inter has no hinting of its own, and plain monochrome rendering gave ragged small text. Size, height and baseline are fixed in `tools/gen_font.py`, so the metrics are the same on every machine; the generated files are checked in. Regenerate with `pip install freetype-py pillow` and `python tools/gen_font.py`.
- Model: `-DMODEL=29` (default) or `-DMODEL=154`. `render_host.py` builds both on the PC. `MODEL=154 python tools/build.py fw` builds the 1.54" to `build/fw154/openesl.bin`; the default is the 2.9" in `build/fw/`.

Flash with [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher) (`write build/fw/openesl.bin`). Back up the original firmware with `read` first. After that, new firmware can go over the radio: `esl-client tag-ota build/fw/openesl-ota.bin` hands it to the AP, which sends it to the tags ([docs/tag-ota.md](../docs/tag-ota.md)). Tags flashed before 0.3.0 have no bootloader and need one more pass with the flasher. Tag firmware 0.4.7 and older draws the content itself from CONTENT and LAYOUT and does not take images, so every tag needs this firmware once: over the radio if it has the bootloader (FW and FW_REQ have not changed), otherwise with the flasher. The image slot is in the SPI flash, so a tag whose SPI flash does not answer shows only its status screens (the 2.9" SPI flash is not probed yet, [docs/tag-ota.md](../docs/tag-ota.md)).

**Size now (SDCC 4.5.0, 2026-10-20, `build.py fw`, with OTA and images):** the bootloader is 994 of 1,024 bytes; the application is 39,654 bytes for the 2.9" and 40,079 for the 1.54" (of 61,440 from 0x0400 to `CODE_SIZE`), 1,756 (2.9") and 1,974 (1.54") bytes of XRAM from 0xE300 and 223 bytes free for the stack in internal RAM. Leaving the drawing of content to the AP took about 19 KB of code and 1 KB of XRAM off the tag.

## Label design (JSON)

Design files are in `designs/`. The format and rules are at the top of `tools/label_design.py`. A design says which layout the label uses, which fields it fills (with placeholders such as `{name}`), and the styles. `render` gives the fields in the same form as the body of `PUT /api/tags/{mac}`, so a client can send them directly.

```bash
python tools/label_design.py check designs/*.json
python tools/label_design.py render designs/pris-standard.json data.json
python -m unittest discover -s test -p "test_label_*.py"
```

The design does not change the firmware, and the tag never sees it: the AP draws the screen. Its layout is one of the built-in layouts "price" and "text", or a layout from a layout file (`designs/layouts/<model>.json`, uploaded to the AP); the field names then come from that file (`render <design> <data> designs/layouts/2.9.json`). The styles `price_highlight` and `frame` map to flag bits 0 and 1 and have the same names as in the API. Characters the AP replaces itself (dashes, curly quotes, "…") are allowed, and the length limit is counted after the replacement, as on the AP.

The example layout files hold `shelf` and `sign`, and four product labels: `vare` (brand in a black band, name, article number/EAN, price), `vare-mva` (brand, name, a small note such as the price inc. VAT, and the highlighted price), `vare-ean` (brand, name, price, and the EAN-13 as a barcode with the digits under it), and `vare-priser` (brand, name, a note, the price ex. VAT and the highlighted price inc. VAT, fields `price` and `price_ex`; the client sends both, so each product can have its own VAT rate, as in `designs/vare-priser.json`). A layout takes at most six fields from the content, but fixed texts such as "inkl. mva" can live in the layout file itself (`"text": "inkl. mva"` instead of `"field"`, as in `vare-ean`), so they need no field. A label text that changes with the content still comes from the design, as in `designs/vare-eks-mva.json` (`"note": "inkl. mva {price_inc}"`).

`make host` draws every design with the data in `designs/examples/<id>.json` to `build/preview/<model>/design-<id>-<n>.png`. A design for a file layout is drawn for every model whose layout file has that layout. The screens are drawn by the AP's own code (`esl_render.c`, built on the PC by `web/build.py --native`), so they are the images the AP sends; an error in a layout file stops the run with the same message the AP would answer with.

## How the drawing works

The AP draws every screen with content (`ap/components/esl_core/esl_render.c`: the built-in layouts "price" and "text", layout files, barcodes) and sends it as an image. The tag keeps the image in its SPI flash slot (`image.c`) and `epd_draw_image()` streams it to the panel a column at a time; the tag needs no layouts, price fonts or barcodes.

The tag draws only its status screens, and ZBS243 does not have RAM for a whole image (296 × 128 = 4736 bytes on the 2.9", 5000 on the 1.54"). The display controller has its own image memory, so `epd.c` asks `draw_column(x)` for one column at a time (128 pixels = 16 bytes on the 2.9") and writes it straight in. Both tags are black/white, so only the black plane is sent (`MODEL_HAS_RED` = 0 for both).

- `status.c` turns up to four lines of text into text runs (font, position, pointer to the text): one line per quarter of the screen, left-aligned and centred vertically, in the medium font, or wrapped in the small font with "…" at the end if it does not fit.
- `draw.c` goes through the runs for each column. Each text run has a cursor that moves forward as x increases, so it does not search the text for every column.
- The fonts are stored column by column, so one font column can be ORed straight into the screen column.

`make host` checks this on the PC; `build/preview/<model>/epd_path.png` is rebuilt from a model of the controller RAM after `epd.c` has written it, so it also shows that the SPI stream, RAM window and address order fit together.

## Choices I have made (can be changed)

- **The "no AP" mark** (requirement T9, #98): after `CFG_LOST_AP_S` = 2 hours without an answer the tag draws its image again from the slot with a small mark (signal bars with a cross, on a white 19 × 20 square) in the top right corner, so the price stays up and the fault still shows. It is one refresh when the AP goes and one when it comes back; the image is not downloaded again. A tag with no image (or only part of a new one in its slot) shows the "No AP found" screen instead. The mark covers that corner of the layout. Raise `CFG_LOST_AP_S` to delay it, or set `lost_ap_s` per tag through the AP (65535 = never).
- **Longer interval without AP** (T11): the first two misses keep the interval, then it doubles up to 10 minutes (`CFG_MAX_POLL_S`, or `max_poll_s` from the AP).
- **Settings from the AP:** `config.h` holds the built-in values. A CONFIG message from the AP (protokoll.md part 1) replaces `poll_s`, `max_poll_s` and `lost_ap_s` (0 = built-in), and the tag sends the message's `cfg_ver` back in every POLL as the acknowledgement. Values below the minimum (10 s, 60 s for `lost_ap_s`) are raised to it. The settings live in `app.cfg` and are kept in flash (T12) in the record described below. A missing or foreign record means the built-in values. `next_poll_s` in a reply still sets the interval as before.
- **Flash (T12):** one page below the stock MAC, kept out of the firmware by `CODE_SIZE` (0xF400) in `build.py`:
  - 0xF800 (`hal_store_*`): one record with the settings: format 1 and the settings (8 bytes). Written after a CONFIG that changes them, always with the radio off. A record of another format is ignored.
  - The screen itself is the image in the SPI flash slot. At start-up (`app_init()`) the tag shows it instead of the start screen (T9) and reports its id in the first POLL, so the AP answers IDLE and nothing is redrawn. A tag with no image shows the start screen.
  - The page is rewritten only when the settings change. `hwtest` keeps a counter in the record page, so run the firmware after `hwtest` to start clean.
- **Firmware over the air (T15)** ([docs/tag-ota.md](../docs/tag-ota.md)): `ota.c` downloads an image into the SPI flash (W25Q80, `hal/zbs243/xflash_zbs243.c`) a burst of blocks per wake-up, checks it, copies the running application to a rollback slot and restarts; the bootloader in page 0 (`boot/`) copies it into the chip. It is on whenever the SPI flash answers at start-up. Radio security (T16) is not done, so anyone in range who speaks the protocol could send firmware too; that was a deliberate choice.
- **Retry:** once, after a random pause of 2-10 ms, as the protocol says. Within each listening window up to three frames are read, so a reply to another tag does not count as a miss.
- **No acknowledgement:** follows [protokoll.md](../docs/protokoll.md). The next POLL with the new `content_id` is the acknowledgement.
- **Ellipsis:** character `0x7F` in the fonts is "…", because Latin-1 does not have it. The AP turns "…" in content into "...", so 0x7F is only drawn where text is cut off.

## Missing hardware facts

The ZBS243 HAL is written and runs on the tag. What is still open: the display commands checked against a datasheet, battery voltage on the ST-GM16XX2 (issue #4), and the sleep current measured (how: [strombudsjett.md](../docs/strombudsjett.md), "Measuring a tag"). Every register fact that is used must go into [docs/kildelogg.md](../docs/kildelogg.md) with its source. The order follows [docs/fremdriftsplan.md](../docs/fremdriftsplan.md):

The original order, now done except as noted above:

1. Phase 0: the pins between ZBS243 and the display's FPC (`hal_epd_*`), which controller the display has, the MAC byte order in flash (`hal_mac_read`), the XRAM size (`--xram-size` in `build.py`).
2. Phase 1: clock, calibrated `hal_delay_ms`, GPIO, UART TX.
3. Phase 2: confirm the commands, polarity and RAM direction in `epd.c` on a real display.
4. Phase 3: the radio.
5. Phases 6 and 8: deep sleep, battery measurement, watchdog.
