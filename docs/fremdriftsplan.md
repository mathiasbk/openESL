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

## Status checklist (2026-10-15)

What is done and what is not, per phase. The details of each phase follow below.

**Phase 0: Preparation**
- [x] SDCC and ESP-IDF (v5.5.5) installed; the tag firmware and the AP build
- [x] Flashing through ZBS_Flasher on a Wemos works (`tools/flash-tui`)
- [x] Display controller found (ST-GM16XX2, 1.54"), pins mapped
- [ ] Power budget for requirement T6 calculated from the real battery

**Phase 1: Signs of life**
- [x] Clock, calibrated delay, GPIO, UART, MAC read on the tag

**Phase 2: Display**
- [x] Panel init, drawing, fonts and layouts on the 1.54" (black/white only, no red)
- [x] Display deep sleep after drawing

**Phase 3: Radio**
- [x] The tag sends and receives 802.15.4 frames with the C6
- [x] RX after deep sleep works (the jig tag polls every ~60 s)

**Phase 4: AP foundation**
- [x] 802.15.4 on the C6 (channel 25, AP, sniffer and stub modes)
- [x] Table of tags in NVS, HTTP API with a token, status page (A6)
- [x] The API over the USB port (`esl-client --serial`), PR #5
- [ ] WiFi on the C6 Super Mini: the board's WiFi is dead (hardware), so WiFi and coexistence are untested
- [x] WiFi setup without menuconfig: setup network `openesl-XXXX` with a form at `/setup`, `GET/PUT /api/wifi` (also over USB, `esl-client wifi`), token set from the form; host-tested and built (`ap/README.md`, "WiFi setup")
- [ ] WiFi setup tried on hardware (needs a C6 with working WiFi): first start opens the setup network, the form joins the network, a wrong password brings the setup network back after 180 s, `PUT /api/wifi` over USB
- [ ] Response time measured on hardware (requirement A3)
- [x] Status screen for C6 boards with a 0.96" ST7789 LCD (`ESL_DISPLAY`, `ap/sdkconfig.lcd`, CI builds `openesl-ap-lcd.bin`); host-tested and built
- [ ] Status screen on the new board: check the pins (guessed), the gaps with the start-up frame, colours and rotation; then WiFi setup and coexistence on its working WiFi

**Phase 5: End to end**
- [x] PUT → the right tag shows the new content, tested on hardware (PR #7)
- [x] Same content, new content, unknown tag, tag restart, layout file over radio, AP gone for 5 min
- [x] End-to-end simulation in CI (`sim/`): the tag firmware against the AP core through the API, with simulated radio and time; the whole test matrix below, CONFIG, layout files and T12, screens saved as PNG
- [x] CONTENT reply lost in the middle of an exchange: in the simulation (cases 4a–4d: one reply lost, both tries lost, the confirming POLL lost, the AP dying while it answers)
- [x] AP gone for an hour: in the simulation (case 8)
- [ ] Both of these provoked on hardware

**Phase 6: Power**
- [x] Deep sleep with timer wake-up on the tag
- [ ] Measure sleep current, energy per POLL and per display update
- [ ] Battery measurement (T8): not supported on this panel yet, reported as 0 mV; in progress (issue #4)
- [x] Longer interval without the AP (T11): backoff up to 600 s

**Phase 7: API polish and example client**
- [x] Example client in Python (`tools/esl-client`)
- [x] Status page on the AP (A6)
- [x] Web flasher and label designer (GitHub Pages, `web/`)
- [x] API documentation checked against the code (`docs/protokoll.md` part 2: transports, limits, error codes, every field in `GET /api/tags`); the AP runs the same `esl_api.c` as the host tests
- [x] Interactive API docs: `docs/openapi.yaml` (OpenAPI 3.1) is the reference, shown with Scalar in the API tab of the web page; CI runs every example in it against `esl_api.c` and checks the replies against the schemas (`ap/test/openapi/check_openapi.py`)
- [ ] "Test Request" from the API tab against the AP on the jig, through `esl_client.py --serial /dev/ttyACM0 serve` (tried here against the API code on the host only)
- [ ] The settings endpoints (`/api/config`, `/api/tags/{mac}/config`) and CONFIG tried on hardware; the other endpoints were used over the USB port in phase 5

**Phase 8: Operational reliability**
- [x] Watchdog on the tag (T10)
- [x] Watchdog on the AP: the radio task and the serial API task are registered with the ESP-IDF task watchdog, which restarts the AP after 10 s (`ap/README.md`, "Watchdog")
- [ ] Watchdog on the AP verified on hardware (a stuck task gives a restart, and `GET /api/tags` shows `"reset":"task_wdt"`)
- [x] Last content in the tag's flash (T12): the content and the layout file are kept in flash and drawn again at start-up, tested on PC (`tag/README.md`, "Flash")
- [ ] T12 verified on hardware: a tag restarted after new content (and with a layout file) shows it without the AP; `hwtest` shows "layout store kept" on the second boot
- [ ] Long-term test: 10 tags for a week
- [ ] Flashing recipe and jig under a minute per tag

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

### Results on hardware (2026-10-15)
ESP32-C6 Super Mini as AP (API over USB, `esl-client --serial`), the 1.54" tag in the jig, firmware from master, poll every 60 s.

| Case | Result |
|---|---|
| Same content again | `changed: false`, the tag keeps getting IDLE and does not redraw |
| New content | CONTENT on the next poll (60 s after the PUT), reported as shown on the poll after (124 s). The display updates at the first poll, so G1 (2 min) holds with a 60 s interval; the API confirms one poll later |
| Tag unknown to the AP (`DELETE`, then the tag polls) | The AP registers it with no content and answers IDLE; the tag keeps what it shows. A new PUT reaches it as usual |
| Tag restarted with empty content | Gets CONTENT on its next poll |
| Layout file over radio | 2 blocks (174 bytes) and 8 blocks (684 bytes) in one wake-up without retries, about 80 ms for 8 blocks. After the last block the tag polls again at once and gets the content drawn with the new layout (`shelf` with a highlighted price shown) |
| AP gone for 5 min with new content waiting | Content survives the AP restart (NVS). The tag had backed off (3 misses, then doubling) and polled 205 s after the AP came back, got CONTENT and went back to 60 s |

Not tried on hardware yet: losing the CONTENT reply itself in the middle of an exchange, and the AP gone for an hour. Both run in the simulation below.

### Results in the simulation (2026-10-15)
`make -C sim` ([sim/README.md](../sim/README.md)): `tag/src/app.c` and `ap/components/esl_core` on the PC, joined by a simulated radio link, driven through `esl_api_handle()`, with simulated time. Both models, in CI on every push. The same numbers for the cases above (same content, new content, unknown tag, restart, layout file) as on hardware.

| Case | Result |
|---|---|
| CONTENT reply lost once | The retry in the same wake-up (2–10 ms later) gets it; not counted as a miss |
| CONTENT lost in both tries | One miss without backoff, CONTENT at the next wake-up 60 s later; the API shows `pending` until the tag confirms |
| The POLL that confirms it is lost | `pending` stays true one interval longer; nothing is resent |
| The AP dies while sending CONTENT, back after 3 min | The content survives in NVS; the tag gets it 60 s after the AP is back |
| AP gone for an hour with new content waiting | 10 misses, the sleep reaches 600 s; the tag found the AP 390 s after it came back (at most 600 s) and went back to 60 s |
| AP gone for more than 2 h | "No AP found" after 7020 s: `lost_s` counts the sleep the tag is about to start, so the screen comes up to one sleep (600 s) before the 2 h in T9 have passed. Content again within 600 s when the AP is back |

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
