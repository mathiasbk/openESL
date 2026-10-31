# Protocols

Two interfaces: **radio** between tag and AP, and **HTTP** from any client to the AP. Both are kept as small as possible.

## 1. Radio: tag ↔ AP (IEEE 802.15.4, 2.4 GHz)

### Basic setup
- Standard 802.15.4 data frame (frame type 001), 250 kbit/s O-QPSK. Both radios do this in hardware.
- One channel per AP, **channel 25** by default (2475 MHz, sits between WiFi channels 11 and 13 and is the least disturbed). The built-in one is set in menuconfig; the Settings page changes it while the AP runs and the tags follow (see "Changing the channel" below).
- Own PAN ID (proposal `0x4553`, "ES"). Choose one that is not used by nearby Zigbee/Thread.
- The tag sends from its 64-bit MAC (extended) to the AP's short address `0x0000`.
- The AP replies from `0x0000` to the tag's 64-bit MAC.
- PAN ID compression on. The MAC header is then 15 bytes in both directions, FCS 2 bytes, max 127 bytes total, so **max 110 bytes payload**.
- No MAC ACK (ACK request bit = 0). The reply from the AP *is* the acknowledgement. This saves ACK timing on the tag side in version 1.
- All multi-byte numbers are little-endian.

### Messages

All messages start with `type` (1 byte) and `proto_ver` (1 byte, = 1).

**POLL** (tag → AP), `type = 0x01`, 18 bytes (12 or 14 from older firmware, see below)
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x01, 0x01 |
| fw_ver | 2 | firmware version, major × 10000 + minor × 100 + patch (0.1.0 = 100, 1.2.3 = 10203; firmware before 0.1.0 sent 1) |
| battery_mv | 2 | battery voltage in mV, 0 = unknown (the 1.54" cannot measure it: its panel cannot be read and the ZBS243 ADC has no supply input, see kildelogg.md) |
| content_id | 4 | the `image_id` of the picture the tag shows now (0 = nothing, or one of its status screens) |
| flags | 1 | bit0 = just started, bit1 = previous drawing failed, bit3 = the tag takes firmware over the air, bit4 = it rejected a firmware image, or went back to its old firmware (see "Firmware transfer"). Bit4 stays on until the tag starts downloading another version, so the AP counts it against the version it offered only once the tag has asked for a block of it: before that, the offer may have been lost. bit5 = the tag shows images drawn by the AP (see "Image transfer"); the tag sets it when it has its SPI flash. bit6 = it also takes run-length coded images (IMG `fmt` 1), bit7 = it also takes images coded column against column (IMG `fmt` 2). bit2 is not used (firmware that drew content itself set it for a rejected layout file) |
| model | 1 | tag model (requirement T18): 1 = 2.9" (296×128), 2 = 1.54" (200×200), 0 = unknown. Was `reserved` (always 0), so old firmware reads as "unknown" |
| cfg_ver | 2 | bytes 12–13: version of the settings the tag uses now (0 = its built-in ones), see CONFIG. Sent only by firmware that understands CONFIG |
| rx_rssi | 1 | byte 14, the link block (see "Signal"): the tag radio's raw RSSI byte for the AP's last answer it took, 0 = none since it started. Not dBm: the scale is not measured yet, higher is stronger |
| rx_lqi | 1 | byte 15: the link quality (0–127) of that answer, 0 = none |
| lost | 1 | byte 16: poll rounds in a row without an answer before this one (255 = 255 or more) |
| link_flags | 1 | byte 17: bit0 = the tag's last answered round needed its second try. The retry itself is the same frame, byte for byte, so the flag goes in the next round's POLL |

The AP accepts a POLL of 12 bytes or more, and the length says what the firmware can take:

| Length | Firmware | The AP may send |
|---|---|---|
| 12–13 | old | IDLE, and IMG and FW when its flags say it takes them |
| 14–17 | 0.6 to 0.8 | also CONFIG |
| 18 or more | current (`tag/src`) | the same; the AP keeps its link block as the tag's signal history |

So older tags never get a message type they do not know. Firmware that drew content itself sent 18 bytes with its layout file version at 14–17; no tag runs it any more, and the AP would read those bytes as a link block. Fields added later go after byte 18, and the AP ignores bytes it does not know. CONFIG adds no flag.

**IDLE** (AP → tag), `type = 0x02`, 4 bytes: "nothing new"
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x02, 0x01 |
| next_poll_s | 2 | seconds until the next POLL (0 = keep) |

Types `0x03` (CONTENT), `0x04` (LAYOUT) and `0x05` (LAYOUT_REQ) carried the fields and the layout file to tags that drew their screens themselves, before the AP drew them. They are retired and not used again: the AP never sends them, and it does not answer them.

**CONFIG** (AP → tag), `type = 0x08`, 18 bytes: settings for this tag
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x08, 0x01 |
| next_poll_s | 2 | as in IDLE |
| cfg_ver | 2 | version of these settings; the tag sends it back in every POLL |
| poll_s | 2 | normal interval in seconds (T4) |
| max_poll_s | 2 | longest interval while the AP does not answer (T11) |
| lost_ap_s | 2 | seconds without an answer before the "no AP" mark, or "No AP found" (T9, #98); 65535 = never |
| long_poll_s | 2 | the interval once the AP has been gone for `long_after_min` (#261) |
| long_after_min | 2 | minutes without an answer before the tag polls only every `long_poll_s`; 65535 = never |
| channel | 2 | the AP's channel (11–26) the tag follows, 0 = the built-in one (25); see "Changing the channel" |

Every setting is u16, and 0 means the tag's built-in value (`tag/src/config.h`: 60 s, 600 s, 7200 s, 3600 s, 1440 min, channel 25). The tag raises a nonzero value below the minimum (10 s for the intervals, 60 s for `lost_ap_s` and `long_poll_s`, 10 min for `long_after_min`) to the minimum, and ignores a channel outside 11–26. New settings are only ever appended: the tag reads the fields it knows and ignores the rest, and a field a shorter CONFIG does not have is 0 (an AP before 0.9 sends 12 bytes, one with the long backoff but no channel 16). A CONFIG shorter than 12 bytes is ignored.

`cfg_ver` is 0 when all settings are 0, otherwise a nonzero 16-bit hash of the settings (CRC32 folded to 16 bits; of the first three settings only when neither the long backoff nor the channel is set, of the first five when only the long backoff is, which is what older APs sent, so an update sends no tag a CONFIG it already has). It is derived from the values, so the AP needs no counter and an AP restart sends nothing new. (A different set of settings with the same hash, 1 in 65535, would not be sent; `poll_s` still arrives through `next_poll_s`.)

**FW** (AP → tag), `type = 0x06`, 15 bytes + data. One block of a firmware image (docs/tag-ota.md)
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x06, 0x01 |
| next_poll_s | 2 | as in IDLE |
| fw_ver | 2 | version of the image, as `fw_ver` in POLL |
| size | 2 | size of the image in bytes, a whole number of 1 KB pages |
| pos | 2 | where in the image the data starts |
| crc | 4 | CRC-32 of the whole image |
| model | 1 | the tag model the image is built for |
| data | rest | bytes `pos .. pos + n` of the image, n ≤ 91 (`ESL_FW_CHUNK_MAX` = 110 − 15 − 4 for the MIC) |

**FW_REQ** (tag → AP), `type = 0x07`, 12 bytes
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x07, 0x01 |
| model | 1 | as in POLL |
| max | 1 | the most data bytes the tag wants in the answer (1..91) |
| fw_ver | 2 | the version the tag is downloading |
| pos | 2 | the first byte the tag is missing |
| crc | 4 | the CRC-32 of the image it is downloading |

**IMG** (AP → tag), `type = 0x09`, 14 bytes + data. One block of a screen image drawn by the AP (see "Image transfer")
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x09, 0x01 |
| next_poll_s | 2 | as in IDLE |
| image_id | 4 | CRC-32 of the `size` image bytes as sent; a CRC of 0 is sent as 1, so the id is never 0. The tag reports it as `content_id` once it shows the image |
| size | 2 | size of the image in bytes, as sent (a compressed image is shorter than the screen) |
| pos | 2 | where in the image the data starts |
| model | 1 | the tag model the image is drawn for |
| fmt | 1 | 0 = raw: 1 bit per pixel in screen coordinates, column by column as the tag's `draw_column()` gives it ((height + 7) / 8 bytes per column, byte 0 bit 7 = row 0 at the top, column 0 at the left first), the black plane and then, on models with red, the red plane. Bit 1 = ink in that plane, and a pixel is never set in both. The tag's display code rotates, transposes and inverts it for its panel (the 1.54" panel takes rows, rotated 180°). `size` is then exactly planes × width × ((height + 7) / 8): 4736 bytes on the 2.9", 5000 on the 1.54". 1 = run-length coded raw image, sent only to tags whose POLL has bit6: a control byte c, then for c < 0x80 the next c + 1 bytes as they are, and for c ≥ 0x80 the next byte repeated c − 0x80 + 2 times (2 to 129). Decoded it is exactly the raw image above; `size` is the coded length and `image_id` the CRC-32 of the coded bytes. 2 = the raw image coded column against column, sent only to tags whose POLL has bit7; see "The 2D image code" below. The AP sends a coded format only when it is shorter than the raw image, and of the two the shorter. Other values are reserved; a tag refuses a format it does not know |
| data | rest | bytes `pos .. pos + n` of the image, n ≤ 92 (`ESL_IMG_CHUNK_MAX` = 110 − 14 − 4 for the MIC) |

**IMG_REQ** (tag → AP), `type = 0x0A`, 10 bytes
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x0A, 0x01 |
| model | 1 | as in POLL |
| max | 1 | the most data bytes the tag wants in the answer (1..92) |
| image_id | 4 | the image the tag is downloading |
| pos | 2 | the first byte the tag is missing |

### Layout file on the AP (binary)
The AP compiles the JSON layout file (part 3) into this format and draws each tag's screen with it (`esl_render.c`). It never goes over the radio. Its CRC-32 is the file's version.

| Bytes | Content |
|---|---|
| 0 | format = 2 (1 had no icons, lines or open and rounded rectangles) |
| 1 | model (as in POLL) |
| 2 | number of layouts n, 1..16 |
| 3 | number of fixed texts t, 0..32 |
| 4 | number of icons i, 0..16 |
| then n times | number of elements (1 byte), then the elements, 14 bytes each |
| then t times | a fixed text: its length (1 byte, 1..64), then the Latin-1 text |
| then i times | an icon: width (2 bytes, 1..128), height (1 byte, 1..128), then the picture as a raw screen image (format 0 under "IMG"): width columns of (height + 7) / 8 bytes, bit 7 of byte 0 the top row, 1 = drawn |

Element, 14 bytes (`ESL_LE_*` in `shared/proto.h`):

| Offset | Bytes | Field |
|---|---|---|
| 0 | 1 | kind: 1 = text, 2 = rectangle, 3 = barcode, 4 = line, 5 = icon, 6 = QR code |
| 1 | 1 | text field 0..5 (text, barcode and QR code), or 0x80 + i for fixed text i of the file. Icon: the icon's number in the file, or 0x40 + f when text field f names the icon (`ESL_LE_ICON_FIELD`); then byte 2 is the fixed text holding icon 0's name, the names of all icons follow it in order, and the icon whose name equals the field's text is drawn centred in the box (nothing when none does or it is larger than the box) |
| 2 | 3 | fonts to try in order: 0 = small (17 px), 1 = medium (24 px), 2 = large (52 px, digits and ` ,-.:`), 3 = mid (36 px, same characters), 4 = text20 (20 px), 5 = price44 (44 px, same characters as large); 0xFF ends the list. Barcode: the first byte is the symbology (0 = Code 128, 1 = EAN-13), the other two 0xFF. QR code: the first byte is the error correction level (0 = L, 1 = M, 2 = Q, 3 = H), the other two 0xFF |
| 5 | 1 | alignment: bits 0–1 horizontal (0 left, 1 centre, 2 right), bits 2–3 vertical (0 top, 4 middle, 8 bottom). Rectangle: border width, 0 = filled. Line: bit 0 set = from the bottom-left to the top-right corner of the box, else from the top-left to the bottom-right |
| 6 | 1 | maximum lines in bits 0–5, 0 = as many as the box holds (at most 3); bits 6–7 turn a text element clockwise by 90° times their value (`ESL_LE_ROT`, 0 in older files). Barcode: module width in pixels 1..4, 0 = the widest that fits. QR code: module size in pixels 1..8, 0 = the largest that fits. Rectangle: corner radius. Line: width 1..16 |
| 7 | 1 | colour: 0 black, 1 white, 2 inverted (white text in a black box), 3 highlight (inverted if flag bit0, else black), 4 frame (black if flag bit1, else not drawn), 5 red (text or rectangle), 6 red inverted (white text in a red box). 5 and 6 only on a model with red (`colors` in `shared/models.json`) |
| 8 | 2 | x (pixels from the left) |
| 10 | 1 | y (pixels from the top) |
| 11 | 2 | w |
| 13 | 1 | h |

A **line** runs corner to corner in its box (a box 1 pixel high or wide is a straight line). A straight line is its box along it and exactly width pixels across it, (width − 1) / 2 of them above or left of the box, with square ends. A slanted line is every pixel whose centre is at most width / 2 from the segment, worked out in whole numbers so every platform draws the same pixels; its ends are round. A **rectangle** with a radius leaves out the pixels whose centre is outside the quarter circle in each corner; with a border, the same shape shrunk by the border (radius minus border) is left as it is inside. An **icon** draws its 1 bits in the element's colour and leaves the rest. Lines, rectangles and icons can be black, white or red (rectangles also frame).

How a text element is drawn (font fallback, wrapping, "…", inverted box) is in `ap/components/esl_core/esl_render.c`; the built-in layouts `price` and `text` are the same kind of element list. The content flags (bit0 highlighted price, bit1 frame) come from `price_highlight` and `frame` in `PUT /api/tags`.

A **barcode** element draws its field as black bars the full height of the box, placed by the horizontal alignment, with the quiet zones (10 modules each side for Code 128, 11 left and 7 right for EAN-13) inside the box. Its colour is always black (0). Code 128 takes ASCII 32–127; a field of only digits (2 or more) is packed two digits per symbol (code set C, an odd last digit in code set B). EAN-13 takes 12 digits (the AP adds the check digit) or 13 with a correct check digit. The AP refuses content whose barcode field cannot be encoded, or does not fit the box even with 1-pixel modules, in `PUT /api/tags` (part 2). At most one barcode per layout.

A **QR code** element (ISO/IEC 18004, byte mode, versions 1–10, the mask chosen by the standard's penalty rules; `esl_qr.c`, after Project Nayuki's QR Code generator library, MIT) draws its field as the smallest version that holds it at the element's error correction level, square in its box, placed by both alignments, with modules of the element's size or, at 0, the largest whole number of pixels that fits. The box also holds a quiet zone of 2 modules on every side (`ESL_QR_QUIET`), drawn white over whatever is under it, so phones read the code on a black band too. The code is black. The compiler works out the largest version the box holds with its quiet zone, and the AP refuses content whose QR field is longer than that version takes (`PUT /api/tags`, part 2). At most one QR code per layout.

How wide a module should be: the 2.9" has 0.23 mm pixels and the 1.54" 0.14 mm. EAN-13 is specified from 0.264 mm per module (80 %), so a 2-pixel module (0.45 mm) on the 2.9" is a safe choice; 1-pixel modules are below the standard and need a good scanner.

The **version** of a layout file is the CRC-32 (IEEE 802.3, the same as zlib) of the whole binary file; 0 is counted as 1. It changes exactly when what the tags show changes; a JSON file that only differs in whitespace, descriptions or layout names gives the same binary and keeps the version. A new version gives the tags with content from that file new images (their `image_id` changes).

The largest compiled file is **4096 bytes** (`ESL_LBIN_MAX`). That is about 290 elements, or for example 16 layouts of 9 elements and 1.9 KB of icons (a 64×64 icon is 515 bytes); 8 KB of JSON usually fills before it does. The AP refuses bigger files when they are uploaded.

These limits, and the per-layout ones below, belong to the AP, not the tag: the AP draws the image, and the tag only shows it. They are set per AP board (`"limits"` in `ap/boards/<board>/board.json`); the numbers here are the ESP32-C6's, the default and the smallest. Boards with PSRAM (`s3-n16r8-c6`, `c5-wroom-1u-n16r8`) allow 64 text lines, 40 rectangles, 32 lines and 16 icons per layout, so a file such an AP accepts can be refused by a C6. `GET /api/layouts` says which board an AP is and what it allows (`board`, `limits`), and the designer shows the limits of the AP it is connected to.

### Sequence
```
Tag                                      AP
 | -- wakes up, turns on radio ---------|
 | POLL(content_id=A) ------------------>|  looks up MAC in the table
 |<----------- IDLE, CONFIG, IMG or FW --|  within ~5 ms
 | listens for max 20 ms                 |
 | IMG or FW: asks for the rest (below)  |
 | radio off, sleeps                     |
```
- If the tag gets no reply, it tries once more after a random pause of 2–10 ms. Then it sleeps until the next interval (requirement T11: longer interval after several misses). After `lost_ap_s` without an answer it draws its image again from the slot with a small "no AP" mark (signal bars with a cross) in the top right corner, so the content stays up (#98); a tag with no image, or whose slot holds only part of a new one, shows "No AP found" with its MAC and firmware version instead (T9), as tag firmware before the mark always did. That is one refresh when the AP goes and one when it is back, none per miss. When the AP answers again the tag shows its image again without the mark (from the slot, no download, and it keeps reporting the image's id throughout), the new image if the AP sends one, or "Waiting for content" if it has none.
- The tag sends no separate acknowledgement. The next POLL carries the `image_id` it shows, and that is the acknowledgement. If an image does not arrive whole, the AP offers it again at the next POLL, and the tag goes on from where it stopped.
- Settings: when a tag sends a `cfg_ver` that differs from the version of the settings the AP has for it, the AP replies CONFIG first (see "Priority" below). The next POLL with the new `cfg_ver` is the acknowledgement. The tag keeps the settings in flash (T12), so after a restart it reports the same `cfg_ver` and needs no new CONFIG; a tag without a stored copy reports `cfg_ver = 0` and gets them again.
- When the AP has been gone for `long_after_min` (a day by default), the tag polls only every `long_poll_s` (an hour by default) instead of every `max_poll_s`, until it gets an answer: the first answer brings its normal interval back. The tag then finds a returned AP up to an hour late, but a tag on a shelf with its AP switched off lasts longer (`docs/strombudsjett.md`).
- `next_poll_s` in every reply is the tag's interval (its own `poll_s`, or the AP default). A tag that does not understand CONFIG still gets its interval this way; the other settings need CONFIG. When scheduled content for the tag is due before that, `next_poll_s` is the time until it is due plus 2 s instead; the next reply brings the normal interval back.
- Changing the channel: see below.
- Unknown MAC: the AP replies IDLE and adds the tag to the table with `content_id: 0`, so the client sees it in `GET /api/tags` and can give it content.

### Signal

Each POLL tells the AP how the link has been (#261): the link block at bytes 14–17 has the tag radio's RSSI and LQI of the AP's last answer, the rounds that went unanswered since, and a flag when the last answered round needed its second try. A second try that gets an answer means the AP's answer to the first was lost, or the first POLL never reached it; a round with no answer at all shows up as `lost` in the next POLL that gets through. The AP counts neither for the first POLL it hears from a tag after it started, since those rounds may have been lost while the AP itself restarted. Together with the RSSI the AP measures on each POLL, the AP keeps the last 8 POLLs per tag in RAM (32 on an AP with PSRAM), shown under `signal` in `GET /api/tags` and as small bars in the dashboard's Signal column. A tag that loses POLLs or needs its retry for a quarter of them, or one heard below about −85 dBm on average, is marked weak there before it drops out.

### Changing the channel

`PUT /api/config {"channel": 20}` (the Settings page, Radio) changes the AP's channel without leaving any tag behind. Only the channel changes this way: the PAN ID stays the one built in, since a tag that missed a new PAN could not find the AP again.

1. The AP keeps the planned move in NVS (`cfg/ch_next`) and puts the new channel in the `channel` field of every tag's CONFIG. That changes every tag's `cfg_ver`, so each tag gets a CONFIG at its next POLL, still on the old channel.
2. The tag keeps the new channel in flash with its other settings and goes on polling on the old channel. Its next POLL with the new `cfg_ver` is the acknowledgement, as for any setting.
3. The AP moves when every tag it has heard lately has acknowledged (heard within 3 of its intervals plus 60 s), or at the latest after twice the longest interval plus 60 s (at most 2 h 1 min). Only then does it keep the new channel in NVS (`cfg/channel`, and `cfg/ch_next` is erased): an AP that restarts during the wait comes up on the old channel, announces the move again and waits anew, counting every tag not heard since the restart as not acknowledged. `"channel_now": true` moves at once, but only to the channel of a move already announced or to the built-in one (else `409`): a tag that has not heard of a channel cannot follow it. Setting the channel the AP is on cancels a move that waits.
4. A tag that gets no answer to a POLL and its retry tries once on the channel from its last CONFIG, and from its third miss in a row also once on the built-in channel (25), each if it is another one. When the AP answers there, the tag stays on that channel and keeps it in flash; otherwise it goes back. So a tag that slept through the move, or acked just as the AP moved, finds it in the same round, and one whose AP went back to the built-in channel before the tag heard of it finds it in the third, for the cost of one or two extra POLLs while the AP is not heard. The built-in channel waits for the third miss since every AP answers every tag (one PAN, one short address): another AP on 25 would otherwise keep a tag that lost a single reply to noise.

Tags with firmware from before 0.9.0 store `cfg_ver` from every CONFIG, so they acknowledge a channel they cannot read. The AP tells them apart by the link block in POLL (18 bytes, the same release): while a tag heard lately sends a shorter POLL, a move away from the built-in channel answers `409` (`channel_old_tags` in `GET /api/config`), unless the request has `"force": true`. Since that is known only from a tag's POLL, the AP also answers `409` while tags in its table have not polled since it started (`channel_unheard_tags`, for 3 of their intervals and a minute at most), and a move that waits is called off (`channel_stopped`) when such a tag shows up before it happens. A radio over WiFi (docs/ap-netradio.md) from before 0.9.0 retunes when told but does not pass the channel on to the tags; it leaves out the features byte at the end of HELLO (bit 0 = passes a move on), and the AP refuses a move while it is connected (`channel_radio_old`). Back to the built-in channel is always allowed, since every tag finds it.

A tag that never got the CONFIG (off for longer than the wait) does not know the new channel; it finds the AP by looking on the other channels (below). After a restart the AP comes up on the channel in NVS; `GET /api/config` shows `channel`, `builtin_channel`, and while a move waits `channel_next`, `channel_waiting` (tags that have not acknowledged) and `channel_due_s`.

### Finding the AP

A tag whose AP answers on none of the channels above (it moved while the tag was off, or it came up on a channel the tag was never told of) looks for it (#258). The third round in a row without an answer sends its POLL once more, without the retry, on each of the first four channels of `CFG_SCAN_CHANNELS` (15, 20, 26, 11: with the built-in 25 the ones clear of WiFi 1, 6 and 11, then 11) that the round has not tried yet; every later round without an answer tries the next channel of the list only, which goes on through every channel the AP can be set to (12–14, 16–19, 21–24, 25) and starts over. Every channel of a round gets the same frame (the same sequence number), as a retry does. The tag stays on the first channel where an AP with its PAN ID answers, and keeps it in flash, so it starts there after a restart. The AP needs nothing for this. While the AP answers the tag does not look; while it is gone it costs one listening window more per round (`docs/strombudsjett.md`). Since the PAN ID and the AP's short address are the same for every openESL AP, a tag can find another AP this way; that AP takes it on like a new tag, the tag keeps the channel, and since the new AP answers, it never looks back. On a site with several APs on their own channels, the tags of an AP that is down for three rounds (30 minutes at `poll_s` 600, a power cut) can so end up with a neighbour, which has no content for them. Pairing the tags with their AP would fix this (#260, parked in #289): a tag would move only on an answer its own AP has signed.

### Firmware transfer (requirement T15)
The same pattern as the image transfer, with its own messages; the design, the bootloader and the rollback are in [tag-ota.md](tag-ota.md).
```
Tag                                          AP
 | POLL(fw_ver=A, flags bit3) --------------->|  AP has image B ≠ A for the model, nothing else waits
 |<----------------- FW(B, size, pos 0, 91 B) |
 | FW_REQ(B, pos 91) ------------------------>|
 |<---------------- FW(B, size, pos 91, ...)  |  ... up to 128 blocks per wake-up
 | (next wake-ups: POLL, FW from 0, the tag goes on where it stopped)
 | has `size` bytes, CRC-32 matches: installs and restarts
 | POLL(fw_ver=B, flags bit0) --------------->|  the new firmware confirms itself on this answer
```
- **When the AP sends it.** Only when it would otherwise answer IDLE (settings and the image first), the POLL has bit3 (the tag has the bootloader and a SPI flash), the AP has an image for the tag's model, and the tag reports another version. The AP stops offering a version once a POLL after the offer has bit4 (the API counts the tag as `failed`); a newer image is offered again.
- **Blocks.** As for IMG: the tag asks with FW_REQ for the next missing byte and keeps what it has between wake-ups. The AP answers a FW_REQ for a version or CRC it no longer has with block 0 of its current image, and with IDLE when it has none, which ends the transfer. The tag asks for at most 128 blocks (11 KB) per wake-up, so a 54 KB image takes five wake-ups. A block that is not the image's last carries `next_poll_s` = 5 (`ESL_BLOCKS_POLL_S`, or the tag's own interval when that is shorter), so a tag that stops part way comes back for the rest in seconds instead of after its whole interval; the last block, and IDLE, bring the normal interval back. The same goes for IMG blocks.
- **Check.** The tag refuses an image for another model, the version it runs, a size that is not whole 1 KB pages or larger than 60 KB, and a version it already rejected. When it has the whole image it reads it back from the SPI flash and checks the CRC-32; a mismatch is a rejection (bit4).
- **Install.** The tag copies its own application to the rollback slot, asks the bootloader to install the new one and restarts. The new firmware reports bit0 (just started) and its version; the first answered POLL confirms it. If it is started 9 times (resets, not wake-ups) without an answer from the AP, the bootloader puts the old firmware back, which then reports bit4.

### Image transfer
The AP draws the whole screen from the tag's content and its model's layout file, and the tag only stores and shows the picture. The tag draws nothing else itself but its start and status screens (`tag/src/status.c`).
```
Tag                                          AP
 | POLL(content_id=A, flags bit5) ----------->|  the AP wants image B ≠ A shown, nothing else waits
 |<------------- IMG(B, size, pos 0, 92 B)    |
 | IMG_REQ(B, pos 92) ----------------------->|
 |<------------- IMG(B, size, pos 92, ...)    |  ... 52 blocks for a raw 2.9" screen, 55 for a 1.54"; coded (bit7) a shelf screen takes 6
 | has `size` bytes: writes them to the panel
 | POLL(content_id=B) ----------------------->|  IDLE
```
- **When the AP sends it.** To a tag whose POLL has bit5, when the `content_id` it reports differs from the `image_id` of the picture the AP wants shown, after CONFIG and before FW (see "Priority"). A tag without bit5 (firmware that drew content itself, or a tag whose SPI flash does not answer) gets no content: the AP answers IDLE, and the API says so in `content_error`. The AP draws the image into one of two buffers (`ESL_IMAGE_BUFS`, `ap/components/esl_core/include/esl_image.h`) and keeps nothing else. Drawing a screen can take longer than the tag listens (20 ms), so the AP's own radio does not draw while the tag waits: a tag whose image is not in a buffer yet gets IDLE with `next_poll_s` = 2 (also on an IMG_REQ, when its buffer was taken or its content changed meanwhile), the AP draws it once that reply is out, and the next POLL gets block 0 at once (`esl_radio_set_draw_later`). A buffer is free again when its last block has gone out, when the tag reports the id, or 5 s after the last block the tag asked for (`ESL_IMAGE_HOLD_MS`). The AP counts both codes the tag takes (bit7: format 2, bit6: format 1) and sends the shorter one: format 2 on text and bars, format 1 on a dithered picture; a raw image goes out when neither is shorter. When both buffers are busy with other tags the AP answers IDLE with `next_poll_s` = 2 and sends block 0 at the next POLL; a tag that also waits for firmware gets the first FW block instead of that IDLE.
- **image_id.** The CRC-32 of the `size` image bytes as they are sent (for a compressed image, the compressed bytes), so the same layout and fields always give the same id, the tag can check the bytes straight from its SPI flash, and a picture the tag already shows is never sent again, also after an AP restart. The id is never 0 (0 in POLL means the tag shows nothing): a CRC of 0 is sent as 1, and the tag compares its CRC the same way.
- **Blocks.** As for FW: the tag asks with IMG_REQ for the next missing byte, keeps what it has in its SPI flash between wake-ups and asks for at most 64 blocks per wake-up (`CFG_IMG_MAX_REQ`), so a whole screen goes in one wake-up. The AP answers an IMG_REQ for an id it no longer has with block 0 of the picture it wants shown now, and with IDLE when it has none, which ends the transfer.
- **Check.** The tag refuses an image for another model, a format it does not know, a raw image whose `size` is not exactly its screen, a coded one that is not shorter than its screen or does not decode to exactly its screen, and an id it refused before. When it has `size` bytes it reads them back from the SPI flash and checks the CRC-32 against the id (a CRC of 0 counts as 1), decodes a coded image into a second area of the slot, writes the image to the panel, and reports the id in `content_id` from then on, also after a restart (the image stays in its flash slot, `tag/src/image.h`). The tag has one slot, and a new download overwrites it: a tag that restarts in the middle of a download shows "Waiting for content" until the download is done. A tag that refuses an image, or whose panel does not answer, reports bit1 (drawing failed) and keeps reporting the id it shows; the AP then stops offering that image until the content changes. An image the tag already holds (the same id again, for example after "No AP found") is drawn from the slot without a download.

#### The 2D image code (fmt 2)
Shelf labels are mostly white, with text, lines and bars, and a column of the image is nearly always like the column to its left. Format 2 codes each column against the one before, the way CCITT T.6 (fax "Group 4") codes a row against the row above: only where the edges of ink move is anything sent. The mode codes are T.6's; the run lengths use Exp-Golomb codes instead of T.6's tables, which keeps the decoder small. The coder is `ap/components/esl_core/esl_img2d.c`, the decoder `decode_2d()` in `tag/src/image.c`.

- The code is a bit string, most significant bit of each byte first. It codes the columns of the raw image (format 0) in order, the red plane's after the black plane's, with nothing between them; each column of `height` pixels is coded against the column before it, the first against an empty one. After the last column the last byte is filled with zero bits, and nothing follows.
- Ink is colour 1. A *change* in a column is a row y whose pixel differs from row y − 1; row −1 has no ink.
- Coding a column starts at a0 = −1 with colour 0 and goes down until a0 is the height. At each step, s = max(a0, 0), b1 is the first change in the reference column (the one before) below a0 to the other colour than the current one, and b2 the change after b1 (the height when there is none):

| Code | Mode | Decoded |
|---|---|---|
| `1` | vertical 0 | the next change a1 is at b1: rows s to a1 − 1 get the current colour, the colour flips, a0 = a1 |
| `011`, `010` | vertical ±1 | as vertical 0 with a1 = b1 + 1, b1 − 1 |
| `000011`, `000010` | vertical ±2 | a1 = b1 + 2, b1 − 2 |
| `0000011`, `0000010` | vertical ±3 | a1 = b1 + 3, b1 − 3 |
| `0001` | pass | rows s to b2 − 1 get the current colour, a0 = b2, the colour stays |
| `001` r1 r2 | horizontal | r1 rows of the current colour from s, then r2 rows of the other colour; a0 = s + r1 + r2, the colour stays. r1 and r2 are Exp-Golomb codes of order 2: r + 4 in binary (b bits), after b − 3 zero bits (0 is `100`, 4 is `01000`) |

- The tag refuses a code that goes past the height of a column (or has a1 at or above a0), ends in the middle of a column, has bytes or one bits after the last column, or does not give exactly the raw size.
- What it gives, on the 405 different screens the simulator draws from the example layouts (`sim/`, scenario 0): on average 509 bytes (6 blocks) on the 2.9" and 490 (6 blocks) on the 1.54", against 2255 and 2156 bytes (25 and 24 blocks) run-length coded and 52 and 55 blocks raw; the largest takes 12 blocks. A screen that format 1 codes shorter (a dithered photo) goes out run-length coded, and one neither code makes shorter goes out raw.
- Cost on the tag: about 1.6 KB of code, 280 bytes of external RAM on the 2.9" (430 on the 1.54": two columns as lists of changes) and 14 bytes of internal RAM for the decoder's state. The image is decoded once, from the download area of the slot to the second area, before its header is written. Decoding takes the 8051 longer than run-length decoding, but the tag also runs the CRC-32 over the coded bytes, and that is a third as many: in the 8051 simulator that comes with SDCC (ucsim), CRC and decoding together take about as long as for the same screen run-length coded (12.6 against 11.4 million clock ticks for a shelf screen of 537 bytes, 19.1 against 19.0 for the largest, 1006 bytes), and the tag receives a third of the blocks.

### Priority: settings, image, firmware
The AP answers each POLL with one message. When several things are waiting for a tag, it sends them in this order:

1. **CONFIG**: when the tag sends `cfg_ver` and it differs from the version of its settings on the AP.
2. **IMG**: the picture the tag should show, when its POLL has bit5 and its `content_id` differs from the `image_id` of the AP's picture (see "Image transfer"). Content the AP cannot draw for the tag (a layout that is not in its model's file, or a tag without bit5) is held back (`content_error` in the API), and the AP goes on to 3.
3. **FW**: new firmware for the tag (see "Firmware transfer").
4. **IDLE**.

The next item goes out at the next POLL. `next_poll_s` in every reply, IMG and FW blocks included, is the tag's own interval (its `poll_s`, or the AP default).

### Listening window and battery
The tag is awake for about 5–25 ms per round. Adjust the window from measurements in phases 5–6; it is the most important power parameter after sleep current.

## 2. HTTP: client → AP (push)

Any software (the client) calls a small REST API on the AP when content changes. The AP never calls the client.

**The reference is [openapi.yaml](openapi.yaml)** (OpenAPI 3.1): every endpoint, field, limit and error code, with examples. The API tab of the web page shows it with test requests (https://mathiasbk.github.io/openESL/#api), and `ap/test/openapi/check_openapi.py` runs its examples against the AP code (`esl_api.c`) and checks every reply against it, so the spec and the code cannot drift apart. Change the spec together with the code.

In short:
- **Reaching the AP:** HTTP on the local network or a VPN (`openesl.local`, or a fixed IP), or the same API over the AP's USB port as `@ESL` lines, with no token (`ap/README.md`, "Over the USB port"). `tools/esl-client` speaks both, and `esl_client.py serve` is a bridge on localhost that lets the web page send test requests.
- **Authentication:** `Authorization: Bearer <token>`; the token is set in the AP (the setup page at `/setup`, `PUT /api/wifi` with `"token"`, also over the USB port right after flashing, menuconfig or NVS). Without a valid token: `401`; after five wrong tokens in a row, `429` for every request for 60 s, so the token cannot be guessed at speed. A read-only status page at `/` needs no token (requirement A6).
- **Errors:** `{"error":"<what is wrong>"}`, in a batch with `"index"` for the row.

| Request | What it does |
|---|---|
| `PUT /api/tags/{mac}` | Set the content of one tag: `{"layout":"price","fields":["Milk 1 L","24.90","NOK 24.90/L"],"price_highlight":true}`. The AP calculates `content_id` (CRC-32); the same content again is `changed:false` and the tag does not draw again |
| `PUT /api/tags` | The same for up to 50 tags, all or nothing |
| `"at"` in either | Scheduled content: `"at":1792069200` (Unix seconds, UTC) sets the content at that time instead of now, also after an AP restart |
| `GET`/`DELETE /api/schedule`, `DELETE /api/schedule/{mac}` | What is scheduled, with its content; remove it |
| `GET`/`PUT /api/time` | The AP's clock (NTP over WiFi, or set by the client), which only scheduled content uses |
| `GET /api/tags`, `GET /api/tags/{mac}` | The AP and its tags: last seen, battery, signal, model, what each tag shows, settings, firmware updates |
| `DELETE /api/tags/{mac}` | Forget a tag: its content, settings and scheduled content |
| `GET /api/tags/{mac}/image` | The screen the AP draws for the tag's content (base64), for previews |
| `PUT /api/tags/{mac}/image` | A picture as the tag's content, for what a layout cannot show: a PNG of the screen's size (296 x 128 on the 2.9"), which the AP turns into the tag's colours (nearest colour, or `?dither=true` for photos), keeps in NVS and sends like any other screen. The tag must have polled once |
| `GET`/`PUT /api/tags/{mac}/config` | Poll interval, longest interval and "No AP found" time for one tag |
| `GET`/`PUT /api/config` | The AP-wide default poll interval, the webhook the AP posts the health report to, the radio's transmit power (`tx_power`, dBm), and the clock (`ntp`, `tz`) |
| `GET /api/health` | The tags that need attention (silent, never seen, drawing failed, battery low, content that cannot be shown), for monitoring |
| `GET /api/layouts`, `GET`/`PUT`/`DELETE /api/layouts/{model}` | The layout file of each model (part 3) |
| `GET`/`POST /api/ota` | New firmware for the AP itself: the app image, whole or in parts; the AP restarts into it (`esl-client ota`) |
| `GET`/`PUT /api/tag-firmware`, `GET`/`DELETE /api/tag-firmware/{model}` | New firmware for the tags of a model, in parts; the AP sends it to them over the radio (part 1, "Firmware transfer"), and shows how far they are (`esl-client tag-ota`) |
| `GET`/`PUT /api/wifi` | The WiFi network the AP joins (`{"ssid":"…","password":"…"}`, never shown back), its state and the last scan |
| `GET`/`PUT /api/sources`, `POST /api/sources/refresh` | Data sources (ESP32-S3 APs): content the AP fetches for itself, such as the weather from MET Norway, and gives to its tags ([data-sources.md](data-sources.md)) |

**WiFi setup:** with no network saved, the AP opens its own network `openesl-XXXX` with a form at `http://192.168.4.1/setup` (`/` redirects there, and a phone that joins is sent to the form by the captive portal) to pick the network and set the token. If the saved network gives no IP for 180 s (`ESL_WIFI_SETUP_AFTER_S`), the setup network opens again next to it, and it closes a minute after the AP has joined. Once a token is set, the form asks for it, with the same lockout as the API after five wrong ones. Details: `ap/README.md`, "WiFi setup".

## 3. Layout files

Requirements G6, G7, T17, T18, A12, A13, A14. The design tool (the Design tab of the web page in `web/`, or the file written by hand) makes **one layout file per tag model**. It holds up to 16 named layouts. A client uploads it with `PUT /api/layouts/{model}` (part 2), chooses a layout per tag by name in `PUT /api/tags` (requirement A14), and the AP draws each tag's screen with it and sends the picture (part 1, "Image transfer"). The file never goes to the tags. The built-in layouts `price` and `text` are in the AP firmware and can always be used, with or without a file. Examples: `tag/designs/layouts/2.9.json` and `1.54.json`; the Design tab previews them with the AP's own drawing code.

```json
{
  "format": "esl-layouts",
  "version": 1,
  "model": "2.9",
  "description": "optional",
  "layouts": [
    {
      "name": "shelf",
      "fields": ["name", "price", "unit", "info"],
      "elements": [
        { "type": "rect", "x": 0, "y": 0, "w": 296, "h": 30, "color": "black" },
        { "type": "text", "field": "name", "x": 4, "y": 0, "w": 288, "h": 30,
          "fonts": ["medium", "small"], "align": "center", "valign": "middle", "lines": 1, "color": "white" },
        { "type": "text", "field": "price", "x": 120, "y": 32, "w": 171, "h": 74,
          "fonts": ["large", "mid", "medium"], "align": "right", "valign": "bottom", "color": "highlight" }
      ]
    }
  ]
}
```

| Key | Rule |
|---|---|
| `format`, `version` | `"esl-layouts"`, `1` |
| `icons` | optional: up to 16 small pictures by name (`a-z 0-9 _`, starting with a letter, at most 16), each `{"w": 32, "h": 32, "data": "<base64>"}`: w and h 1–128 px, data the picture row by row from the top, (w + 7) / 8 bytes per row, bit 7 of a row's first byte its leftmost pixel, 1 = drawn (`\/` for `/` is fine). The Design tab makes them from a PNG, JPEG, GIF or SVG (an SVG is drawn 64 px on its longer side) |
| `model` | `"2.9"` or `"1.54"`, and the same as `{model}` in the URL. Positions are checked against that screen: 296×128 or 200×200 |
| `layouts` | 1 to 16 layouts. Layout n in this list is layout n in the compiled file; clients only see the names |
| `name` | 1–31 of `a-z 0-9 . _ -`, starting with a letter or digit, unique in the file, not `price` or `text` |
| `fields` | 1–6 field names (`a-z 0-9 _`, starting with a letter, at most 16), the order clients send `fields` in |
| `elements` | 1 or more, drawn in order (all rectangles, then lines, then icons, then the barcode and QR code, then all text, see `esl_render.c`) |
| `type` | `"text"` (one field in a box), `"rect"` (a box, filled or only its border), `"barcode"` (one field as a barcode), `"line"`, `"icon"` or `"qr"` (one field as a QR code) |
| `x`, `y`, `w`, `h` | whole pixels; `w`, `h` ≥ 1; the box must be inside the screen. An icon with `icon` takes only `x` and `y` (or `w` and `h` equal to the icon's); one with `field` needs all four. A line runs corner to corner in its box: `h` 1 is a horizontal line, `w` 1 a vertical one |
| `direction` | line only: `down` (default, top-left to bottom-right corner) or `up` (bottom-left to top-right) |
| `width` | line only: 1–16 px, default 1. A straight line is exactly `width` px thick, (width − 1) / 2 of them above or left of its box, with square ends; a slanted one reaches width / 2 past its box and its ends are round |
| `radius`, `border` | rect only: corner radius, and 0 (default) for a filled box or the width of its border; each at most half the shorter side |
| `icon` | icon only: a name from `icons`; give this or `field` |
| `field` | text, barcode and qr: a name from `fields` (or its index). Icon, instead of `icon`: the field whose text is the name of the icon to draw, so the content picks it (a weather symbol); the icon is centred in the box, and nothing is drawn when the text names no icon or the icon is larger than the box. The names are stored once per file as fixed texts |
| `text` | text only, instead of `field`: a fixed text such as `"inkl. mva"` or `"kr"`, stored once in the file (at most 64 bytes in Latin-1, 32 different texts per file), so it costs nothing in the content. It may hold the date fields `{weekday}` (Thursday), `{date}` (29 October 2026), `{date_short}` (29.10.2026) and `{week}` (44, ISO 8601), which the AP fills in with its local date when it draws (in the time zone, `tz` in `PUT /api/config`, UTC until one is set; while the AP does not know the time, after a restart until NTP answers, such a layout is not drawn and its tags keep what they show); a tag with such a layout is drawn again when the date changes, once a day, or once a week for `{week}` alone, and gets the new image only when it came out different. At most 4 such texts per layout. Any other word in braces is drawn as written, and so are these on an AP from before 0.9 |
| `symbology` | barcode only, required: `"code128"` or `"ean13"` |
| `module` | barcode: module width 1–4 px; qr: module size 1–8 px; 0 (default) = the largest that fits the box |
| `ecc` | qr only: error correction `L`, `M` (default), `Q` or `H` (about 7, 15, 25 or 30 % of the code can be lost); higher takes more modules for the same text |
| `fonts` | text only: 1–3 of `small` (17 px), `medium` (24 px), `text20` (20 px), `mid` (36 px), `price44` (44 px), `large` (52 px); `mid`, `price44` and `large` only have digits and ` ,-.:`. Tried in order; the first in which the text fits is used, otherwise the last one with "…" |
| `align`, `valign` | text and qr (`align` also for barcodes): `left`/`center`/`right` (default `left`), `top`/`middle`/`bottom` (default `top`; `bottom` keeps one baseline for every font in the list) |
| `lines` | text only: 0–3, 0 (default) = as many as the box holds |
| `rotate` | text only: `0` (default), `90`, `180` or `270`, degrees clockwise. The box keeps its place on the screen and the text is laid out in it turned: at `90` (reads top to bottom) and `270` (bottom to top) the lines run along `h` and are stacked across `w`, so `w` must hold the smallest font, and `align`, `valign` and `lines` work along the turned box (`left` is the top at `90`, the bottom at `270`). Inverted text gets its box turned too. Needs an AP newer than 0.9.2; an older one refuses the layout file |
| `decimals` | text only: draws a number (e.g. a price) the same way whatever the client sends (`1299.9`, `1299,90`, `1 299,90`, `1.299,90`, `199,-`): `2` → `1299,90`, `0` → `1300` (rounded), `"auto"` → `199` or `199,90` (no decimals when they are `,00`), `"dash"` → `199,-` or `199,90`, `"0-dash"` → `200,-` (rounded to the nearest krone, always `,-`; `199.49` shows as `199,-`, so use it when your prices are whole kroner, since the shelf price must match the till; needs an AP on 0.9.0 or newer, an older one refuses the layout file), `"keep"` → the decimals as sent, with a decimal comma. A dot is the decimal sign when there is no comma and only one dot, so `1.299` is 1,299; send `1299` or `1 299` for a thousand. Spaces and dots before the decimal sign must group three digits. Text that is not a number is drawn as sent. A formatted number is drawn on one line (a smaller font from `fonts` if it does not fit), and never cut short: content with a number that fits in none of the fonts is refused with `422`, and the AP leaves such a number out rather than draw part of it (when a newer layout file makes the box smaller). At most 8 elements per layout with `decimals` or `thousands`. Default: no format, the text as sent |
| `thousands` | text only: `true` puts a space between every three digits, `1 299,90` (with no `decimals`, the decimals as sent). Default `false` |
| `color` | `black` (default), `white`, `frame` (only drawn with `"frame": true`; not lines and icons); text also `invert` (white text in a black box) and `highlight` (inverted with `"price_highlight": true`, else black). On a model with red: `red` (text, rect, line and icon) and `red_invert` (text: white text in a red box) |
| keys starting with `_`, `description` | ignored (comments) |

The AP also checks what it can draw, so a file that is accepted always gives a screen (requirement A13):
- a text box must be at least as high as its smallest font (plus 2 × 4 px for `invert`/`highlight`/`red_invert`), or as wide when it is turned 90° or 270°;
- a barcode is black, at least 10 px high, one per layout, and an EAN-13 box must hold its 113 modules (95 plus the quiet zones) at the chosen module width; content whose barcode field cannot be encoded or is too wide for the box is refused with `422` when it is sent;
- a QR code is black, one per layout, its box at least 25 modules wide and high (21 and the quiet zone); content whose QR field is longer than the largest version the box holds takes is refused with `422`;
- per layout at most 32 text lines (each text element counts the most lines it can use), 20 rectangles (`rect` elements plus `invert`/`highlight` text), 16 lines and 8 icons (`ESL_DRAW_MAX_RUNS`, `ESL_DRAW_MAX_RECTS`, `ESL_DRAW_MAX_LINES`, `ESL_DRAW_MAX_ICONS`) on the ESP32-C6; other boards may allow more (above);
- the binary file is at most 4096 bytes (on every board so far);
- unknown keys, a key given twice and anything else above give `422` with the place in the file.

`tag/tools/label_design.py` designs content for these layouts too: a label design with `"layout": "shelf"` takes its field names from the layout file (`render <design> <data> designs/layouts/2.9.json`).
