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

## What a new stock tag sends (sniffer capture, 2026-10-16)

Tag: new, 1.54", factory firmware, plastic battery tab pulled at t = 174 s. Sniffer on an ESP32-C6 (build from `ap/sdkconfig.sniffer`, 2 s on each channel in the first capture), tag next to the C6, no AP in range. 9 frames in 10 minutes, all of this form (11 bytes; the driver adds RSSI and LQI):

```
41 88 <seq> 34 12 FF FF AF 10 E0 85
FCF 0x8841: data frame, PAN ID compression, short destination and source addresses
dest PAN 0x1234, dest 0xFFFF (broadcast), source 0x10AF, payload E0 85 (2 bytes)
```

- The first frame came about 8 s after the battery went in. The tag sends one frame on every channel 11-26 in a sweep (about 20 ms between frames, sequence number +1 per channel, +16 per sweep, no gaps). Seen sweeps at 182, 212, 246, 275, 477, 501, 534 and 549 s: about 30 s apart, with one pause of 202 s that looks like a back-off.
- Meaning (a reading, not confirmed): a search for an AP. The tag probably listens for a short time after each frame, so a reply has to come on the same channel right after it.
- A used 2.9" tag (label 01C287B82818, probably MAC 000001C287B82818) did the same after fresh batteries, with payload `E0 87` instead of `E0 85`, and no sign of a fixed channel: 17 frames in 290 s, sweeps 1.4-120 s apart, no gaps in the sequence numbers. Source address and PAN were the same (0x10AF, 0x1234). Second capture: 22 frames in 600 s, all from these two tags, no AP, no other address.
- Reading (two tags only, not confirmed): 0x10AF is a shared "not paired" source address, `E0` is the message type "looking for an AP", and the second payload byte is the model or size (0x85 = 1.54", 0x87 = 2.9").
- Not yet known: what the AP's reply looks like. A real factory AP exists (no software; Ethernet port); it can be powered on next to the sniffer to see its replies.

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
