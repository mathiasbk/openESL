# How a tag behaves on the radio

What a tag with openESL firmware does on the air, round by round, with the numbers from the code. It is for anyone who debugs an AP, writes a new one, or reads a sniffer log ([radio-sniffer.md](radio-sniffer.md)) and wants to know whether what they see is right. The messages themselves are in [protocol.md](protocol.md), part 1; this page is about timing, channels and what the tag accepts.

The numbers are the built-in defaults in `tag/src/config.h`. The AP can change the intervals per tag with CONFIG (`PUT /api/tags/{mac}/config`); the channel list, the listening window and the retry pause are fixed in the firmware.

## One round

```
tag wakes up (sleep timer, 1 s ticks)
 ├─ radio on, POLL on its channel            ~1.4 ms on the air (39 bytes)
 ├─ listens about 20 ms for the answer
 ├─ nothing: random pause of 2-10 ms, the same POLL again, listens about 20 ms
 ├─ still nothing: tries other channels (below)
 ├─ IMG or FW answer: asks for the next blocks in the same wake-up
 └─ radio off, sleeps next_poll_s (or longer after misses, below)
```

- **The listening window** is `CFG_RX_TIMEOUT_MS`, 20 ms, counted from the end of the tag's own frame (`exchange()` in `tag/src/app.c`). The radio is in receive mode as soon as the frame is out. A frame that arrives but is not the answer (another tag's POLL, a frame for another tag, a frame with a wrong FCS) starts the 20 ms again, so on a busy channel the window stretches; the tag reads at most three frames with a correct FCS per window (`RX_TRIES`). An AP should have the whole answer on the air well inside 20 ms: the openESL AP aims for 5 ms from the POLL's arrival to its answer being sent (requirement A3), and resends a reply the radio refused only while it would still land in the window (`TX_RETRY_US`, 12 ms, in `ap/main/radio_ieee802154.c`).
- **Air time** is 32 µs per byte at 250 kbit/s, plus 6 bytes of preamble, start and length, so a full 127-byte frame takes about 4.3 ms and a 22-byte POLL payload in a 39-byte frame about 1.4 ms.
- **The retry** comes after the first window and a random pause of 2 to 10 ms (`CFG_RETRY_MIN_MS`, `CFG_RETRY_MAX_MS`). It is the same frame byte for byte, sequence number included, so an AP or a sniffer sees two POLLs with one sequence number about 25 to 33 ms apart. The next POLL that gets an answer tells the AP the round needed its retry (link flag `RETRY`).
- **Blocks:** after an IMG or FW answer the tag asks for the next block straight away (IMG_REQ, FW_REQ), each with the same window and one retry, up to 64 image blocks (128 on a model with red) or 128 firmware blocks per wake-up, and gives up the wake-up at the 9th request that gets no answer (`CFG_FW_MAX_MISS`, 8, counted over the whole wake-up, not only in a row).
- **No MAC ACKs.** The tag never sets the ACK request bit and never sends an ACK; the AP's answer is the acknowledgement. An AP must not set the ACK request bit either (below).

## What the tag accepts as an answer

The tag drops every frame that is not exactly the form openESL's AP sends (`proto_check_frame()` in `tag/src/proto.c`):

| Field | Must be |
|---|---|
| FCS | correct (the radio checks it and drops the frame otherwise) |
| Frame control | `0x8C41` exactly: data frame, no security, no frame pending, **no ACK request**, PAN ID compression, frame version 0, destination extended, source short. Bytes `41 8C` on the air |
| Destination PAN | the tag's PAN (`0x4553`, bytes `53 45`) |
| Destination address | the tag's own 8-byte address, least significant byte first: tag `0000021234567890` is `90 78 56 34 12 02 00 00` on the air |
| Source address | short `0x0000` |
| Length | 15-byte header, then the payload; at most 127 bytes with the FCS |
| Payload | a message the tag knows, with `proto_ver` 1 and the length that message needs (`proto_parse_reply()`) |

A frame with any other frame control, an ACK request bit included, is dropped as if it was never sent. So is a frame with the address bytes in the wrong order. When an AP's answers go out but the tag keeps polling as if it heard nothing, compare the AP's frame byte by byte with this table; a sniffer shows the frame as it went out ([radio-sniffer.md](radio-sniffer.md)).

## Channels

The tag starts on the channel it last heard its AP on, kept in its flash with its settings; a tag fresh from the jig starts on the built-in channel 25. A round with no answer on that channel goes on in this order (`try_announced()` and `scan()` in `tag/src/app.c`):

1. **The announced channel**: the channel from the tag's last CONFIG (the AP announces a move there, [protocol.md, "Changing the channel"](protocol.md#changing-the-channel)), if it is another one. One POLL with its retry.
2. **The built-in channel 25**, from the third round in a row without an answer, if it is another one. One POLL with its retry. It waits for the third round so that a neighbouring AP on 25 does not take a tag that only lost one answer to noise.
3. **The scan**, also from the third round in a row: in the third round once on each of the first four channels of `CFG_SCAN_CHANNELS` that the round has not tried yet, **15, 20, 26 and 11** in that order; in every later round without an answer only the next channel of the list: 12, 13, 14, 16, 17, 18, 19, 21, 22, 23, 24, 25, then from 15 again. A channel the round has tried already is passed over for the next one. One POLL on each, without the retry.

The tag stays on the first channel where an AP answers, and keeps it in flash. Every channel tried in a round gets the same frame as the first POLL, sequence number included. Switching channel costs the tag a few milliseconds (radio off and on), so in the scan the POLLs come about 25 to 40 ms apart: the window plus the switch.

So a tag whose AP disappears is seen by a sniffer like this, on 60 s intervals:

| Round | Sleep before it | Channels tried |
|---|---|---|
| 1st miss | – | own (POLL + retry), announced if another |
| 2nd miss | 60 s | the same |
| 3rd miss | 60 s | own, announced, 25, then 15, 20, 26, 11 (one POLL each) |
| 4th miss | 120 s | own, announced, 25, then 12 |
| 5th miss | 240 s | own, announced, 25, then 13 |
| ... | doubles, up to 600 s | one new channel per round |

## How often it polls

- **Normally** the tag sleeps `next_poll_s` from the AP's last answer: the tag's own `poll_s` (60 s by default), 5 s between the wake-ups of a block download, or 2 s while the AP draws its image.
- **After misses** (`app_backoff_s()`): the first two rounds without an answer keep the interval, and from the third the interval doubles every round, up to `max_poll_s` (600 s): 60, 60, 120, 240, 480, 600, 600 ... The interval it doubles is the last one it had, so a tag that lost the AP during a block download (5 s) comes back after 5, 5, 10, 20 ... s.
- **After `lost_ap_s`** without an answer (2 hours) the tag draws its picture again with the "no AP" mark. It wakes at that time even when the backoff would sleep past it.
- **After `long_after_min`** (a day) it polls only every `long_poll_s` (an hour) until it gets an answer.
- **The first answer** brings the normal interval back, and the miss count, the lost time and the scan start over.

## Restarts and battery pulls

A tag keeps its picture, its settings, its channel and its AP id in flash, and its miss count, interval, lost time and place in the scan in RAM. Its RAM survives the deep sleep between rounds (`power_sleep_s()` in `tag/src/hal/zbs243/hal_zbs243.c`); only a real restart clears it. After a restart the tag shows its stored picture (or the start screen), then polls at once on its stored channel, with the `booted` flag (bit 0 of the POLL flags) set until it gets an answer. That flag is how to tell a restart: the AP gets it in the POLL, and the sniffer shows it as `[booted]`.

Asleep, a tag draws so little that the capacitors on its board keep it alive for a while without a battery. On SoluM 2.9" tags it took about a minute out of the holder before the tag really restarted; a tag whose battery was out for a few seconds goes on where it was, with its miss count and its long interval. To restart a tag on purpose, leave the battery out for a minute or more, then check for `booted` in its first POLL.

The watchdog restarts a tag that hangs for 60 s (requirement T10). A tag that restarts 9 times without an answer from the AP after a firmware update goes back to its old firmware ([tag-ota.md](tag-ota.md)).
