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
| `AA` request | `F9 01 01` (alone) | **Firmware gate.** Tag leaves `AA`, single request, then silent; screen redraws default content |
| `F4` then our `DE` + `D0`/`D1`/`D2` push | — | Push discarded. Tag stays silent. |

`F5` / `F4` / `D0`–`D2` is the stock **picture** transfer. Empty `F9`/`FA`/`A7`/`AD` as `AA` answers are rejected. `AD` decides its subtype from a 16-bit **big-endian size at payload bytes 3–4**, compared with XRAM `F560` (`0x4481`): smaller → `01` (return); larger → `15` (state `0x0B`); equal → `02` or `00`. Isolated `AD` + size `FFFF` (probe12, CI `38892416632`): handshake still `E0`/`D7`/`AA`, then `AA` every ~230 ms, our `AD` in 13 µs, **no new request type, no `F4`, no white screen**. Continuous `AA` means the tag was still sending from state 6. That does **not** prove subtype 15 was rejected: `15`/`02` with a later checksum fail go to state `0x0B` then `41D4`/`47BD` (state 3). Staying on `AA` means that path did not stick, or the frame never passed the earlier checks. Chained `AD FFFF` + `F9`/`FA` (probe13, CI `38894030644`, from startup): handshake `E0` (20 µs) → `D7` (13 µs) → `AA`, then only `AA` every ~200 ms, our chain in 13 µs. **No new request type, no `F4`, no return to `E0`.** Same next-request as isolated `AD` (probe12). Log: `openESL-captures/2026-10-16/probe13.log`. Target `BB950702FF94BAC8`. Observation, not a proof of which check failed. This `F9`/`FA` with payload[1]=0 fails `7EBD`; next trial sets payload[1]=1 as well. That trial (probe14, CI `38899656241`, `9c2716f`): `E0` → `D7` → `AA` every ~200 ms, chain `AD FFFF, F9 01 01, FA 01 01` in 13 µs each time, still only `AA` for ~380 s (482× `AA`), then the tag briefly revisited `E0`/`D7` and went silent as the battery faded (log `probe14.log`). So payload[1]=1 inside the `AD`-first chain did not advance the tag either. **probe15**: `F9 01 01` **alone** as the `AA` answer (no `AD`/`FA` first) **left the tag on `AA`** — one `AA`, then silence, screen redrew the default content. All three chained trials put `AD` first; `AD` moves state before `F9` is handled, and `7EBD` needs no `AD` pointer. `F9 01 01` alone is the answer that advances the tag.

Dispatcher first-byte cases (stock firmware, 0x4D35): `80` (second byte 1 or 2), `A7` (rejected in state 4, needs `FDA0 == A5`), `AD` (subtype in `F831` must be `15` or `02`), `D0`–`D2` (only states 8/9, sender must be the stored AP), `D8`, `DE` (sets 8 or 9), `F5`, `F9`, `FA` (both gated by `7EBD`). `D0`–`D2` as answers to `D7`/`AA` were not accepted because those types belong to the later data-receive states.

## What is known

- The stock ZBS243 firmware has a separate small "OTA image" next to the main firmware. It only receives radio frames (802.15.4 framing) and has flash erase, write and verify routines. It does not use the AES block. Source: Dmitry Grinberg, "Reverse Engineering an Unknown Microcontroller" (dmitry.gr), who studied it to find the radio registers but did not use it for updates.
- The protocol between a SoluM AP and a stock tag (joining, binding to a store network, how an update starts, any CRC or signature on the image, any encryption in the main firmware) is not published anywhere we have found. OpenEPaperLink and other projects flash ZBS243 tags with pogo pins only.
- We have backups of stock flash from our own tags (`backup/<MAC>.bin` and `.info.bin`, written by `tools/flash-tui`).

## Firmware facts (stock tag 000002079350341C, 2026-10-16)

Addresses are code addresses in the 64 KB main flash.

- The stock flash holds two programs. The main firmware is at 0x0000-0xDA7F (reset vector `LJMP 10B1`). A second, separate program sits at 0xE800-0xF8FF with its own vector table (reset at 0xE800 sets SP and jumps to 0xF2D5). Pages 0xDC00-0xE7FF are empty.
- The second program is a flash loader, not a radio program. It has the page write routine (0xF382-0xF3AC: SETTINGS |= 0x38, FPGNO = page, FWRTHREE = 3, 1 KB from xdata 0xE800, wait for TCON2 bit 3), a UART interrupt handler (0xF3F8: SCON, SBUF into ring buffers at xdata 0xE400), and SPI flash code that uses P1.1 as nCS (0xEEC0-0xEF74), the same pin and kind of chip as our W25Q80.
- Reading: the stock OTA most likely stores the new image in the external SPI flash, and this loader copies it into internal flash page by page. That is the same split as our own OTA (slots in SPI flash, bootloader copies).
- The main firmware also drives the SPI flash itself (code 0x1E00-0x2FA0): deep power-down 0xB9, release 0xAB, write enable 0x06, page program 0x02, block erase 0x52, with nCS on P1.1. So the external flash is used by the stock firmware, consistent with it staging an image there.
- Nothing in the main firmware jumps to 0xE800 or 0xE803 directly or by an obvious computed jump; the constants that look like 0xE803 are 1000 (0x03E8). The startup code at 0x10B1 is plain compiler startup. The entry is probably decided before the main firmware runs (a flag checked at reset) or by a path that is not a plain jump. Static tracing is slow because the compiler shares code tails between functions.
- Not found yet: what enters the loader (no `LJMP`/`LCALL` to 0xE800 or 0xE803 in the main firmware, so it is a jump through a pointer, a reset-time check, or the chip's boot ROM), and where the main firmware receives the image over the radio and writes it to the SPI flash. Those two answer open questions 2 and 3.
- Nothing seen yet about a CRC, signature or key on the image. Question 4 is still open.

## Firmware behaviour (2026-10-16, second pass)

MAC at `0xFC00` is `ADD2C8BA94FF 020793500102` (not the tag on air). Info page is calibration, no MAC.

State variable is XRAM `FD9F`. The table at `0x4014` is indexed by `state - 2`:

| `FD9F` | Sends / does |
|---|---|
| 4 | `D7` request (`0x8A89`) |
| 6 | `AA` request (`0x88E0`) |
| 7 | `F4` request (`0x8FBC`, hardcoded `F4 85`, echoes `F66F`) |
| 8 or 9 | data-receive (accepts `D0`/`D1`/`D2`) |
| `0x0B` | set by `AD` when subtype is `15` or `02`; table goes to `41D4` → state 3 |
| `0x0C` | set by `7C1A` after `A7`/`7BEE` when `FB7D == 0`; table goes to `41B9` → `5817` |
| `0x0D` | sets `FB64 = 1` (`0x4063`) |
| `0x0E` | `40C4`: clears `FB64`, not the page-0 rewrite |

`F5` at `0x5255` is the fork: if `FB64 != 1` it writes `F5` into `F66F`, sets state 7, and the screen goes white (the path we hit with `F5 00`). If `FB64 == 1` and a length byte is 0 it calls `2ECE` (state 8) instead — same receive state as `DE`, but after the `AD` path has marked “not a picture”. That is the best firmware-shaped reading so far.

Empty answers the current probe would send, and why they fail (second pass; see the third pass for what was wrong here):

- `F9`/`FA` (`0x9168` / `0x91A3`) call `7EBD`. On failure it returns 0 and both handlers jump to `0x2AE2`. A type byte plus zeros never passes. `7EBD` does **not** need a leftover `AD` pointer: it writes `F833` from the current packet (`4798`).
- `A7` (`0x4DCF`) is allowed in the `AA` state (it only rejects state 4) but needs `FDA0 == 0xA5`. It is a command, not a firmware blob.
- `AD` (`0x2823`) stores the packet pointer in `F82F` first. `0x4481` then sets `F831` from a 16-bit BE size at payload offset 3–4 vs XRAM `F560`: size < `F560` → `01` and return; size > `F560` → `15` (calls `0x6C6B`, sets state `0x0B`); size equal → `02` if a remaining count ≥ `0x1000`, else `00`. `15`/`02` with a checksum mismatch also go to state `0x0B`. The long success path at `0x2AA1` sets state 7, writes `AD` into `F66F` and calls `0x2803` (`FDA0 = A5`, which unlocks `A7`). Zero-padded `AD` has size 0 and becomes subtype `01` or `00`.

`DE` without `FDA0 == A5` still sets state 8 (`0x4ED0`). `D0`–`D2` are then accepted (`0x4D74`: state 8 or 9, then sender check `0x9D04`). Split: `D1` → `0x4933`, `D2` → `0x4A79`, `D0` → `0x7CA1`. Probe11’s push after `F4` was therefore the right types in the wrong session: the tag had gone through state 7 (picture) and likely stopped listening before the 150 ms push.

The program at `0xE800` is a UART factory loader, not radio OTA. Reset goes `E800 → E806 → F2D5` and sits in `SJMP F2EF` waiting for the UART vector at `E803`. Nothing in the main firmware jumps to `0xE800`. `A16E` is also not radio OTA (only caller is `A131` from `63BA`).

On-air chain tried (one listen window, same as `80`+`D8` after `D7`): `AD` with size `FFFF`, then `F9`/`FA` — payload[2]=1 (probe13), then also payload[1]=1 (probe14). Both left the tag on `AA`. `F9` alone with payload `01 01`, no `AD` first (probe15), **did** leave `AA` — see the run log.

## Firmware behaviour (2026-10-16, third pass)

Several second-pass readings were wrong.

`7EBD` (`F9`/`FA` gate):

- Starts by writing the current packet pointer into `F833` via `4798`. An earlier `AD` is not required for that store.
- Reads the byte at payload+2 as a chunk index and compares it with `F490`. Index 1 takes the first-chunk path (`7EF1`). Other values go to `7F47`.
- Success finally requires `F491 == F836`, where `F836` is the byte at payload+1 — that is why the first payload byte must be 1 to match the first-chunk index. On success `8EA1` clears `F490`–`F493`; the only direct `#F490` references sit inside `7EBD`.
- Failure returns 0; `9168`/`91A3` then `LJMP 2AE2`. Success (`R7 == 1`) calls `2ECE` (state 8, data-receive).
- Empty `F9` with only index 1 still fails; `payload[1] = 1` plus index 1 passes. Probe13 (`f5576f6`) set only payload[2] = 1; on air the tag stayed on `AA`. Probe14 set payload[1] = 1 as well, still in the `AD`-first chain; on air the tag still stayed on `AA`. **Probe15 sent `F9 01 01` alone (no `AD`/`FA`) and the tag left `AA`.** So `AD` first was the blocker.

The dispatcher at `4D35` is reached only from `6F1D` with `R6`/`R7` holding a buffer pointer (`F67D`); `4E14` returns a pointer to buffer base+10 and stores the byte at base+9 into `F53E`. The exact buffer layout is still being pinned down.

`AD` (`2823`) and `FDA0`:

- `FDA0 = A5` is written at `2803` (called from the long `AD` success at `2AC4`) and at `31AD` after `F723 == 1` and a compare at `0B1C`.
- A zero-padded `AD` with payload[1:3] = `0001` takes the equal / subtype `00` path, sets state 7, writes `F66F = AD`, does **not** set `FDA0 = A5`, and visits `2943 → 299B → 2AD0` with `F832 = 0x22` (traced, not an on-air result).
- First-record layout (`3599` / `F4A4` slot, checksum at `F49C`) is still unread.

`A7` is a command, not an image:

- `4DCF` rejects state 4 and needs `FDA0 == A5`, then jumps to `41FA`.
- `41FA` copies the packet pointer, reads fields into `FB75`/`FB79`, compares a 16-bit value with `0x0259`, and can set `FB64 = 1`.
- On the empty-size path (`FB79:FB7A == 0`) it calls `7BE7` with `R6 = 0x70` (SPI base `0x7000`).
- `7BEE` then looks at `FB7D`: zero → `7C1A` sets state `0x0C`; nonzero → state 8 (`7C22`).

Firmware-shaped install after that (not seen on air):

- State `0x0C` → `41B9` → `5817`. `5817` writes `F832 = E400`, clears `EA`, and calls `A1C9`.
- `A1C9` is the internal-flash **page 0** programmer: `SETTINGS |= 0x38`, `FWRTHREE = 3`, `FPGNO = 0`, 256 bytes, wait `TCON2.3`. Helper `A22D` points at xdata `E800` (the same 256-byte buffer the UART loader uses). It rewrites page 0 only, not the whole image.
- OpenESL OTA never rewrites page 0 (`ljmp 0x0400` in `tag/boot/boot_zbs243.c`). A stock radio install that goes through `5817` apparently does.
- `D2` at `4AF6` XORs a payload word with code `053C` (`1F 85`). `D0` is `7CA1`; `D1` is `4933`. Those stay the later data path, picture or otherwise.

Still open: exact on-air `AD`/`F9`/`FA`/`A7` layouts and checksums; what belongs in the SPI slot at `0x7000`; whether a CRC/signature/key sits on that slot (question 4).

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
- In the stock firmware the search frame is built at `0x8D10` (length 0x0D, `41 88`, sequence, PAN, broadcast, `AF 10`, `E0`, model byte) and sent from the channel sweep at about `0x9DF5-0x9E14` (a counter in XRAM FCBF runs from 11 up to 26). A second frame builder at `0x8D97` starts with first byte 0xC8 and broadcast, is called from a state at `0x98A3` and has a length that varies; it is the next candidate for the reply or join step.

- Receive side (not confirmed on air; this replaces an earlier wrong reading where 0xC8 was taken as the first byte of the frame): the receive routine at about `0x6DD0-0x6E80` copies the frame into a buffer and masks the second FCF byte with 0xCC. Three forms are handled: 0x88 (short destination, short source), 0xC8 (short destination, long source) and 0xCC (long destination, long source, the source address is copied out as 8 bytes). While the tag is unpaired (a flag in bit-addressable RAM is set) only the 0xC8 form is taken further, so an AP answer to the search is most likely a data frame `41 C8 <seq> <PAN> <dest short> <source long, 8 bytes> E1 ...`. The first payload byte and the payload length go to a routine at `0x9E44`, which knows the message types 0xE1 (length at least 26), 0xD0 (at least 33), 0xD1 (at least 37) and 0xD2 (at least 68) and copies the payload into separate XRAM buffers.
- The tag's own second frame (builder at `0x8D97`) is the same shape: `41 C8`, sequence number, destination PAN and address FFFF FFFF, then the tag's 8-byte MAC, then a variable payload. It is probably sent after an answer and announces the tag by MAC.
- First answer from a stock tag (2026-10-16, 1.54" tag, C6 fixed on channel 25): after our guessed `E1` reply (variant 1: `41 C8 <seq> 34 12 AF 10`, a long source address 00..01, `E1`, 25 zero bytes) the tag changed behaviour and sent its own frame, three times per round with rising sequence numbers: `61 CC <seq> 34 12 <dest 00 00 00 00 00 00 00 01> <source = tag EUI-64> D7 85 1F 00 00 00 00 01`. So FCF 0xCC61: data, acknowledgement requested, PAN compression, long destination (the address we gave as AP) and long source. The payload starts with `D7`, then the model byte (0x85 = 1.54"), then 6 bytes. The tag's screen blinked. The receive route in the stock firmware therefore accepted a minimal `E1` frame with 25 zero bytes after the type.
- The tag asks for an acknowledgement, and in promiscuous mode the radio does not send one, so it retransmits and gives up. The next probe build is therefore not promiscuous: PAN 0x1234 and extended address 00..01 are set so the hardware acknowledges the `61 CC` frame, and each request is answered with a guessed data frame of type D0, D1, D2 or E1 (cycling) so the next step shows in the log.
- The tag's request and the answer it expects (addresses in the main firmware): the state table at about `0x4014` picks an action per state; state 4 sends the `61 CC` request (builder `0x8A89` for the `D7` payload, sender `0x899D`, FCF bytes written explicitly) and waits 50 ticks before sending again. The answer is stored with the sender's 8-byte address first, then length, sequence number and payload, and the dispatcher at `0x4D35` looks at the first payload byte: 0x80 (needs the second byte to be 1 or 2), 0xA7 (not in state 4, needs 0xA5 in a state variable), 0xAD, 0xD0-0xD2 (only in states 8 and 9, and the sender address must equal the stored AP address), 0xD8, 0xDE, 0xF5, 0xF9 and 0xFA. Which of these answers a `D7` request is not clear from the code alone; guessed answers with zero padding of D0, D1, D2 and E1 did not advance the tag, which fits that those types belong to later states.
- Request replies (2026-10-16, second run): answering the `D7` request with `80 02` and `D8` made the tag move on. It now sends an `AA` request instead (`61 CC <seq> ... AA 85 1F 00 36 FF FF FF FF 00 00 00 <counter> FF FF FF FF FF 00 00 00 00 00 01 00 00 <14|15> 10 10 00 00`, 54-byte payload, counter rising, repeated about every 30 s).
- Next experiment: the probe build answers a `D7` request with `80 02` and `D8`, and every `AA` request with a burst of three candidate types (F9, FA, AD, DE, F5, A7, 80 01, 80 02, D0, D1, D2, E1), rotating, and logs which burst went out. A change in the tag's next request, a new frame type or a screen change shows which type it reacts to.
- Experiment prepared: `ap/sdkconfig.probe` (CI artifact `openesl-ap-probe.bin`, not in releases) makes the sniffer answer every `E0` search frame at once, on the same channel, with a guessed `E1` frame in three payload lengths (26, 40 and 70 bytes of zeros), and logs which variant went to which channel. If the tag accepts any of them it should change behaviour, most likely by sending its own `41 C8` frame with its MAC.

- Not yet known: what the AP's reply looks like. A real factory AP exists (no software; Ethernet port); it can be powered on next to the sniffer to see its replies.

## Open questions, in order

1. Does a stock tag without its AP transmit anything (scan, join request), and on which channel and PAN? The sniffer answers this: AP firmware in radio mode `Sniffer` with `ESL_SNIFF_HOP_MS` above 0 steps through channels 11-26.
2. Where is the OTA image in the stock flash, and what starts it (a frame from the AP, a flag in flash, a timer)? Reading: UART loader at `E800`; radio path looks like `AD`/`F9`/`FA` then `A7` staging SPI around `0x7000`, then state `0x0C` → `5817` → `A1C9` rewrites page 0. Not seen on air.
3. What frames does it accept: addressing, packet format, block size, acknowledgements.
4. Is the image checked (CRC, length, version, signature, key)? If it needs a key we cannot get, the study stops here.
5. Which flash range does it write? Our image is the bootloader at page 0 plus the application, and must keep the MAC at 0xFC06 (stock tags have no MAC in the info page; our firmware reads it from the info page today, `0x10–0x17`). Stock radio install through `A1C9` rewrites page 0. That is a real conflict with OpenESL’s “page 0 is never rewritten” split, if this path is the one used on air.

## Plan

1. Sniff a stock tag with the hopping sniffer (needs a tag that still runs stock firmware).
2. Disassemble one stock backup (`tag/tools/dis51.py`), find the OTA image and answer 2-5.
3. If nothing blocks: a "stock" radio mode on the C6 that speaks enough of the stock protocol to hand over our image, and a firmware fallback that reads the MAC from 0xFC06 when the info page is blank.

- Third run (2026-10-16, probe8): after `80 02` + `D8` and a burst `DE`, `F5`, `A7` (sent 31 us, 2.3 ms and 5.2 ms after the request) the tag sent the `AA` request only once (length byte 0x41, last status byte 0x16, earlier runs 0x36 and 0x14/0x15), then went silent, and its screen did a full refresh back to the default content. Only the first frame arrived within the tag's short listening window, so `DE` is the likely accepted type. Next build sends one answer per AA request, starting with `DE`, then `F5`, `A7`, `AD`, `F9`, `FA`, `80 01`, `80 02`, `D0`, `D1`, `D2`, `E1`; the tag must be restarted (battery out and in) for each trial.

- Fourth run (2026-10-16, probe9): `DE` as answer to the AA request sent the tag back to searching; `F5 00` was accepted. The tag then sent a new request type `F4`: `61 CC <seq> ... F4 85 00 00 00 00 F5 00 00 00 00 00 00` (13 byte payload, F5 echoed) and its screen went white (it clears the display before new content). Without an answer to `F4` it timed out after a while and fell back to the unpaired default. The stock code builds `F4` in state 7 (state table entry 0x405E, builder 0x8FBC). Dispatcher cases by first byte: 80, A7, AD, D0-D2 (only in states 8/9), D8, DE (sets state 8 or 9, probably start of the data transfer), F5, F9, FA. The next build answers each `F4` request with one candidate (DE, D8, F9, FA, AD, A7, 80 01, 80 02, D0, D1, D2, F5 01), rotating, and logs which.

- Fifth run (2026-10-16, probe10): a second stock tag in the room disturbed the rotation, so the probe can now be locked to one tag (`CONFIG_ESL_SNIFF_TARGET`, the tag's address as on air). F4 answers `DE`, `D8`, `F9` and `FA` all gave the same result: the tag sends F4 once, the screen turns white, then silence. Reading of the stock code: `DE` leads to the data-receive state (8 or 9), where only `D0`, `D1` and `D2` are accepted, so the next idea was that the tag waits for the AP to push.

- Seventh run (2026-10-16, probe12, C6 flashed with `openesl-ap-probe.bin` from CI `38892416632`): battery in on `BB950702FF94BAC8`. First logged frames were already `AA` (handshake happened before the serial read). Every `AA` got `AD` size `FFFF` in 13 µs. The tag sent only `AA`, ~230 ms apart, counters `0C`…`79` and up, rssi −66…−75. No `F4`, no `F9`/`FA` request, no return to `E0`. Isolated `AD` did not change the next request. Continuous `AA` is not proof that subtype 15 was rejected (see status).

- Eighth run (2026-10-16, probe13, CI `38894030644`): log from C6 startup, then battery in on `BB950702FF94BAC8`. Observed: `E0` at t=1055.29 s (rssi −64, `E1` in 20 µs) → `D7 85 1F 00 00 00 00 01` at t=1057.78 s (rssi −66, `80 02`+`D8` in 13 µs) → first `AA` at t=1061.93 s, then `AA` every ~200 ms, each answered `AD FFFF`, `F9`, `FA` in 13 µs. Counters `03`… rising, last status mostly `16`, rssi −59…−86. No other `61 CC` type, no second `E0`, no `F4`. Same next-request as isolated `AD`. The chain went out in the listen window; it did not change what the tag sent next. Next: same chain with `F9`/`FA` payload[1]=1 (that is what made `7EBD` return 1).

- Ninth run (2026-10-16, probe14, CI `38899656241`, `9c2716f`): log from C6 startup, battery in on `BB950702FF94BAC8`. Observed: `E0` at t=448.54 s → `D7` at t=452.64 s (`80 02`+`D8` in 13 µs) → `AA` from t=455.31 s to 835.55 s (~200 ms apart, rssi −59…−60 early, fading to −75), each answered `AD FFFF, F9 01 01, FA 01 01` in 13 µs. The tag briefly revisited `E0`/`D7` near t=728 s, then went silent at ~835 s with the battery fading. 482× `AA` in total. No other `61 CC` type, no `F4`. Same next-request as probe12/13: payload[1]=1 inside the `AD`-first chain did not advance the tag. Log: `openESL-captures/2026-10-16/probe14.log`.

- Tenth run (2026-10-16, probe15, built and flashed locally with ESP-IDF): log from C6 startup, battery in on `BB950702FF94BAC8`. Observed: `E0` at t=71.56 s (rssi −61, `E1` in 21 µs) → `D7 85 1F 00 00 00 00 C0` at t=76.71 s (rssi −64, `80 02`+`D8` in 13 µs) → one `AA` at t=80.02 s (rssi −65), answered `F9 01 01` **alone** in 13 µs, then silence. The tag's screen redrew the default content (barcode + numbers, not white). This is the first on-air move off the `AA` loop in the firmware direction, consistent with state 8 (data-receive). While in that state the tag re-ran the whole handshake roughly every ~5 min (search variant rotating 1→2→3: `E0`→`D7`→`AA`→`F9 01 01`→silent; `E0` at t=361, 634, 922, 1221, 1579 s), so state 8 is a short data-receive window that falls back to searching after ~5 min. Log: `openESL-captures/2026-10-16/probe15.log`. Next (probe16): after `F9 01 01`, push a `D0` block while the tag listens and watch whether it reacts.

- Sixth run (2026-10-16, probe11 / probe11b, tag lock confirmed): same handshake, then F4 answered with `DE` and a push of `D1`, `D0`, `D2` (payload bytes 7–8 = `FF 01`) and a plain `D1`, 150 ms apart. The tag sent nothing back. No new frame type, no D1 echo, no further F4. The push was discarded. Two repeats, same tag (`BB950702FF94BAC8`). After F4 the log shows only the four pushes and then stats (`rx 4`). The `AA` counters rise between pairings (persistent state in the tag's flash); the `D7` tail varies; `F4` is always `F4 85 00 00 00 00 F5 00 00 00 00 00 00`. Course correction: `F5 00` → `F4` + white screen is the stock **image** path. Firmware replacement has to leave that road. The next build answers each `AA` with one of `F9`, `FA`, `A7`, `AD` (the dispatcher types not yet tried as a single AA answer) and ignores `F4`. A new request type, a return to `E0`, or a screen change other than white shows which type it reacts to. Battery out and in for each trial; leave the C6 running so the candidate counter advances.

- Payload notes from the same runs (1.54" tag, model byte `85`): `D7` is `D7 85 1F 00 00 TT 00 00` with tail `TT` rising across wakes (`00`, `01`, `03`, `07`). `AA` is 32 bytes after the type: `AA 85 1F 00 <status> … <counter> FF FF FF FF FF … <pair that rises> … 15|16 10 10 00 00`. The rising fields are counters, not a fixed join token. `F4` echoes the `F5` we sent.