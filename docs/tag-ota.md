# Firmware update over radio for the tags (T15)

Status: study, nothing implemented. This page is what we know, what we do not know, and a proposed design. The approach is our own; the SPI flash pins and the idea of staging an image in the tag's SPI flash before copying it into the chip come from OpenEPaperLink (atc1441 and others) and Dmitry Grinberg's ZBS243 work, see Credits in the main README.

## What decides the design

| Fact | Value | Source |
|------|-------|--------|
| ZBS243 main flash | 64 KB, 1 KB pages; 0xF400 layout file, 0xF800 record store, 0xFC00 up stock MAC/flags | `tag/tools/build.py`, kildelogg.md |
| Firmware today (v0.2.0, SDCC 4.5.0) | 2.9": 48 927 bytes, 1.54": 49 306 bytes (31.0 KB code + 17.7 KB constants, of which 17 KB fonts) | `make fw`, `MODEL=154 make fw`, `build/fw/openesl.map` |
| Room for code | 0x0000–0xF3FF = 62 464 bytes, so 13 KB free | `CODE_SIZE` |
| Interrupts | none used (`IEN0 = 0`) | `hal_zbs243.c` (`hal_init`) |
| Writing the main flash from the firmware | works: erase + write of a whole 1 KB page | kildelogg.md 2026-10-14/15, `flash_write_page` |
| Radio code | about 1.3 KB | `build/fw/radio_zbs243.sym` |
| SPI flash on the board | **Winbond W25Q80, 1 MB** on P1.1 (nCS), P0.0 SCK, P0.1 MOSI, P0.2 MISO. JEDEC id EF 40 14, SFDP answers, 4 KB erase took 25 ms and a 16-byte write 1 ms, and read back right. The stock firmware left data in sectors 0 and 1 | `eepromprobe.c` on 000002079350341C (1.54"), 2026-10-15; see "Probe" below |
| Tag model in POLL | yes (byte 11, T18), so the AP knows which image a tag needs | `shared/proto.h` |
| Firmware version in POLL | yes (major·10000 + minor·100 + patch) | `shared/proto.h` |

The main point: **two copies of the firmware do not fit in the chip** (2 × 49 KB > 61 KB). The new image has to be kept somewhere else while it arrives, or written straight over the running one by code that is not itself overwritten.

## Two ways to do it

### A. Stage the image in the SPI flash (recommended: the chip is there)

1. The firmware downloads the new image into the SPI flash in the background, a burst of blocks per wake-up, while it keeps showing its content and polling as usual.
2. When the whole image is there and its CRC-32 matches, the tag writes an "install" record and restarts.
3. A small **bootloader** in page 0 (0x0000–0x03FF), which an update never overwrites, sees the record, checks the CRC again, copies the image page by page into 0x0400–0xF3FF, checks it, clears the record and jumps to the application at 0x0400.
4. Power loss during the copy is harmless: the image and the record are still in the SPI flash, so the next start copies again.

Rollback: the 1 MB SPI flash has room for the old image in a second slot (two 64 KB slots, kept clear of the stock data in sectors 0 and 1, for example at 0x80000 and 0x90000). The new firmware marks itself good after its first answered POLL; if the bootloader has started it N times without that mark, it copies the old image back. That protects against a firmware that starts but can no longer talk to the AP, which would otherwise need the jig for every tag.

Cost: the application moves from 0x0000 to 0x0400 and loses 1 KB (still 12 KB free). Since no interrupts are used, the bootloader needs no vector table forwarding; if interrupts are added later, page 0 must forward the vectors to 0x0400. The bootloader needs bit-banged SPI, the flash page write and a CRC-32, which should fit in 1 KB of 8051 code; if not, it takes two pages.

### B. No SPI flash: a radio bootloader

If the chip is missing or unusable, the bootloader itself must receive the image: it holds its own small radio driver and block request loop (about 3–4 KB) and writes each 1 KB page straight into the main flash. The firmware only has to accept "go to update mode" and restart. Power loss leaves the tag in update mode, and it asks again until the image is complete.

Downsides: the tag shows nothing useful during an update, there is no rollback, the bootloader is 3–4× bigger (so less room for fonts later), and a bug in its radio code can only be fixed with the jig.

## Radio protocol (both ways)

The layout file already arrives in blocks (`LAYOUT` / `LAYOUT_REQ`, 94 bytes of data per packet), driven by the tag. Firmware can use the same pattern:

- The AP keeps one image per tag model and a target version (for all tags, or per tag).
- A tag whose POLL reports another version and the right model gets `FW_OFFER` (version, size, CRC-32) after content, layout and CONFIG are done, so prices are never held back by an update.
- The tag asks for blocks with `FW_REQ` (offset, max length) and gets `FW` blocks; about 525 blocks for 49 KB. It takes a burst per wake-up (say 64 blocks, 6 KB) and keeps the offset, so one tag never blocks the AP for long and an interrupted download resumes.
- The image file has a header made by `build.py`: magic, model, version, size, CRC-32. The tag refuses another model, and a version it already runs.
- Rough energy: a block exchange is a few ms of radio at about 10 mA, so the whole update is in the order of 0.01–0.05 mAh, small next to a CR2450. To be measured.

On the AP side this is the same shape as the layout files: `POST /api/tag-firmware` (image per model), a target version in `PUT /api/config` or per tag, progress in `GET /api/tags`, and `esl-client tag-ota`. Releases already build one image per model.

## Security

Without signed or authenticated images, anyone in radio range who speaks the protocol could install any code on the tags. Today the same person can already send any content, which is why T16 (MIC) exists; T16 is on hold. For OTA the stakes are higher (a bricked or hijacked tag), so OTA should not be enabled by default before T16 or a separate check of the image (for example a MAC over the image with a key set when the tag is flashed in the jig) is in place.

## Moving existing tags over

Tags flashed today have the application at 0x0000 and no bootloader. They need one more pass in the jig to get the bootloader and the moved application; after that, updates go over the radio.

## Probe: is there a SPI flash? (yes)

`tag/src/hal/zbs243/eepromprobe.c` answers the open question. It reads the JEDEC id, status and SFDP on P1.1, reads a few bytes, and erases and writes 16 bytes in the **last** 4 KB sector only (the rest of the chip is not touched). The output goes to flash page 0x3D (0xF400), as with `battprobe`.

```sh
cd tag
HWTEST_MAIN=src/hal/zbs243/eepromprobe.c python3 tools/build.py hwtest   # build/hwtest/hwtest.bin
# flash it with the flasher, let the tag run a few seconds, then read 0xF400-0xF7FF back
# and reflash the normal firmware
```

What the answer means:

- `9F jedec` gives a maker byte (for example C2 Macronix, EF Winbond, 85 Puya) and a capacity byte (0x14 = 1 MB), and `after write` reads back the pattern: way A works.
- `9F jedec` reads `FF FF FF` or `00 00 00`: no chip, or not on these pins. Way B, or look for the chip on the board first.

Result on the jig tag 000002079350341C (1.54"), 2026-10-15, from the log at 0xF400:

```
AB id       13 13
9F jedec    EF 40 14
90 mfr/dev  EF 13
05 status1  00
35 status2  00
5A sfdp     53 46 44 50 05 01 00 FF 00 05 01 10 80 00 00 FF
03 @000000  A5 5A BA AB 01 00 00 00 01 01 01 0C 00 92 09 28
03 @001000  00 33 00 03 FF FF FF FF FF FF FF FF FF FF FF FF
sector 0FF000: before FF.., status WEL 02, erase ms 0019, after erase FF.., program ms 0001
after write A5 B4 87 96 E1 F0 C3 D2 2D 3C 0F 1E 69 78 4B 5A
```

So way A works on the 1.54". The 2.9" is not probed yet. The chip must be put in deep power-down (0xB9) at start-up, or it draws current while the tag sleeps (a sleep current of 33 µA was measured before, and the chip is the suspected cause); `eeprom_sleep()` in `hal_init` does that (PR #54).

## Next steps

1. Probe a 2.9" too, when one is in the jig.
2. Decide whether OTA waits for T16.
3. Then, as separate PRs: the bootloader and the moved application (tested in the jig first), the image header in `build.py` and the release, the radio messages (host-tested with the simulator like the layout transfer), the AP API and `esl-client tag-ota`.
