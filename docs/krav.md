# Requirements

**MUST** = version 1 is not finished without it. **SHOULD** = included if cheap. **LATER** = deliberately deferred to version 2.

## Overall

| ID | Requirement | Level |
|---|---|---|
| G1 | A price or text change sent to the AP's API shows on the correct tag within 2 minutes. | MUST |
| G2 | All code in tag and AP is written for this project or taken from sources with a permissive licence (MIT, BSD, Apache 2.0, OFL or similar). No code from other firmware for the same hardware. | MUST |
| G3 | At least 20 tags per AP. | MUST |
| G4 | The system tolerates power loss on the AP without losing content or requiring tags to be set up again. | MUST |
| G5 | Builds of tag and AP firmware are reproducible from git (pinned SDCC and ESP-IDF versions). | MUST |
| G6 | Supports both SoluM 2.9" (ST-GM29XX2, 296×128) and SoluM 1.54" (152×152). Screen size is a per-tag parameter, not an assumption in the code. Model and colours of the 1.54" are confirmed in phase 0. | MUST |
| G7 | The screen design (layout) is made in a separate design tool (not built yet) and stored as a JSON file: fields, positions, fonts, colours. The firmware reads the layout from the file instead of having it fixed in the code. The fixed layouts ("Price", "Text") are the first step until the file format exists. | MUST |

## Tag (ZBS243)

| ID | Requirement | Level |
|---|---|---|
| T1 | Shows layout "Price": product name (1–2 lines), price in large font (black or red), one subtitle. | MUST |
| T2 | Shows layout "Text": up to 4 lines of free text. | MUST |
| T3 | Supports æ, ø, å, Æ, Ø, Å and common characters (Latin-1). | MUST |
| T4 | Asks the AP for new content at a fixed interval (default 60 s, the AP can change it). | MUST |
| T5 | Updates the screen only when the content is actually new (content ID). | MUST |
| T6 | Battery life at least 1 year on the original batteries with a 60 s interval and one screen update per day. Budget: battery capacity (mAh) / 8760 h. With e.g. 500 mAh usable capacity this gives about 55 µA on average; aim for under half of that to leave margin for cold and ageing. Read the battery type from the tag and calculate the final figure in phase 0. | MUST |
| T7 | Unique ID: the tag's 64-bit MAC (the one already in the tag's flash). | MUST |
| T8 | Reports battery voltage and firmware version to the AP. | MUST |
| T9 | Shows its own screen at start-up (MAC + firmware version) and when it has not found the AP for a long time. | MUST |
| T10 | Watchdog: if the firmware hangs, the tag restarts itself. | MUST |
| T11 | Longer interval when the AP does not answer (e.g. 60 s → 10 min) to save battery. | SHOULD |
| T12 | Shows the content again after a restart without waiting for the AP (stores the last content in flash). | SHOULD |
| T13 | Barcode or QR code. | LATER |
| T14 | Arbitrary images/logos sent through the API. | LATER |
| T15 | Firmware update over radio (OTA). | LATER |
| T16 | Encrypted/signed content (AES-CCM or similar). | LATER, but the packet format has room for it |
| T17 | Draws according to a layout file received from the AP (G7), and only fetches it again when the layout version has changed. | MUST |
| T18 | The tag states its model (2.9" or 1.54") in POLL, so the AP and the client know which layout and text lengths apply. | MUST |
| T19 | Display driver, resolution and layout are parameterised per tag size. One firmware build per size from the same code. | MUST |

## AP (ESP32-C6)

| ID | Requirement | Level |
|---|---|---|
| A1 | Connects to WiFi (SSID/password set once, stored in NVS). | MUST |
| A2 | HTTP API with token: set content for one or many tags, read status ([protokoll.md](protokoll.md) part 2). | MUST |
| A3 | Answers a tag's poll within about 5 ms, so the tag can keep a short listening window. | MUST |
| A4 | Stores the content per tag in NVS so it survives a restart. | MUST |
| A5 | Status per tag available in the API: last seen, battery, RSSI, content shown. | MUST |
| A6 | Simple status page at `http://<ap-ip>/` (list of tags). | SHOULD |
| A7 | Fixed channel and PAN ID, configurable. | MUST |
| A8 | Logging over USB serial. | MUST |
| A9 | Several APs in the same location. | LATER |
| A10 | OTA for the AP itself. | SHOULD (ESP-IDF has nearly all of this ready) |
| A11 | The same content sent again gives no new drawing (the AP compares `content_id`). | MUST |
| A12 | Stores layout files per tag model, accepts new ones via the API and sends them to the tags over radio when the layout is new for the tag (G7). | MUST |
| A13 | Knows the model of each tag and validates text length and layout against it (G6). | MUST |
| A14 | The AP holds no business logic. It knows only tags, layouts and text fields, never products or prices as such. Clients choose the layout per tag by the name in the layout file (e.g. `price-2.9`, `price-1.54`). | MUST |

## Not goals for version 1

OTA, images, encryption, NFC, buttons/LED, several APs, roaming, tag models other than SoluM 2.9" and 1.54", client software (any program that speaks the HTTP API can be used).
