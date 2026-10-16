# Study: flashing stock tags over the radio

Goal: put OpenESL on a tag that still runs the stock SoluM firmware, without pogo pins, by using the stock firmware's own update path. Started 2026-10-16. This page holds what is known, what is open, and the plan. Facts that the code ends up using also go in [kildelogg.md](kildelogg.md).

## What is known

- The stock ZBS243 firmware has a separate small "OTA image" next to the main firmware. It only receives radio frames (802.15.4 framing) and has flash erase, write and verify routines. It does not use the AES block. Source: Dmitry Grinberg, "Reverse Engineering an Unknown Microcontroller" (dmitry.gr), who studied it to find the radio registers but did not use it for updates.
- The protocol between a SoluM AP and a stock tag (joining, binding to a store network, how an update starts, any CRC or signature on the image, any encryption in the main firmware) is not published anywhere we have found. OpenEPaperLink and other projects flash ZBS243 tags with pogo pins only.
- We have backups of stock flash from our own tags (`backup/<MAC>.bin` and `.info.bin`, written by `tools/flash-tui`).

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
