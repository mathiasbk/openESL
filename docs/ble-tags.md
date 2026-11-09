# Bluetooth tags (in progress)

Bluetooth (BLE) price tags are sold cheaply under several brands. Unlike the SoluM tags
they keep their maker's firmware: openESL does not flash them, it speaks their protocol.
Nothing here works with a real tag yet. This page is the plan and records what is
already built.

## How they differ from the 802.15.4 tags

| | SoluM tags (802.15.4) | Bluetooth tags |
|---|---|---|
| Firmware on the tag | openESL's own | the maker's |
| Who starts a transfer | the tag: it polls the AP, the AP answers (`esl_radio`) | the AP: it connects to the tag and sends the picture (`esl_push`) |
| What the AP sends | a picture it draws (`esl_render`, `esl_image`), in blocks over our protocol | the same picture, packed for the maker's protocol |
| Radio on the AP | 802.15.4 (C6, C5, or a C6 next to an S3) | BLE (any ESP32 with BLE) |

So drawing the screen, the store, the API, the dashboard and Home Assistant stay as they
are. What is new is how the picture gets to the tag.

## Parts

1. **Learning a tag's protocol:** [`tools/ble-probe`](../tools/ble-probe/README.md) reads a
   phone's Bluetooth log of the maker's app and lists what it wrote to which
   characteristic. Clean-room: only our own recordings of our own tags and the makers'
   published documents, and what we learn is written here in our own words.
2. **Delivery** (`esl_core/esl_push.c`, built and tested on the PC, not used on the AP
   yet): the tags the AP sends to itself. Each round, a tag whose content gives another
   image than the one it was last given gets it through a transport. A tag that is not
   reached is tried again after 30 s, doubling up to 30 min, or at the next round when
   the scan hears it (once per run of misses, so a tag that is heard but cannot be
   connected to keeps its back-off). A tag that refuses an image is not sent it again until its content changes.
   After each attempt the store is told as if the tag had polled (its model, battery and
   the image it shows), so "last seen", battery and "shows its content" work as for any
   tag.
3. **BLE on the AP** (started: `main/radio_ble.c`, radio mode `CONFIG_ESL_RADIO_BLE`,
   board `c6-supermini-ble`): NimBLE in the central role scans all the time and logs each
   device it hears once (its address, signal and name: how a tag is found). Reports go
   on a queue to the push task, which calls `esl_push_seen` and runs `esl_push_round`
   every 2 s. To do: pairing (an API to add a tag, kept in NVS with the id of the image
   each was last given) and the transport `send`, which connects, hands the picture to
   the tag's protocol module and disconnects. Until then `send` answers "not reached".
4. **Protocol modules** (to do, once the tags are recorded): one per tag family, turning
   a raw picture (the black plane, then the red plane) into the maker's commands.
5. **Models:** `shared/models.json` already has `radio` and `protocol` fields; a
   Bluetooth tag becomes a model with `radio: "ble"` and its maker's protocol.
6. **Yellow** (to do): some of these tags are black/white/red/yellow. The AP draws the
   picture, so yellow needs a plane in the renderer and the designer, not in any tag
   firmware.

## Which AP

Measured with ESP-IDF 5.5.5 (2026-11-09): the Bluetooth controller (`libble_app`, about
238 KB) and the NimBLE host (`libbt`, about 79 KB) make the app 1561 KB. That does not fit
the 1.5 MB app slots of the C6 Super Mini's partition table, so `c6-supermini-ble` has its
own table without the `tagfw` partition (Bluetooth tags have no tag firmware for the AP to
keep): 1728 KB slots, 167 KB free. Such an AP is installed fresh over USB, never moved to
from the 802.15.4 firmware over the air. Heap use is not measured on hardware yet.


An ESP32-C6 runs either the 802.15.4 tags or the Bluetooth tags, not both: BLE is a
separate firmware choice, so the 802.15.4 firmware does not grow and its radio is never
shared. An S3 or C5 has room for both, and on an S3 with a C6 radio the S3 can do BLE
while the C6 keeps 802.15.4.
