# Firmware update over radio for the tags (T15)

Status: implemented and on. Tested on the PC (host tests for the tag, the bootloader and the AP, and the end-to-end simulation) and on the jig tag 000002079350341C (1.54"), where four updates in a row installed and were confirmed (see "Tried on the jig"). Rollback is not tried on a tag yet. Radio security (T16) is not done, so this was built and turned on without it, by decision (see "Security").

The approach is our own; the SPI flash pins and the idea of staging an image in the tag's SPI flash before copying it into the chip come from OpenEPaperLink (atc1441 and others) and Dmitry Grinberg's ZBS243 work, see Credits in the main README.

## How it works

```
 esl-client tag-ota  ->  AP (tagfw partition)  ~~radio~~>  tag: SPI flash slot  ->  restart  ->  bootloader copies it in
```

1. A release has `openesl-tag-<model>-<version>-ota.bin` per model (`tag/build/fw/openesl-ota.bin` from `build.py fw`): a 16-byte header (magic `OSFW`, format, model, version, size, CRC-32; `ESL_FWF_*` in `shared/proto.h`) and the application, padded to whole 1 KB pages.
2. `esl-client tag-ota FILE`, or the Firmware card on the web page's Dashboard tab (over the AP's USB port or the bridge), uploads it with `PUT /api/tag-firmware` (16 KB parts). The AP keeps one image per model in its `tagfw` partition and checks the CRC before it offers it.
3. A tag polls with `fw_ver` and POLL flag bit3 ("I take firmware over the air"). When nothing else waits for it (settings, the screen image; layout file and content for firmware 0.4.7 and older) and it runs another version, the AP answers with the first FW block instead of IDLE. The tag asks for the rest with FW_REQ, at most 128 blocks (11 KB) per wake-up, and writes them to the SPI flash. A request that gets no answer even after its one retry is sent again, up to 8 times per wake-up (`CFG_FW_MAX_MISS`), before the tag leaves the rest for the next poll. A 40 KB image takes four wake-ups or a few more; the tag keeps showing its screen and polling as usual in between. The messages are in [protokoll.md](protokoll.md), "Firmware transfer".
4. With the whole image in the SPI flash, the tag reads it back and checks the CRC-32, copies its own application to the rollback slot, marks "install" and restarts (`ota.c`).
5. The bootloader in page 0 (`tag/boot/`) checks the image again, copies it page by page into 0x0400–0xF3FF and jumps to it. The new firmware shows the image in its slot again (T12) and polls. Coming from 0.4.7 or older there is no image yet, so it shows its start screen until the AP sends one, at its first poll.
6. The first answered POLL confirms the new image. If it is started 9 times without that (resets, not wake-ups from sleep), the bootloader copies the old application back and marks the update as rolled back; the old firmware then reports POLL flag bit4, and the AP stops offering that version.

The image can go to one tag instead of all of its model: `PUT /api/tag-firmware?...&to=<MAC>` uploads it for that tag alone, and `POST /api/tag-firmware/<model>?to=<MAC>` (or `?to=all`) gives the image the AP already has to one more tag (or to all). The list (at most 16 tags) is kept in NVS (`tagfw/to<model>`), so a restart does not turn it into an update for all; an image from before the list is for all. The dashboard's arrow next to a tag does this for that tag, and the Firmware card's "to all" button for the model. The AP notes how far each tag has fetched the image (RAM only), and `GET /api/tags` has it as `fw_update` (`received` of `size`), `GET /api/tag-firmware` as `progress` for the model; the dashboard shows both as bars and reads every 5 s while a tag is updating.

`esl-client tag-ota` without a file shows each image and how many tags run it, still need it, rejected it, or cannot take updates (firmware from before this, or no SPI flash).

## What decides the design

| Fact | Value | Source |
|------|-------|--------|
| ZBS243 main flash | 64 KB, 1 KB pages; 0xF400 free (the layout file in 0.4.7 and older), 0xF800 record store, 0xFC00 up stock MAC/flags | `tag/tools/build.py`, kildelogg.md |
| Firmware (SDCC 4.5.0, with OTA and images) | application 39,654 bytes (2.9") and 40,079 (1.54"); bootloader 994 bytes; OTA file 39,952 bytes (2.9") and 40,976 (1.54") | `build.py fw`, 2026-10-20 |
| Interrupts | none used (`IEN0 = 0`), so the bootloader needs no vector forwarding | `hal_zbs243.c` (`hal_init`) |
| Writing the main flash from the firmware | works: erase + write of a whole 1 KB page | kildelogg.md 2026-10-14/15, `flash_write_page` |
| SPI flash on the board | **Winbond W25Q80, 1 MB** on P1.1 (nCS), P0.0 SCK, P0.1 MOSI, P0.2 MISO. 4 KB erase 25 ms, 16-byte write 1 ms | `eepromprobe.c` on 000002079350341C (1.54"), 2026-10-15; see "Probe" below |
| Tag model and firmware version in POLL | yes (T18), so the AP knows which image a tag needs | `shared/proto.h` |

Two copies of the firmware do not fit in the chip (2 × 40 KB > 61 KB), so the new image is kept in the SPI flash while it arrives.

## Memory

| Where | What |
|-------|------|
| Chip 0x0000–0x03FF | bootloader (`tag/boot/boot_core.c` decisions, `boot_zbs243.c` hardware), never written by an update |
| Chip 0x0400–0xF3FF | the application (`--code-loc 0x400`), at most 61,440 bytes |
| SPI 0x80000 | slot NEW: header at 0x80000, image from 0x80100 |
| SPI 0x90000 | slot OLD: the application that ran before the update, same layout |
| SPI 0xA0000 | update state: one byte per flag, 0xFF = not set, programmed to 0x00 to set (install, copied, 8 tries, confirmed, rolled back), so setting a flag never needs an erase |
| SPI 0xB0000 | the screen image drawn by the AP (protokoll.md "Image transfer", `tag/src/image.h`): 16-byte header at 0xB0000, written last, the image from 0xB0100 |

The slots are well clear of the data the stock firmware left in sectors 0 and 1. The layout is in `tag/src/otaslot.h`.

## Bootloader

It runs only after a reset: deep sleep resumes where the firmware stopped, so a wake-up from sleep never passes through it. It has its own bit-banged SPI (whole-byte writes to P1, see kildelogg), the flash page write and a CRC-32 in assembler, and must fit 1 KB (`build.py` fails the build otherwise).

| State at start | What it does |
|----------------|--------------|
| no install, or already rolled back | jumps to the application |
| install, not copied | checks slot NEW (header and CRC); copies it and marks "copied", or marks "rolled back" if it is bad (nothing was copied, the old application is still there) |
| copied, not confirmed | marks one more try; after 8, copies slot OLD back (if its CRC is right) and marks "rolled back" |
| copied and confirmed | jumps to the application |

A power cut while copying leaves "copied" unset, so the next start copies again from the SPI flash. A power cut while the tag writes slot OLD leaves "install" unset, so nothing happens and the download is fetched again.

A firmware that runs, keeps polling and never reaches the AP is not rolled back: rollback counts restarts, and a firmware that loops without hanging is not restarted by the watchdog. A firmware that hangs is (the watchdog resets it).

## Checks on the tag

- An image for another model, the version it runs, version 0, or a size that is not whole 1 KB pages or larger than 60 KB is not taken.
- A wrong CRC-32 after the download, a failed SPI flash write, or a version that was rolled back: the version is remembered and reported with POLL flag bit4, and not taken again until the AP offers another one.
- After an install the new firmware compares the version in the image header with its own. If they differ (an image built with the wrong VERSION file), it counts as failed too, so the AP does not offer it again and again. The image still runs.
- A tag whose SPI flash does not answer at start-up does not set POLL flag bit3, so the AP never offers it firmware.

## Moving existing tags over

Tags flashed with firmware before this have the application at 0x0000 and no bootloader. They need one more pass with the flasher (`openesl-tag-<model>-<version>.bin`, which now holds the bootloader and the application); after that, updates go over the radio.

Tag firmware 0.4.7 and older draws content from CONTENT and LAYOUT and does not take the AP's images, so every tag needs newer firmware once. A tag with the bootloader gets it this way: the FW and FW_REQ messages and the image header are the same, and the AP still answers the 18-byte POLL of the old firmware. The AP needs a USB flash once too, for the `tagfw` partition (an AP updated with `POST /api/ota` keeps its old partition table and answers 404 on `/api/tag-firmware`).

## Security

Without signed or authenticated images, anyone in radio range who speaks the protocol can install any code on the tags. The same person can already send any content, which is what T16 (MIC) is for; T16 is on hold. The study recommended keeping OTA off until T16 or a separate check of the image is in place; Mathias decided on 2026-10-15 to build it and have it on now. When T16 comes, the FW block and the image header are the places to add a MAC.

## Energy

A block exchange is a few ms of radio at about 10 mA. A whole 40 KB image is about 450 exchanges, in the order of 0.01–0.05 mAh, small next to a CR2450. Erasing and writing the SPI flash adds a few mA for about 1.5 s in total. Not measured yet.

## Probe: is there a SPI flash? (yes)

`tag/src/hal/zbs243/eepromprobe.c` answered the open question. It reads the JEDEC id, status and SFDP on P1.1, reads a few bytes, and erases and writes 16 bytes in the **last** 4 KB sector only (the rest of the chip is not touched). The output goes to flash page 0x3D (0xF400), as with `battprobe`.

```sh
cd tag
HWTEST_MAIN=src/hal/zbs243/eepromprobe.c python3 tools/build.py hwtest   # build/hwtest/hwtest.bin
# flash it with the flasher, let the tag run a few seconds, then read 0xF400-0xF7FF back
# and reflash the normal firmware
```

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

The 2.9" is not probed yet; if its chip is missing or elsewhere, the 2.9" firmware simply never reports bit3. The chip must be put in deep power-down (0xB9) when it is not used, or it draws current while the tag sleeps; `eeprom_sleep()` in `hal_init` does that at start-up (PR #54), and `hal_xflash_close()` after every use.

## Tried on the jig (2026-10-15)

Setup: the ESP32-C6 AP flashed over USB with this branch (the new partition table adds `tagfw` at 0x390000; NVS with the tags and layout files was kept), the API over USB (`esl-client --serial /dev/ttyACM0`), the 1.54" jig tag 000002079350341C flashed once with `openesl.bin` (bootloader + application) through the ZBS_Flasher, poll interval 60 s, RSSI -51 to -60 dBm at the AP. The tag came back with fw 200 (0.2.0), fetched its content and showed it.

The test images were the same source with a different `VERSION` (0.2.90 = fw 290), uploaded with `esl-client tag-ota tag/build/fw154/openesl-ota.bin`:

| Update | Upload to new version reported | FW requests per wake-up | Result |
|--------|--------------------------------|-------------------------|--------|
| 0.2.0 → 0.2.90 | about 5 min | not logged | installed, confirmed, price shown again |
| 0.2.90 → 0.2.0 | over 9 min (see below) | not logged | installed, confirmed, price shown again |
| 0.2.0 → 0.2.90 | 394 s | 140, 100, 113, 109, 137, 46 | installed, confirmed, price shown again |
| 0.2.90 → 0.2.0 | 490 s | 137, 134, 134, 2, 134, 46, 9, 44 | installed, confirmed, price shown again |

`esl-client tag-ota` showed "1 pending" during each download and "1 updated" after it. `draw_failed` stayed false and the content id stayed the same, so the restarted firmware drew the stored content (T12). Rebuilding 0.2.0 gave a byte-identical OTA file.

What was seen:

- An image takes 6–8 wake-ups, not five: a wake-up ends early when a FW_REQ gets no answer twice (`exchange_retry`), and the download goes on at the next poll. The AP counted 70 failed transmits of about 1,300 replies. This only makes an update slower. Since then the tag sends an unanswered FW_REQ again, up to 8 times per wake-up (`CFG_FW_MAX_MISS`), so a lost answer no longer ends the wake-up. Tried the same evening (below): four or five wake-ups per update.
- The second update took over 9 minutes, and the tag reported the new version right after the AP restarted (opening its serial log reset it). Two more updates with the log open the whole time did not stall, so this was most likely a run of short wake-ups like the ones above, not a stuck state. It was not caught in the log, so this is not shown.
- The AP log names every reply that is not CONTENT or LAYOUT "IDLE", also FW blocks and CONFIG, and logs each FW_REQ as a poll with `id=00000000 model=0`. This was misleading when following an update in the log. Fixed since: the poll line names FW and CONFIG replies and shows the tag's `fw=`, and FW_REQ is logged at debug level only (the stats line counts the blocks).

### From the web page (2026-10-15 evening)

The Firmware card on the Dashboard tab, in headless Chromium through the bridge (`esl_client.py --serial /dev/ttyACM0 serve`), with a second 1.54" tag, 0000020663463415. That tag still had the OEPL firmware: it was backed up whole first (`ZBS_Flasher`), then flashed with `openesl.bin`. It only started after it was put into the jig again, since the jig powered it only now and then; on a battery it ran steadily.

| Step | Result |
|------|--------|
| 1.54" 0.2.90 from the page's list | upload about 1 s; the tag reported 290 241 s after its first poll, 5 wake-ups (4 with download), table "1 updated" |
| 1.54" 0.2.0 from "A file from this computer" | 312 s from the upload, first poll after 42 s, 6 wake-ups; no round ended early |
| "Remove" | `GET /api/tag-firmware` shows no image for 1.54 |
| AP 0.2.91 from the list | upload 9 s; the page said "The AP is back and runs 0.2.91" 3 s after the restart (`ota_1`) |
| AP 0.2.0 from a file | 16.5 s; back on `ota_0`; the tag image on the AP survived both restarts |

With `CFG_FW_MAX_MISS` an update needed 4–5 download rounds instead of 6–8. This was also the first try of the AP's own OTA (`POST /api/ota`) on hardware; the rollback of a bad AP image is still not tried.

Seen on the way: the AP keeps the tags' run-time state (last poll, OTA support) in RAM only, so right after an AP restart the dashboard shows a tag as never seen until it polls again.

## Still to do

1. Try a rollback on the jig tag: an image that never polls must be copied back after 9 starts and reported with POLL flag bit4.
2. Probe a 2.9".
3. Measure the energy of a whole update.
4. Signed images, with T16.
