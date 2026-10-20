# AP with a separate radio chip (ESP32-S3 + ESP32-C6)

The AP normally runs on one ESP32-C6, which does WiFi and 802.15.4 on the same radio by
time-division and has 512 KB of RAM. For more tags, the AP can instead run on an
ESP32-S3 with PSRAM, with an ESP32-C6 next to it as a pure 802.15.4 radio
([issue #146](https://github.com/mathiasbk/openESL/issues/146), risiko.md R4).

| Chip | Firmware | Does |
|---|---|---|
| ESP32-S3 N16R8 (host) | AP firmware, board `s3-n16r8-c6` (`openesl-ap-esp32s3-n16r8-<version>.bin`) | WiFi, HTTP API, dashboard, tag store, rendering, `esl_radio_handle()`: everything the C6 AP does, up to 1000 tags |
| ESP32-C6 (radio) | `ap/radio/` (`openesl-ap-radio-esp32c6-<version>.bin`) | 802.15.4 only, no WiFi: forwards every frame to the host and sends the replies |

Status: experimental. Both images build; it has not been tried on hardware yet.

## Wiring

| ESP32-S3 | ESP32-C6 (Super Mini) |
|---|---|
| GPIO17 (TX) | GPIO17 (RX) |
| GPIO18 (RX) | GPIO16 (TX) |
| GND | GND |
| 5V or 3V3 | 5V or 3V3 |

The pins can be changed in menuconfig: `ESL_COPROC_TX_GPIO`/`ESL_COPROC_RX_GPIO` on both sides.
Both chips keep their USB port for flashing and the log.

## Flashing

1. The C6: `openesl-ap-radio-esp32c6-<version>.bin` at 0 (`esptool --chip esp32c6 write_flash 0 ...`).
   Its log says `OpenESL 802.15.4 radio`, and once the host runs, `channel .., PAN ..`.
2. The S3: `openesl-ap-esp32s3-n16r8-<version>.bin` at 0. Its log says `radio 1.0 up: 802.15.4 AP on
   channel ..` when it reaches the C6, and `no answer from the radio` every two seconds when it
   does not (check TX/RX crossed and GND).

The web flasher does not offer these images yet, and the S3 cannot update the C6 yet; both are
later steps in the issue.

## The link

UART at 921600 baud, 8N1, no flow control. Code: `ap/components/esl_coproc` (tested on the PC
in `ap/test/test_coproc.c`), used by `ap/main/radio_uart.c` on the host and `ap/radio/` on the C6.

Every message is one frame: `A5 type len payload[len] crc16` (CRC-16/CCITT-FALSE over type, len
and payload, little-endian). The receiver hunts for `A5`, so the C6's boot text or a frame cut
by a reset costs only the frames it overlaps.

| Type | Direction | Payload | Meaning |
|---|---|---|---|
| `01` PING | host → radio | none | answer with HELLO |
| `02` CONFIG | host → radio | channel, PAN, short address, TX power, flags (sniffer) | start receiving with these; answered with HELLO |
| `03` TX | host → radio | ref, PSDU | send this frame (the reply to RX `ref`) |
| `04` STATS? | host → radio | none | answer with STATS |
| `81` HELLO | radio → host | link version, configured, firmware version, IEEE address, settings | at boot and after PING/CONFIG |
| `82` RX | radio → host | ref, RSSI, channel, PSDU | a received frame |
| `83` TX_DONE | radio → host | ref, status, resends, µs from RX to first transmit | how the TX went |
| `84` STATS | radio → host | counters | for the host's log every minute |

Timing: the tag listens 20 ms after its POLL. The RX for a POLL is about 40 bytes on the wire
(0.4 ms) and the reply at most 133 (1.5 ms), so a reply leaves the C6 2–3 ms after the POLL
arrived. The C6 times its resends from when it received the tag's frame (`ref`), as
`radio_ieee802154.c` does on the C6 alone, and does not send a reply that comes after 12 ms
(TX_DONE status LATE).

A radio that restarts says HELLO with `configured = 0`, and the host sends CONFIG again.
