# Tag firmware (ZBS243 / SEM9110)

## Tools
- **SDCC** (Small Device C Compiler) for the 8051, pinned to one version (see `tag/README.md`). Different versions give different code. The SDCC runtime library is GPL with a linking exception, so it does not restrict the firmware's licence.
- **Python** (`tag/tools/build.py`) builds the firmware and packs `.ihx` into `.bin` for the flasher.
- **Flasher:** [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher) by atc1441 on a D1 mini (`read`, `write`, `mac -m`), with pogo pins to the tag's debug pads.
- **Debugging:** UART output from the tag (pad 4 to the flasher) and a logic analyser.
- **8051 disassembler:** Ghidra (supports the 8051) for studying a backup of the tag's original SoluM firmware.

## What is known and what must be found out

| Area | Publicly documented? | How to find out |
|---|---|---|
| The 8051 core (instructions, standard SFRs, interrupts) | Yes, the Intel MCS-51 documentation | Read the documentation |
| The SSD16xx display controller (commands, init, OTP LUT) | Yes, datasheet from Solomon Systech | Read the datasheet. Confirm which controller it is (often printed on the FPC cable, otherwise by reading the status/ID register) |
| Which ZBS243 pins go to the display, EEPROM, battery | Partly, from community research (see Credits in the main README) | Measure with a multimeter from the chip pin to the FPC connector. Note in [kildelogg.md](kildelogg.md) |
| ZBS243-specific registers: clock, GPIO direction, UART, timer, SPI | Partly, from community research (see Credits in the main README) | Public write-ups, experiments, and study of a backup of the original firmware |
| Radio registers (channel, transmit, receive, RSSI) | Partly, as above | As above. **This is the largest uncertainty in the project.** |
| Deep sleep and wake-up with timer | Partly, as above | As above, plus measuring current draw |
| Where the MAC is stored in flash | Yes (`mac -m` in the flasher, a flash dump) | Dump the info page with the flasher and locate the bytes |

## Simplifications for version 1
- **SPI to the display can be bit-banged** with GPIO. Then you only need to know the GPIO registers to get the display running. The display is slow anyway, so speed does not matter.
- **Use the display's built-in OTP waveform** (update mode from the datasheet). Then you avoid your own LUTs, which are the hardest part of an e-paper display.
- **No external EEPROM in version 1.** The last content (max 110 bytes) and the settings are kept in one flash page in the ZBS243 and the layout file in another (requirement T12), so a restarted tag draws its content again without the AP (`tag/README.md`, "Flash").
- **No interrupt-driven radio at first.** Send, poll the status register in a loop with a timeout, read the packet. Simple to debug.

## Modules

| Module | Responsibility | Depends on |
|---|---|---|
| `hal_gpio`, `hal_clock`, `hal_uart` | Clock, pins, debug output | Register findings |
| `hal_timer`, `power` | Delays, deep sleep, wake after N seconds, watchdog | Register findings |
| `epd` | SSD16xx: reset, init, write the black/white plane, update, wait for BUSY, sleep | GPIO, datasheet |
| `font` | Generated tables: small (17 px) and medium (24 px) in Latin-1, mid (36 px) and large (52 px) with digits and ` ,-.:` only | Font generator |
| `draw` | Calculates one pixel column at a time from layout + text and streams it to `epd` | `font` |
| `layout` | Element interpreter (text, rectangle, barcode, fixed text) for the built-in layouts "price" and "text" and up to 16 layouts from the downloaded file: positions, line wrapping, truncation with «…» | `draw` |
| `barcode` | Code 128 and EAN-13 as bars for the barcode element | `draw` |
| `radio` | Init, set channel/PAN, send frame, receive frame with timeout, RSSI | Register findings |
| `proto` | Build POLL/LAYOUT_REQ, parse IDLE/CONTENT/CONFIG/LAYOUT ([protokoll.md](protokoll.md)) | `radio`, `shared/proto.h` |
| `battery` | Voltage from the display controller's voltage detector (100 mV steps, once every 60 POLLs); 0 = unknown on the ST-GM16XX2 (issue #4) | Register findings |
| `main` | State machine: start-up → start-up screen → loop {wake, POLL, handle, sleep} | Everything |

## Display 2.9": 296×128, black/white
- Both tags are black/white only; there is no red. Emphasis is done in black/white, as the stock SoluM firmware does: an inverted field (white text on a black box, e.g. the price) and a black frame.
- The display controller has its own image memory (4736 bytes for the black/white plane). The ZBS243 has too little RAM for a whole image, so `draw` makes one column at a time (16 bytes) and writes it straight in.
- A full update takes a few seconds and draws a few mA. That is why the tag only draws when `content_id` changes (requirement T5), and `epd` skips the update itself when the new image has the same CRC-32 as the one the panel shows (`epd_shown_crc`).
- The display must be put into deep sleep after drawing, or it will drain the battery.

## Multiple tag sizes: 2.9" and 1.54" (requirements G6, T19)
- The 1.54" (ST-GM16XX2) is 200×200, black/white only (reported from hardware inspection). The display controller and pins are confirmed in phase 0 in the same way as for the 2.9", and noted in [kildelogg.md](kildelogg.md). Like the 2.9" it has no red, so only the black/white plane is sent.
- Everything that depends on the display is collected in one model description: width, height, column height in bytes, controller type, init sequence, default layouts. `epd`, `draw` and `layout` read the values from there and have no fixed numbers for 296×128.
- One firmware build per model from the same code, selected with a build option (e.g. `MODEL=29` / `MODEL=154`). The model is reported to the AP in POLL (requirement T18).

## Tag identity: MAC, model and barcode
- The stock firmware keeps 6 bytes at flash 0xFC06–0xFC0B. Printed as 12 hex digits, they are the number under the Code 128 barcode on the label: tag 0000020663463415 has `020663461013` (barcode decoded from a photo, 2026-10-16; its first 8 digits are in the MAC, the last 4 are inferred and not yet checked against a backup).
- The flashers (`tools/flash-tui`, the Flash tag page) build the MAC from those bytes the same way as ZBS_Flasher: `00 00`, the first 4 bytes, the model code (0x3B = 2.9", 0x34 = 1.54"), and 0x10 with the XOR of the nibbles of bytes 2–7 in the low nibble. They write it to info page 0x10–0x17 before the main flash (and with it the stock bytes) is erased. They also show the barcode, log it (`flash_log.csv`) and keep it with the backup.
- So the MAC follows from the label and the model alone: `esl_client.py mac <12 digits> [--model 2.9|1.54]`, or the rule in the API description (`docs/openapi.yaml`, "Which tag is which"). A client that links products to tags by scanning the label does not need the flash log.
- The model is also reported by the tag itself in POLL and shown as `model` in `GET /api/tags`.

## Layout from file (requirements G7, T17)
- The layouts come from a file made in the design tool, not fixed in the code. The AP checks the JSON file and turns it into a binary format; the tag downloads that in blocks when the AP reports a new version ([protokoll.md](protokoll.md) part 1, "Layout transfer", and part 3).
- `layout.c` interprets a list of elements (text field, filled rectangle, barcode, fixed text from the file) with position, fonts, colour, alignment and line count. "Price" and "Text" are such lists in flash; a downloaded file holds up to 16 more in the 14-byte wire format, drawn straight from XRAM with `layout_apply_bin()`.
- The download goes into one 768-byte XRAM buffer (`layout_file[]`), is checked with CRC-32 and a structure check (`layout_file_install()`), and is kept until the AP offers a new version. A copy is kept in flash and installed again at start-up, so a restart does not need a new download.
- "Price" and "Text" stay built in as fallback layouts; content for a file layout the tag does not have is shown with "Text".

## Fonts
- The fonts are made with a Python script (Pillow) from a font with an open licence that allows embedding (Inter, OFL). The source font's licence file is in `tag/fonts/`.
- Only the characters needed are generated: Latin-1 for small and medium fonts, only digits and ` ,-.:` for the mid and large price fonts. That keeps the firmware well within 64 KB of flash.

## Memory and coding style on the 8051
- Little internal RAM. Put buffers in `__xdata`, keep the stack small, avoid recursion and `printf` with floating point.
- Use `uint8_t` where you can; 16- and 32-bit arithmetic is expensive on the 8051.
- One fixed SDCC version and the same flags throughout.
