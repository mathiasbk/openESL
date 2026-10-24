# ESP32-S3 with a wireless C6 radio

**Status:** planned. Not in any release yet; this page says what it will be and, above all, what it costs.

A separate setup from the [ESP32-S3 + C6](ESP32-S3-and-C6) on a cable: the ESP32-S3 runs the AP, and the ESP32-C6 that talks to the labels sits somewhere else on the same WiFi network, each on its own USB charger, with no wire between them. Useful when the labels are in a room the S3 cannot be in.

**If the C6 can sit next to the S3, use the cable instead.** It has none of the limits below.

## Limits

These come from measurements on a C6, not guesses.

- **Changes show up later.** A new price reaches the radio at its next sync with the S3, normally within 10 s (about 5 s on average), up to 30 s when the labels keep the radio busy, and is shown when the label next wakes up. Add that to the label's usual interval.
- **Big updates take longer.** Images (about 5 KB each) and label firmware (about 53 KB) go over WiFi in the pauses between labels, and while they do the radio hears almost no labels. When many labels change at once, the radio gets their images over several syncs, and new label firmware rolls out more slowly than with a cable.
- **The dashboard lags.** "Last seen", battery and whether a label got its image come in batches and can be up to 30 s old.
- **WiFi takes the radio from the labels.** The C6 has one radio for WiFi and the labels. Each WiFi packet blocks the labels for about 90 ms, while a label listens only 20 ms for a reply. The radio therefore uses WiFi as little as it can, between labels. About 2–3 % of first replies were missed when measured with one small packet every 5 s (the label simply tries again); real syncs are larger and not measured yet. The AP never sends to the radio on its own, it only answers. Do not open the radio's address in a browser or point anything else at it while it runs.
- **It needs good WiFi where the radio sits.** Weak WiFi makes every packet longer and costs more label replies.
- **Many labels on one radio:** more retries, as the quiet moments between labels get shorter. Not measured yet.
- **New labels and new images cost a little battery.** Until the radio has what a label needs, it tells the label to come back in 5 s, at most 3 times in a row.
- **If WiFi or the S3 goes down,** the labels keep showing what they have and get nothing new. After a long outage the dashboard misses some battery readings.
- **Two chips to update:** the S3 over the air as usual, the radio separately.

## How it works, in short

Over a cable the radio asks the S3 for each reply, which takes a few milliseconds. Over WiFi that takes 20–110 ms, too slow for a label. So the S3 tells the radio in advance what each label should get (its image, settings and firmware), the radio answers the labels on its own, and afterwards it tells the S3 what happened.

Later the plan is to let one S3 have several wireless radios, each serving the labels it hears best.

The technical details and the measurements: [docs/ap-netradio.md](https://github.com/mathiasbk/openESL/blob/master/docs/ap-netradio.md).
