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
| 2026-10-14 | ST-GM16XX2 display: FPC marked HINK-E0154A16-A0, 200×200 black/white | stock-fw + measurement | FPC label read on the tag. Resolution: stock fw stores 0x00C8 × 0x00C8 (xdata 0xFD8E/0xFD90, code 0x1D82) and sends it with the init sequence. See [st-gm16xx2.md](st-gm16xx2.md) | – |
| 2026-10-14 | Flash 0xFC00–0xFC0C is copied to xdata 0xFD79–0xFD85 at boot; 0xFC0A = board variant, 0xFC0B = panel type (1 = UC-type, 2 = SSD-type) | stock-fw | Copy routine at code 0x650B; flags used at 0x5D3F, 0x5F40–0x5F84, 0x9BCC. Both of our tags have `01 02` | – |
| 2026-10-14 | Board variant 1: BS1 on P1.2, P0.3 driven high before EPD access; variant 2: BS1 on P1.5, P1.6 high | stock-fw + experiment | Code 0x5F40–0x5F84. The display works without driving P0.3 (tested on two tags) | – |
| 2026-10-14 | EPD pins on the ST-GM16XX2: CS P1.7, D/C P2.2, RESET P2.0, BUSY P2.1 | stock-fw + experiment | Command routine 0x3F8A (CLR P2.2), data 0x3F66 (SETB P2.2), CS P1.7 around each byte. Confirmed by a working panel | – |
| 2026-10-14 | SSD-type panel: BUSY is high while busy, low when ready | stock-fw + experiment | Wait loop at 0x5A76 (`JNB P2.1` for type 2). BUSY read 0 after reset on the tag | – |
| 2026-10-14 | SSD init on ST-GM16XX2: 01[C7 00 00] 3A[1B] 3B[0B] 11[01] 44[00 18] 45[C7 00 00 00] 3C[33] 0C[C7 8F 8F] 21[83] | stock-fw + experiment | Code 0x9C22. Panel draws with this sequence | – |
| 2026-10-14 | Refresh on ST-GM16XX2: 1A[temp 00] 22[D7] 20, wait BUSY, 21[03], 10[01]. 0x18=0x80 with 0x22=0xF7 does not change the panel | stock-fw + experiment | Code 0x5ABC. The 0xF7 variant tested on the tag: refresh runs, nothing changes | – |
| 2026-10-14 | The ST-GM16XX2 controller ignores 0x46/0x47 (auto-fill) and the invert bits in 0x21; BW RAM 1 = white | experiment | Uncleared RAM showed as noise; 0x21 = 0x8B gave no change; manual fill with 0xFF gave white | – |
| 2026-10-14 | ZBS243: reading back a port bit set as output (P2_2) does not return the written level | experiment | Code that branched on `P2_2` never saw it high; tracking the level in a variable fixed it | – |

## Unconfirmed: in the code, but must be checked before it counts as a fact

These appear in `tag/src/epd.c` marked "unconfirmed". They are the common codes in the SSD1680 family, not yet checked against a datasheet. The controller on the ST-GM29XX2 is also not confirmed (phase 0). Move a line up into the table above once it is confirmed.

| Fact | Must be confirmed against |
|---|---|
| Commands 0x01 driver output, 0x10 deep sleep, 0x11 data entry, 0x12 SW reset, 0x18 temperature sensor, 0x20 master activation, 0x22 display update control 2, 0x24 write black/white RAM, 0x26 write red RAM, 0x3C border, 0x44/0x45 RAM window X/Y, 0x4E/0x4F RAM counter X/Y | The datasheet of the controller actually on the display |
| 0xF7 as the update sequence with OTP waveform | The same datasheet |
| The black/white RAM stores 1 for white (hence `EPD_BW_INVERT`) | Datasheet + test on the display |
| 128 pixels per RAM line along the short side, 296 lines | Datasheet + test on the display |

Clock, GPIO, UART, timer, watchdog, ADC, flash writing, radio and deep sleep on the ZBS243 are still stubs in `tag/src/hal/zbs243/`. No register addresses are in use yet.
