# Hardware notes and sources

One line per hardware fact used in the code, with where it came from. This makes it easy to check a fact again, and to see what is confirmed and what is still a guess.

Source types: `datasheet`, `measurement`, `experiment`, `stock-fw` (study of a backup of the tag's original firmware), `text` (article or wiki), `own` (earlier work in this project).

| Date | Fact | Source | Detail (document, page, address, measurement method) | Used in |
|---|---|---|---|---|
| 2026-10-10 | The chip is SEM9110 (ZBS243 family) | measurement | Read from the chip on an opened tag | – |
| 2026-10-10 | The debug interface pads and their order | text | Community documentation of the ST-GM29XX2 (see Credits in the main README) | – |
| 2026-10-10 | The stock MAC is in flash 0xFC06–0xFC0B | own | Read from a flash dump. Should be confirmed independently, e.g. by comparing the MAC on the label with a flash dump | – |
| 2026-10-12 | Code is linked only below 0xFC00, because the stock MAC is at 0xFC06–0xFC0B | own | Follows from the line above | `tag/tools/build.py` (`CODE_SIZE`) |
| 2026-10-12 | 802.15.4 frame control: bits 0–2 frame type (data = 1), 3 security, 4 frame pending, 5 ACK request, 6 PAN ID compression, 10–11 destination addressing mode, 12–13 version, 14–15 source addressing mode; short address = 2, extended = 3 | datasheet | IEEE 802.15.4, MAC frame format (frame control field) | `shared/proto.h` |
| 2026-10-12 | 802.15.4 sends multi-byte fields and extended addresses least significant byte first | datasheet | IEEE 802.15.4, general frame format | `tag/src/proto.c` |
| 2026-10-12 | The FCS is CRC-16 with polynomial x^16+x^12+x^5+1, initial value 0, reflected (check value 0x2189 for "123456789") | datasheet | IEEE 802.15.4, FCS field. The check value is tested in `tag/test/test_proto.c` | `tag/src/proto.c` |
| 2026-10-12 | 8051: interrupts are disabled with EA (IE.7) | datasheet | Intel MCS-51 documentation, IE register. The name `EA` comes from SDCC's `<8051.h>` | `tag/src/hal/zbs243/hal_zbs243.c` |
| 2026-10-14 | ZBS243 SFRs: CLKSPEED 0x8E (0x01 = 16 MHz crystal), CLKEN 0xB7 (0x01 timers, 0x20 UART), CFGPAGE 0xFF, P0/P1/P2 FUNC 0xAD–0xAF, DIR 0xB9–0xBB (1 = input), PULL 0xBC–0xBE | text | Dmitry Grinberg's ZBS243 reverse engineering (zbs243.h), as used in atc1441's ZBS_Flasher examples and OpenEPaperLink Tag_FW_ZBS243. Confirmed on 000002079350341C: UART and pins work | `tag/src/hal/zbs243/zbs243.h`, `hal_zbs243.c` |
| 2026-10-14 | UART TX on P0.6 (debug pad 4) with P0FUNC bit 6; UARTSTA 0x98 (0x10 on, 0x02 TX ready), UARTBUF 0x99, baud = 16 MHz / (BRG + 1), BRG 138 = 115200 | text + measurement | As above. Measured: clean text at 115200 on the debug D1 mini (ttyUSB0) | `hal_zbs243.c` |
| 2026-10-14 | Timer 0 is a classic 8051 timer clocked at 16 MHz / 12 (1333 ticks per ms) | text + measurement | As above. Measured: 87 one-second counts in 87 s of PC time | `hal_zbs243.c` (`hal_delay_ms`) |
| 2026-10-14 | Watchdog in CFGPAGE 4: WDTENA 0xBA, WDTPET 0xBB, WDTCONF 0xBF bit 7 | text | As above. Not exercised yet (turned off in `hal_init`) | `hal_zbs243.c` |
| 2026-10-14 | The MAC is in the info page at 0x10–0x17, least significant byte first; read with the flash DMA (CFGPAGE 4, FPGNO 0x80, CLKSPEED 0x21 during the copy) | text + measurement | Same source for the flash DMA. Read back 000002079350341C on the tag, same as the flasher's MAC backup | `hal_zbs243.c` (`hal_mac_read`) |
| 2026-10-14 | XRAM is 8 KB at 0xE000–0xFFFF; the radio registers are memory mapped at 0xDF00–0xDFFF | text | As above (OEPL links at 0xE300 with size 0x1D00) | `tag/tools/build.py` |
| 2026-10-14 | EPD pins: CS P1.7, D/C P2.2, RESET P2.0, BUSY P2.1 (high = busy), SCK P0.0, MOSI P0.1, BS1 P1.2 low | text + measurement | OEPL boardcommon.c / sem9010.c, and [st-gm16xx2.md](st-gm16xx2.md). Measured: BUSY goes high after 0x12 and for about 2.9 s after a refresh | `hal_zbs243.c` |
| 2026-10-14 | Radio init values, channel table, frame filter (0xDF88–0xDF93, short address 0x0000 as OEPL), TX via TXPTR/TXLEN + TRIGGER 0x02, RX via GOTLEN + RX DMA (TRIGGER 0x04), pending flags at 0xDFAD (0x40 TX done, 0x20 frame in FIFO, 0x10 ACK/preamble) | text + experiment | Dmitry Grinberg / OEPL radio.c. Tested polled, without CPU interrupts, against the OEPL AP on channel 11: the AP logs our check-ins and ACKs them, and the tag receives the AvailDataInfo reply with a correct checksum | `tag/src/hal/zbs243/radio_zbs243.c` |
| 2026-10-14 | RADIO_GOTLEN (SFR 0xFA) is a read port on the RX FIFO: the first read gives the PHY length (frame + 2), and each further read pops the next frame byte. Read it once. After the DMA, rxbuf[len - 1] is the RSSI and rxbuf[len] has bit 7 = FCS correct | experiment | Seen on the tag 2026-10-14 (0x29, then 0x41 = first frame byte). Polling GOTLEN during reception broke receiving | `radio_zbs243.c` |
| 2026-10-14 | The flash DMA (hal_mac_read) overwrites SETTINGS, which clears bits the radio init sets there; read the MAC before the radio init | experiment | Before the fix the radio "sent" (0x40) but the AP heard nothing | `radio_zbs243.c` (`radio_on`) |
| 2026-10-14 | ST-GM16XX2: a controller RAM line (Y) is a screen row and RAM X bytes run along it, MSB = leftmost pixel; with data entry 0x07 and the layout transposed 8 columns at a time, the image is the right way round. 1 = white in the BW RAM | measurement | Tag 000002079350341C: the start screen ("waiting for content") read correctly (Mathias, 2026-10-14); one layout column per RAM line gave a rotated, mirrored image | `tag/src/epd.c` (`EPD_TRANSPOSE`) |
| 2026-10-14 | Not understood: with a length held in a CPU register across a CFGPAGE = 4 section in `fetch_frame`, the computed frame size came out one less than the length read. Keeping the value in xdata fixed it | experiment | Seen in the SDCC 4.5.0 code for `fetch_frame`; the generated code looked correct | `radio_zbs243.c` |

## Unconfirmed: in the code, but must be checked before it counts as a fact

These appear in `tag/src/epd.c` marked "unconfirmed". They are the common codes in the SSD1680 family, not yet checked against a datasheet. The controller on the ST-GM29XX2 is also not confirmed (phase 0). Move a line up into the table above once it is confirmed.

| Fact | Must be confirmed against |
|---|---|
| Commands 0x01 driver output, 0x10 deep sleep, 0x11 data entry, 0x12 SW reset, 0x18 temperature sensor, 0x20 master activation, 0x22 display update control 2, 0x24 write black/white RAM, 0x26 write red RAM, 0x3C border, 0x44/0x45 RAM window X/Y, 0x4E/0x4F RAM counter X/Y | The datasheet of the controller actually on the display |
| 0xF7 as the update sequence with OTP waveform | The same datasheet |
| The black/white RAM stores 1 for white (hence `EPD_BW_INVERT`) | Datasheet + test on the display |
| 128 pixels per RAM line along the short side, 296 lines | Datasheet + test on the display |

Clock, GPIO, UART, timer, MAC reading and the radio are implemented in `tag/src/hal/zbs243/` (radio receive not yet confirmed on air). ADC/battery, temperature, flash writing, watchdog use and deep sleep are still stubs.
