# SoluM ESL remote (SLT-RC004)

A handheld remote that SoluM sold to shops for its stock ESL system. We sniffed it with the AP in sniffer mode on 2026-10-19 to see whether openESL could use it as a wireless button. Result: the frames are trivial to recognise, but the remote sends on channel 11 and openESL uses channel 25, so nothing uses it yet.

## The device

| | |
|---|---|
| Model | SLT-RC004, FCC ID [E2X-RC](https://fccid.io/E2X-RC) ("ESL Remote Controller", Samsung Electro-Mechanics, granted 2014-10-13) |
| Radio | 2405–2480 MHz, 8 mW, IEEE 802.15.4 (the FCC test report is titled "DTS-ZIGBEE") |
| Buttons | **W** (large), **left**, **right**, and an OFF/ON switch |
| Power | 3 V (two AAA) |
| Inside the battery compartment | Ten round pads in two rows of five, likely test or programming pads. Not measured, and the MCU is unknown. |

The newer SoluM Newton "Hybrid Remocon" (EL090MBCX0) is documented: the centre button wakes or refreshes a stock tag, and left and right flip between pages stored on the tag. It adds active NFC. The SLT-RC004 most likely does the same over 2.4 GHz only (inferred, not tested against stock tags).

## What it sends

Measured with a sniffer locked on channel 11 while the buttons were pressed W, left, right, W, left, right, about 1 s apart.

- **Channel 11**, unencrypted 802.15.4 data frames.
- **One press = one burst of 10 identical frames**: 5 frames about 10 ms apart, an ~80 ms gap, then 5 more (~160 ms in all).
- The frame (12-byte PSDU without the FCS):

```
41 88 | seq | FF FF   | FF FF | EF BE        | 19 | cmd | 00
FCF     seq   dst PAN   dst     src = 0xBEEF   payload
```

  - FCF 0x8841: data frame, PAN ID compression, short addresses, no ACK request.
  - Broadcast to PAN 0xFFFF and address 0xFFFF from short address 0xBEEF. The source is the same every time, so the frame carries no identity for the remote.
  - `seq` goes up by 1 per press. All 10 frames of a burst share it, so a receiver can collapse a burst to one press.
  - **`cmd`: 01 = W, 02 = left, 03 = right.**
- No encryption, no rolling code: any 802.15.4 radio can receive the commands, and send them too.
- Signal at the AP, remote held next to it: about −60 to −87 dBm.

Example frames as the sniffer logs them (the last two bytes are the RSSI and LQI that the ESP-IDF driver writes over the FCS, not frame data):

```
rx ch 11 14 B rssi -68: 41 88 9C FF FF FF FF EF BE 19 01 00 BC 0A   W
rx ch 11 14 B rssi -71: 41 88 9D FF FF FF FF EF BE 19 02 00 B8 0A   left
rx ch 11 14 B rssi -76: 41 88 9E FF FF FF FF EF BE 19 03 00 B4 0A   right
```

A second, unrelated and weak sender was also heard on channels 11, 12, 19 and 21: 13-byte broadcasts on PAN 0x1234 from short address 0x10AF, payload `E0 85 xx yy`. It is neither the remote nor openESL.

## Using it with openESL (not done)

Recognising a press is a few lines: a frame on channel 11 with source 0xBEEF and payload `19 cmd 00`, deduplicated on `seq`. The obstacle is the channel. The AP and the tags use channel 25, and the AP's radio listens on one channel at a time. Options, none chosen:

- a second ESP32-C6 (or H2) on channel 11 as a button receiver,
- the AP leaving channel 25 for short listens on channel 11 (tag polls arriving then would be missed),
- moving the whole openESL network to channel 11.

Whether the remote's channel can be changed (by pairing, say) is unknown. Since the tags only wake to poll, a press could only change what a tag shows at its next poll.

## How it was measured

- Sniffer build of the AP (`ESL_RADIO_SNIFFER`), first with `ESL_SNIFFER_HOP_MS=300` (hops through channels 11–26 and stays 10 s on a channel where it heard a frame) to find the channel, then with `ESL_CHANNEL=11` and no hopping to map the buttons. See [ap/README.md](../ap/README.md#configuration).
- The sniffer image was put on the AP with `esl-client ota`, so the AP's settings in NVS stayed. The sniffer never confirms itself (it has no WiFi or API), so the next reset rolls the AP back to its normal firmware. That includes a serial terminal that opens the USB port with DTR/RTS asserted (pyserial's default, `esl-client ota --wait`): open the port with DTR and RTS held low while the sniffer runs.
