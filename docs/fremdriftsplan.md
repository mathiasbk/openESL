# Work plan

Unit: **evening** = 2–3 hours. The estimates are rough and assume you learn as you go. The largest spreads are where the registers are unknown (phases 1, 3 and 6).

The order is **risk first**: the hardest and most uncertain part (the tag's radio) is tested early, before you spend time on the AP, clients and polish. The AP track can run in parallel when you need a break from the tag.

## Overview

| Phase | Content | Estimate | Milestone (done when …) |
|---|---|---|---|
| 0 | Preparation | 2–3 evenings | An empty firmware builds and flashes; the tag starts and does not hang |
| 1 | Signs of life on the tag | 3–8 evenings | The tag writes "hello" on UART and can wait a known number of ms |
| 2 | Display | 4–8 evenings | The tag draws "24,90" white on a black box and one line of text in black |
| 3 | Radio (gate) | 5–12 evenings | The tag sends a frame that the C6 sniffer sees, and receives a frame from the C6 |
| 4 | AP foundation | 3–5 evenings | The C6 with WiFi answers a fake tag with IDLE/CONTENT in under 5 ms |
| 5 | End to end | 3–5 evenings | `curl -X PUT …/api/tags/{mac}` → the right tag shows the new price |
| 6 | Power | 5–10 evenings | Measured average consumption within the budget in requirement T6 |
| 7 | API polish and example client | 2–4 evenings | A price sent by the example client shows on the tag within 2 min (requirement G1) |
| 8 | Operational reliability | 3–6 evenings | 10 tags run for a week without any hanging or running out of battery |
| **Total** | | **30–60 evenings** | About 3–6 months with 2–3 evenings a week |

## Phase 0: Preparation
- Install and pin the SDCC and ESP-IDF versions.
- Back up (`read`) the original SoluM firmware on 2–3 tags before flashing anything else. It is the basis for the register study, and lets you restore the tags.
- Confirm the battery type and capacity, and calculate the power budget (requirement T6).
- Confirm the display controller and download the datasheet.
- Use a multimeter to find which ZBS243 pins go to the display's FPC (CS, DC, RST, BUSY, SCK, MOSI, power), and note them in [kildelogg.md](kildelogg.md).
- Build and flash a firmware that is just an endless loop. Confirm that the flasher writes and that the tag does not start in a previous firmware.

## Phase 1: Signs of life
- Find out how UART TX is set up (study the SoluM backup in Ghidra: which SFRs are written early in start-up?). Alternative: bit-bang UART on a known GPIO and measure with the logic analyser.
- Find the clock setup and make a calibrated delay function (verify with the logic analyser).
- Find GPIO direction and output. Confirm by toggling a pin you can measure.
- **Checkpoint:** if you have no UART or toggling pin after 8 evenings, stop and consider the options in [risiko.md](risiko.md).

## Phase 2: Display
- Bit-bang SPI on the pins from phase 0. Reset, wait for BUSY, read a status register to confirm contact.
- Init according to the datasheet, fill white, then black. Use the OTP waveform.
- Write the font generator and `draw`/`layout`. Test layouts 1 and 2 with fixed strings, including æøå and text that is too long.
- Put the display into deep sleep after drawing.

## Phase 3: Radio (gate)
- Build the C6 sniffer first (see [ap-firmware.md](ap-firmware.md)).
- Study how the SoluM firmware turns on the radio and transmits. Look for writes to the same registers around a send sequence. Confirm with experiments.
- Sub-goals: (a) the tag sends something the sniffer sees, (b) correct channel and format, (c) the tag receives a frame that the C6 sends, (d) RSSI.
- **Gate:** if you cannot reach (a) after 12 evenings, the tag's own firmware is not realistic without more help. Read [risiko.md](risiko.md) before continuing.

## Phase 4: AP foundation (can run in parallel with phases 1–3)
- 802.15.4 up on the C6 with a fixed channel and PAN ID.
- Fake tag on the second C6.
- WiFi, NVS, table of tags, pre-built replies. Measure response time.
- The HTTP API ([protokoll.md](protokoll.md) part 2) with a token. Test with `curl` and the fake tag.

## Phase 5: End to end
- `proto.c` on the tag: POLL → IDLE/CONTENT → draw.
- Test matrix: new tag (unknown MAC), same content (no drawing), new content, CONTENT lost (AP switched off in the middle), AP gone for an hour.
- Adjust the listening window and the retry.

## Phase 6: Power
- Find deep sleep with timer wake-up (SoluM backup + experiments).
- Measure: sleep current, energy per POLL, energy per display update. Calculate the average and compare with the budget.
- Turn off everything not needed while asleep (display, radio, pins that leak current through pull-ups).
- Battery measurement (requirement T8) and longer interval without the AP (T11).

## Phase 7: API polish and example client
- Document the API with complete examples, and check that it is stable.
- A small example client (e.g. a Python script) that sets content, does a full send and reads status ([ap-firmware.md](ap-firmware.md), "Writing a client").
- The status page on the AP (requirement A6) that lists tags, battery and "last seen".

## Phase 8: Operational reliability
- Watchdog on the tag (T10) and the AP.
- Store the last content in the tag's flash (T12) if flash writing has been found.
- Long-term test: 10 tags, one week, log status from the API. Fix errors.
- Write a flashing recipe per tag (backup → write → MAC) and build a jig so that it takes under a minute per tag.

## After version 1 (not planned in detail)
Signed content (T16), own flasher, OTA (T15), images (T14), barcode (T13), several APs.
