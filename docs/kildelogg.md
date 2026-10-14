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

## Unconfirmed: in the code, but must be checked before it counts as a fact

These appear in `tag/src/epd.c` marked "unconfirmed". They are the common codes in the SSD1680 family, not yet checked against a datasheet. The controller on the ST-GM29XX2 is also not confirmed (phase 0). Move a line up into the table above once it is confirmed.

| Fact | Must be confirmed against |
|---|---|
| Commands 0x01 driver output, 0x10 deep sleep, 0x11 data entry, 0x12 SW reset, 0x18 temperature sensor, 0x20 master activation, 0x22 display update control 2, 0x24 write black/white RAM, 0x26 write red RAM, 0x3C border, 0x44/0x45 RAM window X/Y, 0x4E/0x4F RAM counter X/Y | The datasheet of the controller actually on the display |
| 0xF7 as the update sequence with OTP waveform | The same datasheet |
| The black/white RAM stores 1 for white (hence `EPD_BW_INVERT`) | Datasheet + test on the display |
| 128 pixels per RAM line along the short side, 296 lines | Datasheet + test on the display |

Clock, GPIO, UART, timer, watchdog, ADC, flash writing, radio and deep sleep on the ZBS243 are still stubs in `tag/src/hal/zbs243/`. No register addresses are in use yet.
