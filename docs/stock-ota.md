# Study: flashing stock tags over the radio

Goal: put OpenESL on a tag that still runs the stock SoluM firmware, without pogo pins, by using the stock firmware's own update path. Started 2026-10-16. This page holds what is known, what is open, and the plan. Facts that the code ends up using also go in [kildelogg.md](kildelogg.md).

## What is known

- The stock ZBS243 firmware has a separate small "OTA image" next to the main firmware. It only receives radio frames (802.15.4 framing) and has flash erase, write and verify routines. It does not use the AES block. Source: Dmitry Grinberg, "Reverse Engineering an Unknown Microcontroller" (dmitry.gr), who studied it to find the radio registers but did not use it for updates.
- The protocol between a SoluM AP and a stock tag (joining, binding to a store network, how an update starts, any CRC or signature on the image, any encryption in the main firmware) is not published anywhere we have found. OpenEPaperLink and other projects flash ZBS243 tags with pogo pins only.
- We have backups of stock flash from our own tags (`backup/<MAC>.bin` and `.info.bin`, written by `tools/flash-tui`).

## Findings from a stock backup (2026-10-16, tag 000002079350341C)

Read-only study of the flash backup. Addresses are code addresses in the 64 KB main flash.

- The stock flash holds two programs. The main firmware is at 0x0000-0xDA7F (reset vector `LJMP 10B1`). A second, separate program sits at 0xE800-0xF8FF with its own vector table (reset at 0xE800 sets SP and jumps to 0xF2D5). Pages 0xDC00-0xE7FF are empty.
- The second program is a flash loader, not a radio program. It has the page write routine (0xF382-0xF3AC: SETTINGS |= 0x38, FPGNO = page, FWRTHREE = 3, 1 KB from xdata 0xE800, wait for TCON2 bit 3), a UART interrupt handler (0xF3F8: SCON, SBUF into ring buffers at xdata 0xE400), and SPI flash code that uses P1.1 as nCS (0xEEC0-0xEF74), the same pin and kind of chip as our W25Q80.
- Reading: the stock OTA most likely stores the new image in the external SPI flash, and this loader copies it into internal flash page by page. That is the same split as our own OTA (slots in SPI flash, bootloader copies).
- The main firmware also drives the SPI flash itself (code 0x1E00-0x2FA0): deep power-down 0xB9, release 0xAB, write enable 0x06, page program 0x02, block erase 0x52, with nCS on P1.1. So the external flash is used by the stock firmware, consistent with it staging an image there.
- Nothing in the main firmware jumps to 0xE800 or 0xE803 directly or by an obvious computed jump; the constants that look like 0xE803 are 1000 (0x03E8). The startup code at 0x10B1 is plain compiler startup. The entry is probably decided before the main firmware runs (a flag checked at reset) or by a path that is not a plain jump. Static tracing is slow because the compiler shares code tails between functions.
- Not found yet: what enters the loader (no `LJMP`/`LCALL` to 0xE800 or 0xE803 in the main firmware, so it is a jump through a pointer, a reset-time check, or the chip's boot ROM), and where the main firmware receives the image over the radio and writes it to the SPI flash. Those two answer open questions 2 and 3.
- Nothing seen yet about a CRC, signature or key on the image. Question 4 is still open.

## Open questions, in order

1. Does a stock tag without its AP transmit anything (scan, join request), and on which channel and PAN? The sniffer answers this: AP firmware in radio mode `Sniffer` with `ESL_SNIFF_HOP_MS` above 0 steps through channels 11-26.
2. Where is the OTA image in the stock flash, and what starts it (a frame from the AP, a flag in flash, a timer)?
3. What frames does it accept: addressing, packet format, block size, acknowledgements.
4. Is the image checked (CRC, length, version, signature, key)? If it needs a key we cannot get, the study stops here.
5. Which flash range does it write? Our image is the bootloader at page 0 plus the application, and must keep the MAC at 0xFC06 (stock tags have no MAC in the info page; our firmware reads it from there today).

## Plan

1. Sniff a stock tag with the hopping sniffer (needs a tag that still runs stock firmware).
2. Disassemble one stock backup (`tag/tools/dis51.py`), find the OTA image and answer 2-5.
3. If nothing blocks: a "stock" radio mode on the C6 that speaks enough of the stock protocol to hand over our image, and a firmware fallback that reads the MAC from 0xFC06 when the info page is blank.
