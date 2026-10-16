# Study: flashing stock tags over the radio

Goal: put OpenESL on a tag that still runs the stock SoluM firmware, without pogo pins, by using the stock firmware's own **firmware** update path. Started 2026-10-16. This page holds what is known, what is open, and the plan. Facts that the code ends up using also go in [kildelogg.md](kildelogg.md). Raw on-air logs stay outside git (`openESL-captures/`, next to this repo).

## Status (read this first)

Handshake that works, on channel 25, PAN 0x1234, our AP address `00…01`:

| Tag sends | We answer | Result |
|---|---|---|
| `E0` search (`41 88`, source `10AF`) | `E1` + zeros, FCF `41 C8` | Tag binds and sends `D7` |
| `D7` request (`61 CC`) | `80 02` then `D8` | Tag sends `AA` |
| `AA` request | `F5 00` | **Image path**, not firmware: tag sends `F4` and the screen goes white |
| `AA` request | `DE` | Tag goes back to searching |
| `F4` then our `DE` + `D0`/`D1`/`D2` push | — | Push discarded. Tag stays silent. |

`F5` / `F4` / `D0`–`D2` is the stock **picture** transfer. Empty `F9`/`FA`/`A7`/`AD` as `AA` answers will be rejected (reading of this dump, 2026-10-16): `F9`/`FA` need a chunk header and a pointer `AD` sets first; `A7` needs pairing flag `FDA0 == A5`; `AD` needs subtype `02` or `15`. Do not burn battery cycles on zero-padded guesses of those four. Target tag on air: `BB950702FF94BAC8` (`CONFIG_ESL_SNIFF_TARGET`).

Dispatcher first-byte cases (stock backup, 0x4D35): `80` (second byte 1 or 2), `A7` (rejected in state 4, needs `FDA0 == A5`), `AD` (subtype in `F831` must be `15` or `02`), `D0`–`D2` (only states 8/9, sender must be the stored AP), `D8`, `DE` (sets 8 or 9), `F5`, `F9`, `FA` (both gated by `7EBD`). `D0`–`D2` as answers to `D7`/`AA` were not accepted because those types belong to the later data-receive states.

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

## Reading of flash.bin (2026-10-16, second pass)

Dump from the user, kept next to the repo (not in git): `openESL-captures/stock-dump/flash.bin` (64 KB) and `info.bin` (1 KB). Reset `LJMP 10B1`. MAC at `0xFC00` is `ADD2C8BA94FF 020793500102` (not the tag on air). Info page is calibration, no MAC.

State variable is XRAM `FD9F`. The table at `0x4014` is indexed by `state - 2`:

| `FD9F` | Sends / does |
|---|---|
| 4 | `D7` request (`0x8A89`) |
| 6 | `AA` request (`0x88E0`) |
| 7 | `F4` request (`0x8FBC`, hardcoded `F4 85`, echoes `F66F`) |
| 8 or 9 | data-receive (accepts `D0`/`D1`/`D2`) |
| `0x0B` | set by `AD` when subtype is `15` or `02` |
| `0x0D` | sets `FB64 = 1` (`0x4063`) |

`F5` at `0x5255` is the fork: if `FB64 != 1` it writes `F5` into `F66F`, sets state 7, and the screen goes white (the path we hit with `F5 00`). If `FB64 == 1` and a length byte is 0 it calls `2ECE` (state 8) instead — same receive state as `DE`, but after the `AD` path has marked “not a picture”. That is the best firmware-shaped reading so far.

Empty answers the current probe would send, and why they fail:

- `F9`/`FA` (`0x9168` / `0x91A3`) call `7EBD`. That routine needs a pointer in `F833` (written by `AD` at `0x285F`) and a chunk index / count that ends with `F491 == F836`. On failure it returns 0 and both handlers jump to `0x2AE2` (back to waiting). A type byte plus zeros never passes.
- `A7` (`0x4DCF`) is allowed in the `AA` state (it only rejects state 4) but needs `FDA0 == 0xA5`. That flag is written at `0x31AD` after a check we have not completed. The body at `0x41FA` looks like a size/bitmask command (compares a 16-bit value to `0x0259`), not a firmware blob.
- `AD` (`0x2823`) stores the packet pointer, then treats `F831` as a subtype: `15` or `02` can set state `0x0B` and later `FB64`. Any other subtype falls through and usually ends at `0x2AE2`. `AD 00` is the other subtype.

`DE` without `FDA0 == A5` still sets state 8 (`0x4ED0`). `D0`–`D2` are then accepted (`0x4D74`: state 8 or 9, then sender check `0x9D04`). Split: `D1` → `0x4933`, `D2` → `0x4A79`, `D0` → `0x7CA1`. Probe11’s push after `F4` was therefore the right types in the wrong session: the tag had gone through state 7 (picture) and likely stopped listening before the 150 ms push, and `F833` was never set up as `AD` would have done.

The program at `0xE800` is a UART factory loader, not radio OTA. Reset goes `E800 → E806 → F2D5` and sits in `SJMP F2EF` waiting for the UART vector at `E803`. The main firmware can write its own flash: `0xA16E` / `0xA1E7` do `C7 = 0x38` (SETTINGS), `DF = 3` (FWRTHREE), page in `D8`, wait `CF.3`. Radio firmware replacement, if it exists, stays in the main program (receive → SPI and/or this page write). Nothing in the main firmware jumps to `0xE800`. No `LJMP 0000`. No signature check found on the radio path yet (question 4 still open).

Next, in this order: (1) finish `AD` so `F831` is decoded and we can send subtype `02` or `15`; (2) find what sets `FDA0 = A5` (`0x31AD`); (3) list callers of `0xA16E`. Do not restart a tag on the C6 until the probe sends one of those shaped frames — the C6 still has the probe11 image path unless it was reflashed, and zero-padded `F9`/`FA`/`A7`/`AD` will only confirm this reading.

## What a new stock tag sends (sniffer capture, 2026-10-16)

Tag: new, 1.54", factory firmware, plastic battery tab pulled at t = 174 s. Sniffer on an ESP32-C6 (build from `ap/sdkconfig.sniffer`, 2 s on each channel in the first capture), tag next to the C6, no AP in range. 9 frames in 10 minutes, all of this form (11 bytes; the driver adds RSSI and LQI):

```
41 88 <seq> 34 12 FF FF AF 10 E0 85
FCF 0x8841: data frame, PAN ID compression, short destination and source addresses
dest PAN 0x1234, dest 0xFFFF (broadcast), source 0x10AF, payload E0 85 (2 bytes)
```

- The first frame came about 8 s after the battery went in. The tag sends one frame on every channel 11-26 in a sweep (about 20 ms between frames, sequence number +1 per channel, +16 per sweep, no gaps). Seen sweeps at 182, 212, 246, 275, 477, 501, 534 and 549 s: about 30 s apart, with one pause of 202 s that looks like a back-off.
- Meaning (a reading, not confirmed): a search for an AP. The tag probably listens for a short time after each frame, so a reply has to come on the same channel right after it.
- A used 2.9" tag (label 01C287B82818, MAC probably 000001C287B83B1A, derived from the label as another thread worked out; not read from the tag) did the same after fresh batteries, with payload `E0 87` instead of `E0 85`, and no sign of a fixed channel: 17 frames in 290 s, sweeps 1.4-120 s apart, no gaps in the sequence numbers. Source address and PAN were the same (0x10AF, 0x1234). Second capture: 22 frames in 600 s, all from these two tags, no AP, no other address.
- Reading (two tags only, not confirmed): 0x10AF is a shared "not paired" source address, `E0` is the message type "looking for an AP", and the second payload byte is the model or size (0x85 = 1.54", 0x87 = 2.9").
- Fourth capture (factory AP, 2026-10-16): the SoluM AP (SLG-CP101) sent nothing on 802.15.4 for 15 minutes, with and without Ethernet. It has a fixed network setup from an earlier installation and keeps its radio off without that server. Dead end; the AP reply has to come from the tag firmware instead.
- Where the probe is built in the stock backup (addresses in the main firmware, no bytes copied): the routine at 0x8D10 fills a frame buffer in XRAM (length 0x0D, `41 88`, sequence, PAN, broadcast, `AF 10`, `E0`, model byte) and sends it. It is called from the channel sweep at about 0x9DF5-0x9E14 (a counter in XRAM FCBF runs from 11 up to 26). A second frame builder at 0x8D97 starts with first byte 0xC8 and broadcast, is called from a state at 0x98A3 and has a length that varies; it is the next candidate for the reply or join step.

- Receive side (reading of the stock backup, not confirmed on air; this replaces an earlier wrong reading where 0xC8 was taken as the first byte of the frame): the receive routine at about 0x6DD0-0x6E80 copies the frame into a buffer and masks the second FCF byte with 0xCC. Three forms are handled: 0x88 (short destination, short source), 0xC8 (short destination, long source) and 0xCC (long destination, long source, the source address is copied out as 8 bytes). While the tag is unpaired (a flag in bit-addressable RAM is set) only the 0xC8 form is taken further, so an AP answer to the search is most likely a data frame `41 C8 <seq> <PAN> <dest short> <source long, 8 bytes> E1 ...`. The first payload byte and the payload length go to a routine at 0x9E44, which knows the message types 0xE1 (length at least 26), 0xD0 (at least 33), 0xD1 (at least 37) and 0xD2 (at least 68) and copies the payload into separate XRAM buffers.
- The tag's own second frame (builder at 0x8D97) is the same shape: `41 C8`, sequence number, destination PAN and address FFFF FFFF, then the tag's 8-byte MAC, then a variable payload. It is probably sent after an answer and announces the tag by MAC.
- First answer from a stock tag (2026-10-16, 1.54" tag, C6 fixed on channel 25): after our guessed `E1` reply (variant 1: `41 C8 <seq> 34 12 AF 10`, a long source address 00..01, `E1`, 25 zero bytes) the tag changed behaviour and sent its own frame, three times per round with rising sequence numbers: `61 CC <seq> 34 12 <dest 00 00 00 00 00 00 00 01> <source = tag EUI-64> D7 85 1F 00 00 00 00 01`. So FCF 0xCC61: data, acknowledgement requested, PAN compression, long destination (the address we gave as AP) and long source. The payload starts with `D7`, then the model byte (0x85 = 1.54"), then 6 bytes. The tag's screen blinked. The receive route in the stock firmware therefore accepted a minimal `E1` frame with 25 zero bytes after the type.
- The tag asks for an acknowledgement, and in promiscuous mode the radio does not send one, so it retransmits and gives up. The next probe build is therefore not promiscuous: PAN 0x1234 and extended address 00..01 are set so the hardware acknowledges the `61 CC` frame, and each request is answered with a guessed data frame of type D0, D1, D2 or E1 (cycling) so the next step shows in the log.
- Static reading of the tag's request and the answer it expects (addresses in the main firmware): the state table at about 0x4014 picks an action per state; state 4 sends the `61 CC` request (builder 0x8A89 for the `D7` payload, sender 0x899D, FCF bytes written explicitly) and waits 50 ticks before sending again. The answer is stored with the sender's 8-byte address first, then length, sequence number and payload, and the dispatcher at 0x4D35 looks at the first payload byte: 0x80 (needs the second byte to be 1 or 2), 0xA7 (not in state 4, needs 0xA5 in a state variable), 0xAD, 0xD0-0xD2 (only in states 8 and 9, and the sender address must equal the stored AP address), 0xD8, 0xDE, 0xF5, 0xF9 and 0xFA. Which of these answers a `D7` request is not clear from the code alone; guessed answers with zero padding of D0, D1, D2 and E1 did not advance the tag, which fits that those types belong to later states.
- Request replies (2026-10-16, second run): answering the `D7` request with `80 02` and `D8` made the tag move on. It now sends an `AA` request instead (`61 CC <seq> ... AA 85 1F 00 36 FF FF FF FF 00 00 00 <counter> FF FF FF FF FF 00 00 00 00 00 01 00 00 <14|15> 10 10 00 00`, 54-byte payload, counter rising, repeated about every 30 s).
- Next experiment: the probe build answers a `D7` request with `80 02` and `D8`, and every `AA` request with a burst of three candidate types (F9, FA, AD, DE, F5, A7, 80 01, 80 02, D0, D1, D2, E1), rotating, and logs which burst went out. A change in the tag's next request, a new frame type or a screen change shows which type it reacts to.
- Experiment prepared: `ap/sdkconfig.probe` (CI artifact `openesl-ap-probe.bin`, not in releases) makes the sniffer answer every `E0` search frame at once, on the same channel, with a guessed `E1` frame in three payload lengths (26, 40 and 70 bytes of zeros), and logs which variant went to which channel. If the tag accepts any of them it should change behaviour, most likely by sending its own `41 C8` frame with its MAC.

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

- Third run (2026-10-16, probe8): after `80 02` + `D8` and a burst `DE`, `F5`, `A7` (sent 31 us, 2.3 ms and 5.2 ms after the request) the tag sent the `AA` request only once (length byte 0x41, last status byte 0x16, earlier runs 0x36 and 0x14/0x15), then went silent, and its screen did a full refresh back to the default content. Only the first frame arrived within the tag's short listening window, so `DE` is the likely accepted type. Next build sends one answer per AA request, starting with `DE`, then `F5`, `A7`, `AD`, `F9`, `FA`, `80 01`, `80 02`, `D0`, `D1`, `D2`, `E1`; the tag must be restarted (battery out and in) for each trial.

- Fourth run (2026-10-16, probe9): `DE` as answer to the AA request sent the tag back to searching; `F5 00` was accepted. The tag then sent a new request type `F4`: `61 CC <seq> ... F4 85 00 00 00 00 F5 00 00 00 00 00 00` (13 byte payload, F5 echoed) and its screen went white (it clears the display before new content). Without an answer to `F4` it timed out after a while and fell back to the unpaired default. The stock code builds `F4` in state 7 (state table entry 0x405E, builder 0x8FBC). Dispatcher cases by first byte: 80, A7, AD, D0-D2 (only in states 8/9), D8, DE (sets state 8 or 9, probably start of the data transfer), F5, F9, FA. The next build answers each `F4` request with one candidate (DE, D8, F9, FA, AD, A7, 80 01, 80 02, D0, D1, D2, F5 01), rotating, and logs which.

- Fifth run (2026-10-16, probe10): a second stock tag in the room disturbed the rotation, so the probe can now be locked to one tag (`CONFIG_ESL_SNIFF_TARGET`, the tag's address as on air). F4 answers `DE`, `D8`, `F9` and `FA` all gave the same result: the tag sends F4 once, the screen turns white, then silence. Reading of the stock code: `DE` leads to the data-receive state (8 or 9), where only `D0`, `D1` and `D2` are accepted, so the next idea was that the tag waits for the AP to push.

- Sixth run (2026-10-16, probe11 / probe11b, tag lock confirmed): same handshake, then F4 answered with `DE` and a push of `D1`, `D0`, `D2` (payload bytes 7–8 = `FF 01`) and a plain `D1`, 150 ms apart. The tag sent nothing back. No new frame type, no D1 echo, no further F4. The push was discarded. Two repeats, same tag (`BB950702FF94BAC8`). After F4 the log shows only the four pushes and then stats (`rx 4`). The `AA` counters rise between pairings (persistent state in the tag's flash); the `D7` tail varies; `F4` is always `F4 85 00 00 00 00 F5 00 00 00 00 00 00`. Course correction: `F5 00` → `F4` + white screen is the stock **image** path. Firmware replacement has to leave that road. The next build answers each `AA` with one of `F9`, `FA`, `A7`, `AD` (the dispatcher types not yet tried as a single AA answer) and ignores `F4`. A new request type, a return to `E0`, or a screen change other than white shows which type it reacts to. Battery out and in for each trial; leave the C6 running so the candidate counter advances.

- Payload notes from the same runs (1.54" tag, model byte `85`): `D7` is `D7 85 1F 00 00 TT 00 00` with tail `TT` rising across wakes (`00`, `01`, `03`, `07`). `AA` is 32 bytes after the type: `AA 85 1F 00 <status> … <counter> FF FF FF FF FF … <pair that rises> … 15|16 10 10 00 00`. The rising fields are counters, not a fixed join token. `F4` echoes the `F5` we sent.
