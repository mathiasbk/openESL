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

**POLL** (tag → AP), `type = 0x01`, 18 bytes (12 or 14 from older firmware, see below)
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x01, 0x01 |
| fw_ver | 2 | firmware version, major × 10000 + minor × 100 + patch (0.1.0 = 100, 1.2.3 = 10203; firmware before 0.1.0 sent 1) |
| battery_mv | 2 | battery voltage in mV |
| content_id | 4 | ID of the content the tag shows now (0 = nothing) |
| flags | 1 | bit0 = just started, bit1 = previous drawing failed, bit2 = the tag rejected the layout file the AP offered last (see "Layout transfer") |
| model | 1 | tag model (requirement T18): 1 = 2.9" (296×128), 2 = 1.54" (200×200), 0 = unknown. Was `reserved` (always 0), so old firmware reads as "unknown" |
| cfg_ver | 2 | bytes 12–13: version of the settings the tag uses now (0 = its built-in ones), see CONFIG. Sent only by firmware that understands CONFIG |
| layout_ver | 4 | bytes 14–17: version of the layout file the tag has (0 = none, built-in layouts only). Requirement T17 |

The AP accepts a POLL of 12 bytes or more, and the length says what the firmware can take:

| Length | Firmware | The AP may send |
|---|---|---|
| 12–13 | old | IDLE, CONTENT (built-in layouts) |
| 14–17 | with CONFIG | also CONFIG |
| 18 or more | current (`tag/src`) | also CONFIG and LAYOUT, and content with a layout from the layout file |

So older tags never get a message type they do not know. Fields added later go after byte 18, and the AP ignores bytes it does not know. The flag bits are separate (bit0, bit1, bit2); CONFIG adds no flag.

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
| layout | 1 | 1 = Price, 2 = Text (built in), 0x10 + n = layout n of the tag's layout file (n = 0..15) |
| flags | 1 | bit0 = highlighted price (white digits in a black box), bit1 = black frame around the screen |
| fields | rest | up to 4 text fields, each as `length (1 byte)` + Latin-1 text |

Field meaning: layout 1 = `[name, price, subtitle]`; layout 2 = `[line1, line2, line3, line4]`; a file layout = the `fields` of that layout in the layout file (part 3), in that order. The text is at most about 96 bytes in total. Reserve 4 bytes for a future MIC (requirement T16), so the AP rejects anything over **about 90 bytes of text** (after conversion to Latin-1; exactly 92, `ESL_MAX_TEXT_BYTES`).

The AP only sends layout 0x10 + n to a tag that reports the AP's current layout file version in its POLL. A tag that gets a layout number it does not have (should not happen) shows the fields with the built-in Text layout.

**CONFIG** (AP → tag), `type = 0x08`, 12 bytes: settings for this tag
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x08, 0x01 |
| next_poll_s | 2 | as in IDLE |
| cfg_ver | 2 | version of these settings; the tag sends it back in every POLL |
| poll_s | 2 | normal interval in seconds (T4) |
| max_poll_s | 2 | longest interval while the AP does not answer (T11) |
| lost_ap_s | 2 | seconds without an answer before "No AP found" (T9); 65535 = never |

Every setting is u16, and 0 means the tag's built-in value (`tag/src/config.h`: 60 s, 600 s, 7200 s). The tag raises a nonzero value below the minimum (10 s for the intervals, 60 s for `lost_ap_s`) to the minimum. New settings are only ever appended: the tag reads the fields it knows and ignores the rest, and a field missing from a shorter message counts as 0.

`cfg_ver` is 0 when all settings are 0, otherwise a nonzero 16-bit hash of the settings (CRC32 folded to 16 bits). It is derived from the values, so the AP needs no counter and an AP restart sends nothing new. (A different set of settings with the same hash, 1 in 65535, would not be sent; `poll_s` still arrives through `next_poll_s`.)

**LAYOUT** (AP → tag), `type = 0x04`, 12-byte header + up to 94 bytes of data
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x04, 0x01 |
| next_poll_s | 2 | as in IDLE |
| layout_ver | 4 | version of the AP's layout file for the tag's model; 0 = the AP has none (use the built-in layouts) |
| size | 2 | size of the whole layout file in bytes (0 when layout_ver is 0) |
| pos | 2 | where in the file the data starts |
| data | rest | bytes `pos .. pos + n` of the file, n ≤ 94 (`ESL_LAYOUT_CHUNK_MAX` = 110 − 12 − 4 for the MIC) |

**LAYOUT_REQ** (tag → AP), `type = 0x05`, 10 bytes
| Field | Bytes | Description |
|---|---|---|
| type, proto_ver | 2 | 0x05, 0x01 |
| model | 1 | as in POLL |
| max | 1 | the most data bytes the tag wants in the answer (1..94) |
| layout_ver | 4 | the version the tag is downloading |
| pos | 2 | the first byte the tag is missing |

### Layout file on the tag (binary)
The AP turns the JSON layout file (part 3) into this format. It is what the tag downloads and keeps.

| Bytes | Content |
|---|---|
| 0 | format = 1 |
| 1 | model (as in POLL); the tag rejects a file for another model |
| 2 | number of layouts n, 1..16 |
| 3 | 0 (reserved) |
| then n times | number of elements (1 byte), then the elements, 14 bytes each |

Element, 14 bytes, the same order as `layout_elem_t` in `tag/src/layout.h`:

| Offset | Bytes | Field |
|---|---|---|
| 0 | 1 | kind: 1 = text, 2 = filled rectangle |
| 1 | 1 | text field 0..3 (text only) |
| 2 | 3 | fonts to try in order: 0 = small (17 px), 1 = medium (24 px), 2 = large (52 px, digits and ` ,-.:`), 3 = mid (36 px, same characters); 0xFF ends the list |
| 5 | 1 | alignment: bits 0–1 horizontal (0 left, 1 centre, 2 right), bits 2–3 vertical (0 top, 4 middle, 8 bottom) |
| 6 | 1 | maximum lines, 0 = as many as the box holds (at most 3) |
| 7 | 1 | colour: 0 black, 1 white, 2 inverted (white text in a black box), 3 highlight (inverted if flag bit0, else black), 4 frame (black if flag bit1, else not drawn) |
| 8 | 2 | x (pixels from the left) |
| 10 | 1 | y (pixels from the top) |
| 11 | 2 | w |
| 13 | 1 | h |

How a text element is drawn (font fallback, wrapping, "…", inverted box) is described in `tag/src/layout.h`; the built-in layouts are the same kind of element list.

The **version** of a layout file is the CRC-32 (IEEE 802.3, the same as zlib) of the whole binary file; 0 is counted as 1. The tag checks the CRC of the file it has put together against the version before it uses it. The version changes exactly when what the tags draw changes; a JSON file that only differs in whitespace, descriptions or layout names gives the same binary and keeps the version.

The largest file a tag accepts is **768 bytes** (`ESL_LBIN_MAX`, an XRAM buffer on the tag). That is about 54 elements, for example 7 layouts of 7 elements. The AP refuses bigger files when they are uploaded.

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
- Settings: when a tag sends a `cfg_ver` that differs from the version of the settings the AP has for it, the AP replies CONFIG instead of IDLE. A new layout file and new content go first (see "Priority" below): CONFIG comes on the poll after they are done. The next POLL with the new `cfg_ver` is the acknowledgement, exactly as for content. The tag keeps the settings in flash (T12), so after a restart it reports the same `cfg_ver` and needs no new CONFIG; a tag without a stored copy reports `cfg_ver = 0` and gets them again.
- `next_poll_s` in every reply is the tag's interval (its own `poll_s`, or the AP default). A tag that does not understand CONFIG still gets its interval this way; the other settings need CONFIG.
- Unknown MAC: the AP replies IDLE and adds the tag to the table with `content_id: 0`, so the client sees it in `GET /api/tags` and can give it content.

### Layout transfer (requirements G7, T17, A12)
```
Tag                                          AP
 | POLL(layout_ver=A) ----------------------->|  AP has version B ≠ A for the model
 |<------------- LAYOUT(B, size, pos 0, 94 B) |
 | LAYOUT_REQ(B, pos 94) -------------------->|
 |<------------ LAYOUT(B, size, pos 94, ...)  |  ... until the tag has `size` bytes
 | checks CRC-32 = B, model, structure        |
 | POLL(layout_ver=B) ----------------------->|
 |<---------------------- IDLE or CONTENT ---|
```
- **When the AP sends it.** The AP answers a POLL with the first LAYOUT block instead of IDLE/CONTENT when the POLL has `layout_ver` (18 bytes or more) and it differs from the AP's version for the tag's model, also when the AP has no file (version 0: the tag drops its file). Content waits until the tag has the file; content with a built-in layout as well, so a new file always reaches the tags first. Firmware whose POLL is shorter than 18 bytes never gets LAYOUT.
- **Blocks.** The tag asks for the next missing byte with LAYOUT_REQ and the AP answers with the block from `pos`, at most `max` and 94 bytes. The tag sends each LAYOUT_REQ in the same awake period as the POLL, with the same listening window (20 ms, up to 3 frames) and the same single retry after 2–10 ms. A 768-byte file takes 9 blocks, about 30–100 ms of radio time once per file. The tag asks for at most 16 blocks per round.
- **Lost blocks.** If a block does not come (after the retry), the tag ends the round and keeps what it has. At the next POLL the AP offers block 0 again; if the version and size are the same, the tag goes on from where it stopped. Bytes the tag already has are not taken again. Blocks are matched by `pos`, so a late or repeated answer does no harm.
- **The file changes during the transfer.** Every LAYOUT carries the version. If the tag asks for a version the AP no longer has, the AP answers with block 0 of its current file; the tag sees the new version (or size) and starts over.
- **Check.** When the tag has `size` bytes it computes the CRC-32 and checks the format, the model, that the layouts fill the file exactly, and every element (kind, field, fonts, colour, lines, box inside the screen). A good file is installed with its version. A bad one is dropped; the tag remembers the version, does not try it again, and sets POLL flag bit2 until it gets a file it can use. The AP sees the flag after it has offered that version and stops offering it (the API shows `layout_failed`); content with a built-in layout then gets through again, content with a file layout does not. A new version is offered again.
- **Old drawing.** The tag downloads into the buffer it draws from, so it has no file while a download is in progress and reports `layout_ver` 0. When its file changes (installed, dropped or overwritten) and the screen shows content drawn with a file layout, it reports `content_id` 0 in the next POLL, so the AP sends that content again, resolved against the new file. After a finished download the tag sends that POLL at once, in the same round.
- **Storage on the tag.** The tag keeps a copy of the file in flash (requirement T12) and installs it again at start-up, so after a restart it reports the same `layout_ver` and needs no new download. It also keeps the last content and draws it at start-up, reporting its `content_id` in the first POLL, so the AP answers IDLE. A tag whose flash copy is missing or damaged reports 0 and fetches the file again before it gets content.
- **Unknown messages.** The tag ignores LAYOUT if it is too short, too long or reaches past `size`. The AP ignores LAYOUT_REQ shorter than 10 bytes.

### Priority: layout, content, settings
The AP answers each POLL with one message. When several things are waiting for a tag, it sends them in this order:

1. **LAYOUT**: the layout file, when the tag's `layout_ver` differs from the AP's version for its model (and the tag has not rejected that version). It goes first because the content may use a layout from the file.
2. **CONTENT**: when the tag's `content_id` differs from the AP's. Content with a file layout the tag cannot use (old firmware, not in the file, file rejected) is held back (`content_error` in the API), and the AP goes on to 3.
3. **CONFIG**: when the tag sends `cfg_ver` and it differs from the version of its settings on the AP.
4. **IDLE**.

After a finished layout download the tag sends a new POLL at once, so content (or CONFIG) follows in the same wake-up; otherwise the next item goes out at the next POLL. `next_poll_s` in every reply, LAYOUT blocks included, is the tag's own interval (its `poll_s`, or the AP default).

### Listening window and battery
The tag is awake for about 5–25 ms per round. Adjust the window from measurements in phases 5–6; it is the most important power parameter after sleep current.

## 2. HTTP: client → AP (push)

Any software (the client) calls a small REST API on the AP when content changes. The AP never calls the client.

**The reference is [openapi.yaml](openapi.yaml)** (OpenAPI 3.1): every endpoint, field, limit and error code, with examples. The API tab of the web page shows it with test requests (https://mathiasbk.github.io/openESL/#api), and `ap/test/openapi/check_openapi.py` runs its examples against the AP code (`esl_api.c`) and checks every reply against it, so the spec and the code cannot drift apart. Change the spec together with the code.

In short:
- **Reaching the AP:** HTTP on the local network or a VPN (`openesl.local`, or a fixed IP), or the same API over the AP's USB port as `@ESL` lines, with no token (`ap/README.md`, "Over the USB port"). `tools/esl-client` speaks both, and `esl_client.py serve` is a bridge on localhost that lets the web page send test requests.
- **Authentication:** `Authorization: Bearer <token>`; the token is set in the AP (menuconfig or NVS). Without a valid token: `401`. A read-only status page at `/` needs no token (requirement A6).
- **Errors:** `{"error":"<what is wrong>"}`, in a batch with `"index"` for the row.

| Request | What it does |
|---|---|
| `PUT /api/tags/{mac}` | Set the content of one tag: `{"layout":"price","fields":["Milk 1 L","24.90","NOK 24.90/L"],"price_highlight":true}`. The AP calculates `content_id` (CRC-32); the same content again is `changed:false` and the tag does not draw again |
| `PUT /api/tags` | The same for up to 50 tags, all or nothing |
| `GET /api/tags`, `GET /api/tags/{mac}` | The AP and its tags: last seen, battery, signal, model, what each tag shows, settings, layout file state |
| `DELETE /api/tags/{mac}` | Forget a tag: its content and settings |
| `GET`/`PUT /api/tags/{mac}/config` | Poll interval, longest interval and "No AP found" time for one tag |
| `GET`/`PUT /api/config` | The AP-wide default poll interval |
| `GET /api/layouts`, `GET`/`PUT`/`DELETE /api/layouts/{model}` | The layout file of each model (part 3) |

## 3. Layout files

Requirements G6, G7, T17, T18, A12, A13, A14. The design tool (the Design tab of the web page in `web/`, or the file written by hand) makes **one layout file per tag model**. It holds up to 16 named layouts. A client uploads it with `PUT /api/layouts/{model}` (part 2), chooses a layout per tag by name in `PUT /api/tags` (requirement A14), and the AP sends the file to the tags over radio (part 1, "Layout transfer"). The built-in layouts `price` and `text` stay in the firmware and can always be used, with or without a file. Examples: `tag/designs/layouts/2.9.json` and `1.54.json`; previews in `tag/build/preview/<model>/layout-*.png` (`python tools/render_host.py` in `tag/`).

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
| `model` | `"2.9"` or `"1.54"`, and the same as `{model}` in the URL. Positions are checked against that screen: 296×128 or 200×200 |
| `layouts` | 1 to 16 layouts. **Layout n in this list is sent as CONTENT layout 0x10 + n**; clients only see the names |
| `name` | 1–31 of `a-z 0-9 . _ -`, starting with a letter or digit, unique in the file, not `price` or `text` |
| `fields` | 1–4 field names (`a-z 0-9 _`, starting with a letter, at most 16), the order clients send `fields` in |
| `elements` | 1 or more, drawn in order (all rectangles before all text, see `tag/src/draw.h`) |
| `type` | `"text"` (one field in a box) or `"rect"` (filled box) |
| `x`, `y`, `w`, `h` | whole pixels; `w`, `h` ≥ 1; the box must be inside the screen |
| `field` | text only: a name from `fields` (or its index) |
| `fonts` | text only: 1–3 of `small` (17 px), `medium` (24 px), `mid` (36 px), `large` (52 px); `mid` and `large` only have digits and ` ,-.:`. Tried in order; the first in which the text fits is used, otherwise the last one with "…" |
| `align`, `valign` | text only: `left`/`center`/`right` (default `left`), `top`/`middle`/`bottom` (default `top`; `bottom` keeps one baseline for every font in the list) |
| `lines` | text only: 0–3, 0 (default) = as many as the box holds |
| `color` | `black` (default), `white`, `frame` (only drawn with `"frame": true`); text also `invert` (white text in a black box) and `highlight` (inverted with `"price_highlight": true`, else black) |
| keys starting with `_`, `description` | ignored (comments) |

The AP also checks what the tag can draw, so a file that is accepted always works on the tag (requirement A13):
- a text box must be at least as high as its smallest font (plus 2 × 4 px for `invert`/`highlight`);
- per layout at most 8 text lines (each text element counts the most lines it can use) and 5 rectangles (`rect` elements plus `invert`/`highlight` text): the tag's scene has room for no more (`DRAW_MAX_RUNS`, `DRAW_MAX_RECTS`);
- the binary file is at most 768 bytes;
- unknown keys, a key given twice and anything else above give `422` with the place in the file.

`tag/tools/label_design.py` designs content for these layouts too: a label design with `"layout": "shelf"` takes its field names from the layout file (`render <design> <data> designs/layouts/2.9.json`).
