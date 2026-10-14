# Protocols

Two interfaces: **radio** between tag and AP, and **HTTP** from any client to the AP. Both are kept as small as possible.

## 1. Radio: tag ↔ AP (IEEE 802.15.4, 2.4 GHz)

### Basic setup
- Standard 802.15.4 data frame (frame type 001), 250 kbit/s O-QPSK. Both radios do this in hardware.
- Fixed channel (proposal: **channel 25**, 2475 MHz, sits between WiFi channels 11 and 13 and is the least disturbed). Configurable.
- Own PAN ID (proposal `0x4553`, "ES"). Choose one that is not used by nearby Zigbee/Thread.
- The tag sends from its 64-bit MAC (extended) to the AP's short address `0x0000`.
- The AP replies from `0x0000` to the tag's 64-bit MAC.
- PAN ID compression on. The MAC header is then 15 bytes in both directions, FCS 2 bytes, max 127 bytes total, so **max 110 bytes payload**.
- No MAC ACK (ACK request bit = 0). The reply from the AP *is* the acknowledgement. This saves ACK timing on the tag side in version 1.
- All multi-byte numbers are little-endian.

### Messages

All messages start with `type` (1 byte) and `proto_ver` (1 byte, = 1).

**POLL** (tag → AP), `type = 0x01`, 12 bytes
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x01, 0x01 |
| fw_ver | 2 | firmware version |
| battery_mv | 2 | battery voltage in mV |
| content_id | 4 | ID of the content the tag shows now (0 = nothing) |
| flags | 1 | bit0 = just started, bit1 = previous drawing failed |
| reserved | 1 | 0 |

**IDLE** (AP → tag), `type = 0x02`, 4 bytes: "nothing new"
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x02, 0x01 |
| next_poll_s | 2 | seconds until the next POLL (0 = keep) |

**CONTENT** (AP → tag), `type = 0x03`, max 110 bytes
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x03, 0x01 |
| next_poll_s | 2 | as in IDLE |
| content_id | 4 | new ID |
| layout | 1 | 1 = Price, 2 = Text |
| flags | 1 | bit0 = red price, bit1 = red frame/stripe |
| fields | rest | up to 4 text fields, each as `length (1 byte)` + Latin-1 text |

Field meaning: layout 1 = `[name, price, subtitle]`; layout 2 = `[line1, line2, line3, line4]`. The text is at most about 96 bytes in total. Reserve 4 bytes for a future MIC (requirement T16), so the AP rejects anything over **about 90 bytes of text** (after conversion to Latin-1).

### Sequence
```
Tag                                      AP
 | -- wakes up, turns on radio ---------|
 | POLL(content_id=A) ------------------>|  looks up MAC in the table
 |<---------------- IDLE or CONTENT -----|  within ~5 ms
 | listens for max 20 ms, then radio off|
 | on CONTENT: draws screen, sleeps     |
```
- If the tag gets no reply, it tries once more after a random pause of 2–10 ms. Then it sleeps until the next interval (requirement T11: longer interval after several misses).
- The tag sends no separate acknowledgement. The next POLL carries the new `content_id`, and that is the acknowledgement. If CONTENT is lost, the AP sends it again at the next POLL.
- Unknown MAC: the AP replies IDLE and adds the tag to the table with `content_id: 0`, so the client sees it in `GET /api/tags` and can give it content.

### Listening window and battery
The tag is awake for about 5–25 ms per round. Adjust the window from measurements in phases 5–6; it is the most important power parameter after sleep current.

## 2. HTTP: client → AP (push)

Any software (the client) calls a small REST API on the AP when content changes. The AP never calls the client.

**Prerequisite:** the client must be able to reach the AP on the network: same local network, or a VPN if the client runs elsewhere. Give the AP a fixed IP (DHCP reservation) or use the mDNS name `openesl.local`.

**Authentication:** `Authorization: Bearer <token>`. The token is set in the AP (menuconfig or NVS) and in the client's configuration. Version 1 uses plain HTTP on the local network or VPN (an HTTPS server on the ESP32 is possible, but heavy). Replies without a valid token: `401`.

Text is sent as UTF-8 JSON. The AP converts it to Latin-1 (unknown characters become `?`) and rejects text that is too long with `422`.

### `PUT /api/tags/{mac}`: set content for one tag
```json
{ "layout": "price", "fields": ["Semi-skimmed milk 1 L", "24.90", "NOK 24.90/L"], "price_red": true }
```
```json
{ "layout": "text", "fields": ["Welcome", "Open 9–21", "", ""], "frame_red": true }
```
- `price_red` (bool) gives a red price (flag bit0 in CONTENT). `frame_red` (bool) gives a red frame around the screen (flag bit1). Both are optional and default to `false`.
- `layout` is `price` or `text` today. When layout files arrive (part 3), it becomes a layout name from the file (requirement A14).

Reply `200`:
```json
{ "mac": "00000123456789A8", "content_id": 2914470401, "changed": true, "pending": true }
```
- The AP calculates `content_id` itself (CRC32 of layout, fields and flags). The same content twice gives `changed: false`, and the tag does not draw again. A client can therefore safely send everything every time without tracking what has changed.
- `pending: true` means the tag has not fetched the content yet.
- An unknown MAC is accepted. The content is ready when the tag appears.

### `PUT /api/tags`: set content for many tags (batch)
```json
{ "tags": [ { "mac": "00000123456789A8", "layout": "price", "fields": ["…","…","…"], "price_red": false }, … ] }
```
Reply `200` with one line per tag as above. Practical for the daily price job: one call instead of one per tag. Max 50 tags per call (limited by RAM on the AP). The batch is all-or-nothing: one invalid row gives `422` with `index` pointing at the row, and no tag is changed. `507` if the table becomes full.

### `GET /api/tags`: status for all tags
```json
{
  "ap": { "fw": "0.1.0", "uptime_s": 86400, "channel": 25, "max_tags": 64 },
  "tags": [
    { "mac": "00000123456789A8", "seen_s_ago": 12, "battery_mv": 2950, "rssi": -61,
      "fw": 1, "content_id": 2914470401, "shown_content_id": 2914470401, "pending": false }
  ]
}
```
- `shown_content_id` is what the tag reported in its last POLL. When it equals `content_id`, the tag shows the right content.
- Tags that have sent POLL without any content set have `content_id: 0`. This is how a client finds new tags.
- The AP has no clock, so it sends "seconds ago". The client converts this to a time.
- `max_tags` is how many tags the AP has room for (set in `menuconfig`, default 64).
- `seen_s_ago`, `battery_mv`, `rssi` and `fw` are `null` for tags not seen since the AP started.

### `GET /api/tags/{mac}`: status for one tag
The same object as one line in the list above, plus the content (`layout`, `fields`, `price_red`, `frame_red`).

### `DELETE /api/tags/{mac}`
Removes the tag from the AP. The tag keeps what it shows until it gets new content.

## 3. Planned: tag model and layout files (not specified yet)

Requirements G6, G7, T17, T18, A12. The details are agreed between the tag and AP firmware before they are written into parts 1 and 2.

- **Model in POLL (T18):** the tag states whether it is 2.9" or 1.54". Candidate: the `reserved` byte in POLL.
- **Layout API:** `PUT /api/layouts/{model}` uploads the layout file (JSON from the design tool, currently max 8 KB per model), `GET /api/layouts` lists the version per model.
- **Layout transfer over radio:** a layout is larger than 110 bytes, so it is sent in blocks with a new message type. Block size, numbering, whether the tag asks for missing blocks, and how the tag learns that the layout version is new (e.g. a field in IDLE/CONTENT) are not decided.
