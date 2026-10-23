# AP with a wireless radio (ESP32-S3 + ESP32-C6 over WiFi)

A separate AP setup, not a variant of [the S3 with a C6 on a cable](ap-radio-link.md): the ESP32-S3
runs the AP, and the ESP32-C6 that talks to the tags sits somewhere else on the same WiFi network,
each on its own power supply, with no wire between them. Short name in code and file names: `netradio`.

**Status: planned.** Only the radio's reply cache is written and tested on the PC
(`ap/components/esl_core/esl_rcache.c`, `ap/test/test_rcache.c`). There is no firmware for it yet, and
nothing in a release. The steps left are at the end.

**Read the limits first.** The setup works only because the radio answers the tags on its own and uses
WiFi as little as it can. That has costs the cabled S3 + C6 does not have. If the radio can sit next to
the S3, the cable is better in every way; the wireless radio is for when it cannot.

## Limits

These come from measurements on a C6 (below), not from guesses. They are what to expect, and what to
tell anyone who picks this setup.

- **Changes reach a tag later.** The radio fetches from the S3 at most every 10 s when something is
  waiting, every 30 s at the latest, and never while a tag is fetching an image. A new price is drawn on
  the S3, sent to the radio at its next sync, and shown when the tag next wakes up. On top of the tag's
  interval, add up to about 10–30 s.
- **The dashboard is behind.** "Last seen", battery, RSSI and whether an image arrived come in batches
  with each sync, so they can be up to 30 s old. The tag table on the S3 is right, only late.
- **WiFi costs tag replies.** The C6 has one 2.4 GHz radio for WiFi and 802.15.4. Every WiFi exchange,
  even one small packet, takes it from the tags for about 80–95 ms, and a tag listens only 20 ms for a
  reply. With one packet every 5 s, 2.6 % of first replies were lost (the tag tries once more, so
  very few wake-ups fail entirely); with several packets a second, about 40 % are lost and the setup is
  useless. Nothing else may talk to the radio over WiFi: no dashboard, no API, no pings.
- **Coex must stay at the driver's default.** Giving 802.15.4 a higher priority kills WiFi (measured),
  because the radio listening between frames counts as busy. There is nothing to tune here.
- **It needs good WiFi where the radio sits.** A weak signal means a lower bit rate, so every packet
  is longer on the air and takes longer from the tags. Measured at −74 to −77 dBm; better is better.
- **More tags, more retries.** The radio syncs only between tags. With many tags the quiet gaps get
  short, syncs fall more often on a tag's reply, and more tags need their second try. Not measured with
  many tags yet.
- **A tag the radio has nothing for polls more often.** When the radio lacks something for a tag (a tag
  it has not heard of, or an image or firmware it has not got yet) it tells the tag to come back in 5 s
  and asks the S3 at the next sync. That costs the tag battery until the radio has it, normally one or
  two wake-ups.
- **When WiFi or the S3 is gone, the tags see the old state.** The radio keeps answering from what it
  has: the image it was told about, the interval, the settings. Nothing new reaches the tags, and if the
  outage is long, the oldest log entries are dropped (counted), so the S3 misses some battery readings
  and "last seen" times.
- **Memory on the C6.** The radio holds the state of every tag it serves, a few images, its log, and the
  tag firmware per model in its flash. How many tags one radio can serve this way is not decided yet.
- **Two chips to update.** The S3 updates over the air as always; the radio is flashed separately until
  the S3 can update it.

## How it works

Over the cable the radio forwards every frame and the S3 replies within a few ms. Over WiFi a round trip
takes 22–110 ms, so that cannot work. Instead:

1. **The radio answers every tag itself**, from a cache the S3 fills beforehand (`esl_rcache`): per tag
   the interval, the settings, the image it should show and the firmware version to offer, plus the
   images and firmware themselves. It picks the reply the way `esl_radio_handle()` does on the S3, in
   the same order (CONFIG, then the image, then firmware, else IDLE), and remembers a tag that failed
   to draw an image or rolled back firmware, so it does not offer that again.
2. **It logs every frame it got.** At each sync the S3 fetches the log and runs it through
   `esl_radio_handle()`, so its tag table ends up as if it had heard the tags itself, only later.
3. **The radio decides when to use WiFi** (`esl_rcache_sync_due()`): only when no tag frame has come for
   60 ms (300 ms in the middle of an image), every 10 s when nothing is missing, 1 s after a tag it
   lacked something for, and every 30 s whatever the tags are doing. In one sync it sends its log and
   says what it lacks, and the S3 answers with the state of every tag in the log and anything new.

The S3 runs the same AP firmware as with the cable, with the tag store, rendering, dashboard and API.
Only the link to the radio differs.

## Measurements

C6 on WiFi with 802.15.4 sending one frame every 50 ms, retried within 12 ms like the AP, on channel 20,
WiFi RSSI −74 to −77 dBm, coex at the default, power save MIN (MAX made no difference).

| UDP packets per second to the C6 | 802.15.4 frames not sent |
|---|---|
| 0 | 0.6–1.4 % |
| 0.2 (one every 5 s) | 2.6 % |
| 1 | 8–9 % |
| 5 | 37 % |
| 20 | 36–39 % |

- WiFi holds the radio in stretches of 50–80 ms, so a retry within 12 ms rarely saves a frame.
- UDP round trip: median 22–26 ms under traffic, 70–110 ms when the C6 has been idle (it has to wake).
- Without power save (PS NONE) about half the frames were lost even with no traffic.
- Coex priority H or M for 802.15.4: every frame went out, and WiFi died (no traffic, or no connection).

## Plan

| Step | What | Status |
|---|---|---|
| 1 | Measure WiFi against 802.15.4 on the C6 | done (above) |
| 2a | The radio's reply cache and log, tested against fake tags on the PC | done |
| 2b | The S3 side: build each tag's state from the store, replay the log | |
| 3 | One radio over WiFi: TCP with TLS-PSK, the S3 announces `_openesl-ap._tcp` on mDNS, the radio connects; set-up and pairing on the radio's own set-up page with a code from the dashboard; a "Radios" tab | |
| 4 | Tag firmware kept in the radio's flash, so the S3 does not send it for every tag | |
| 5 | Several radios per S3: each tag owned by the radio that hears it best, never switched mid-image | |

Release files will have their own names so they are not mixed up with the cabled radio:
`openesl-ap-esp32s3-n16r8-netradio-<version>.bin` and `openesl-ap-netradio-esp32c6-<version>.bin`.

**Alternative: Ethernet.** A C6 with a W5500 (SPI Ethernet, possibly PoE) uses no 2.4 GHz for the link,
so none of the WiFi limits above apply and the replies might even come from the S3 as over the cable (not measured). It
needs a cable to a switch instead of to the S3.
