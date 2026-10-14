# ST-GM16XX2: what we learned getting it to draw

The small SoluM tag (label ST-GM16XX2, display FPC marked **HINK-E0154A16-A0**) was brought up on 2026-10-14 by studying a backup of its stock firmware and testing changes to the OpenEPaperLink (OEPL) tag firmware. The OEPL changes live in our fork of OEPL, not here (this repo contains no code from other firmware). This page keeps the hardware facts, so our own tag firmware can use them. Each fact is also a line in [kildelogg.md](kildelogg.md).

## Summary

- The display is **200×200, black/white**, not 152×152 as on the older 1.54" ST-GR16000.
- The controller is **SSD-type** (BUSY high while busy), on the same pins as on the other M2 tags.
- It **ignores** some commands that SSD1619/SSD1680-style code relies on. Code written for those controllers shows nothing, or shows noise or an inverted image (see the table).
- With the stock init and refresh sequence below, plus the workarounds, the panel draws correctly.

## How the stock firmware picks the panel

At boot the stock firmware copies flash `0xFC00–0xFC0C` to xdata `0xFD79–0xFD85` (the routine at code `0x650B`, length 0x0D). The two bytes after the MAC then select the hardware:

| Flash | Our tags | Meaning |
|---|---|---|
| `0xFC0A` | `01` | Board variant. 1: BS1 on P1.2, P0.3 driven high before EPD access. Other: BS1 on P1.5, P1.6 high |
| `0xFC0B` | `02` | Panel type. 2: SSD-type (BUSY active high). 1: UC-type (UC8151 family, BUSY active low) |

The stock firmware contains complete drivers for both panel types. Both the 1.6" (`000002079350341C`) and the 2.9" (`000001C280923B15`) tags we have show `01 02`, so the 2.9" probably needs the same treatment (not tested yet).

## Pins

As on the other M2 tags: EPD CS P1.7, D/C P2.2, RESET P2.0, BUSY P2.1, BS1 P1.2 (low = 4-wire SPI).

## Init and refresh (SSD path of the stock firmware)

After a hardware reset (RESET low, then high) and `0x12` soft reset:

```
01 C7 00 00        driver output: 200 gate lines
3A 1B              dummy line period
3B 0B              gate line width
11 01              data entry: X increment, Y decrement
44 00 18           RAM X window 0..24 (200 px)
45 C7 00 00 00     RAM Y window 199..0
3C 33              border waveform
0C C7 8F 8F        booster soft start
21 83              display update control 1
```

Refresh:

```
1A <temp> 00       write the temperature register (°C in the first byte)
22 D7              display update control 2
20                 master activation, then wait while BUSY is high
21 03
10 01              deep sleep
```

The stock firmware never sends `0x18` (temperature sensor select). It writes the temperature itself with `0x1A` and uses `0x22 = 0xD7`. With OEPL's `0x18 = 0x80` and `0x22 = 0xF7` the refresh runs but the panel does not change.

## Things this controller does not do

| What | What happens | Workaround |
|---|---|---|
| `0x46`/`0x47` auto-fill of RAM | Ignored. RAM keeps random content, which shows as noise | Clear the RAM by writing every byte |
| Invert bits in `0x21` | Ignored | Invert the pixel data before writing it |
| Red plane (`0x26`) | Not shown (BW panel) | Draw everything in the black/white plane |

Polarity: in the BW RAM, `1` = white and `0` = black.

## ZBS243 detail found on the way

Reading a port pin that is set as an output (for example `P2_2`, the EPD D/C line) does not return the level the code wrote. Code must keep its own copy of an output state if it needs it later.

## Consequences for our own tag firmware

- `tag/src/epd.c` uses `0x18` and `0xF7`, which do not work on this panel. Use the sequence above.
- [tag-firmware.md](tag-firmware.md) assumes the 1.54" is 152×152. The ST-GM16XX2 is 200×200.
- Read `0xFC0A`/`0xFC0B` at boot to choose the panel driver, as the stock firmware does.

## Tools

- [`tag/tools/dis51.py`](../tag/tools/dis51.py) is a small 8051 disassembler used to read the stock firmware: `python3 dis51.py flash.bin 9C22 9C90`.
- A D1 mini running a SoftwareSerial passthrough on D5 reads the tag's UART (pad 4) at 115200. Lines are clean when the tag sends short bursts.
- With the ESP8266 ZBS_Flasher, `reset` only works after the flasher has done an operation since it powered up (for example `read-infopage`). Otherwise it times out.
