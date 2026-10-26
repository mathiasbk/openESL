# Upgrading from older versions

Most releases are just [update the AP](Updating-the-access-point), then [update the labels](Updating-labels). Every release lists its upgrade instructions at the top of its [release notes](https://github.com/mathiasbk/openESL/releases), and this page has them too.

The usual order:

1. Take a backup (`esl_client.py backup ap.json`).
2. Update the **AP** over the air. Do not flash it with "Erase" ticked: that wipes the labels, layouts, token and WiFi.
3. If your AP is an ESP32-S3 with a C6 as its radio: flash the C6 only when the release says its radio changed.
4. Update the **labels** last, one of each model first, then a few more, then all. They keep working on the old firmware meanwhile, so with many labels this can take days.

## Skipping releases

Go straight to the newest release: you do not have to install the ones in between. Do the steps of each release you skip, in one pass. From 0.5.x to 0.8.1, for example:

1. Backup, then the AP to 0.8.1 over the air. It keeps `openesl.local`.
2. The C6 radio of an S3 on a cable has not changed since 0.5.0; flash it only if it is the one with the 0.96" screen and its colours are wrong.
3. The labels to the label firmware of the 0.8.1 release (it is version 0.6.0), one of each model first.
4. Use the new layout elements (icons, lines, QR codes, rounded boxes) and pictures only when you will not go back to 0.5.x: a 0.5.x AP refuses such layout files.

## From 0.8.0 to 0.8.1

Update the AP. The labels' firmware has not changed, and neither has the C6 radio of an S3 on a cable. Going back to 0.8.0 keeps everything.

- Labels no longer keep polling every 2 s after an AP restart when the global poll interval is 0.
- A label in the middle of a firmware or picture download comes back after 5 s for the rest, so a firmware update takes minutes instead of four poll intervals.
- S3 with a C6 over WiFi: flash the C6 too for the faster downloads. In the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab pick "S3 + C6 over WiFi", skip the S3 and flash the C6 without "Erase". It keeps its WiFi.
- `esl_client.py --url` works without `http://`, and there is a page on [moving to a new AP](Moving-to-a-new-AP).

Every detail: [docs/release.md](https://github.com/mathiasbk/openESL/blob/master/docs/release.md#upgrading-from-080-to-081).

## From 0.7.x to 0.8.0

Update the AP. The labels' firmware has not changed, and neither has the C6 radio of an S3 on a cable. Going back to 0.7.0 keeps everything.

- The AP now checks every label's content against its layout file, at start and when you upload a file. A label whose barcode, QR code or number does not fit its box keeps what it shows and gets nothing new until it gets content that fits, or a layout file it fits: look for a content error in the dashboard (or `content_error` in `GET /api/tags`) after the update. Before 0.8.0 such a label showed the code blank.
- S3 with a C6 over WiFi: the C6 can now send a browser that opens its address to the S3's dashboard. Optional: in the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab pick "S3 + C6 over WiFi", skip the S3 and flash the C6 without "Erase". It keeps its WiFi.
- The designer has a [template library](Label-templates). On the AP it fetches the templates from the internet.

Every detail: [docs/release.md](https://github.com/mathiasbk/openESL/blob/master/docs/release.md#upgrading-from-07x-to-080).

## From 0.6.x to 0.7.0

Update the AP only. The labels' firmware has not changed, and neither has the C6 radio of an S3 (on a cable or over WiFi). Going back to 0.6.0 keeps everything.

- A new AP is called `openesl-xxxx.local` on the network, where xxxx is the four digits of its setup network. That way two new APs on one network each get their own name.
- An AP that already has its WiFi saved keeps `openesl.local`, so bookmarks and Home Assistant keep working. If you have two of those on one network, they still share the name: rename one with "Name" in the WiFi card, or with `esl_client.py --serial <port> wifi --hostname openesl-s3`.
- The [Flash AP](https://mathiasbk.github.io/openESL/#ap) and [Flash tag](https://mathiasbk.github.io/openESL/#tag) tabs are now step-by-step guides. Flashing an AP that runs OpenESL keeps its data, unless you tick "Erase": then it is a new AP, called `openesl-xxxx.local`, so change the address in Home Assistant or give it its old name back.

Every detail: [docs/release.md](https://github.com/mathiasbk/openESL/blob/master/docs/release.md#upgrading-from-06x-to-070).

## From 0.5.x to 0.6.0

Update the AP first, then the labels, as usual. Either order works here: old and new labels and APs still understand each other.

- Layouts get lines, rounded and open boxes, icons and QR codes. After the update the AP sends every label that uses a layout file its screen once more. QR codes now keep a blank border inside their box, so a QR box holds a little less than before.
- Images can go to 0.6.0 labels with a new code that is often shorter, so they arrive faster.

**Going back** to 0.5.x: a 0.5.x AP refuses at start a layout file that uses icons, lines, QR codes or the new box options, and that model has no layouts until a file without them is uploaded. Pictures put on labels through the API are dropped too, though the labels keep showing them. Update the AP to 0.6.0 again and they come back, unless you uploaded a layout file or sent content to those labels in the meantime. The backup keeps the pictures as well. Take a backup (`esl_client.py backup ap.json`) before updating.

Every detail: [docs/release.md](https://github.com/mathiasbk/openESL/blob/master/docs/release.md#upgrading-from-05x-to-060).

## From 0.4.x to 0.5.0

In 0.5.0 the AP draws every screen and sends it to the label as an image. Before, the label drew the text itself.

1. Take a backup (`esl_client.py backup ap.json`), then update the **AP first**. A 0.5.0 AP still sends firmware to 0.4.x labels.
2. Then update the labels, one of each model first. Until then they keep the screen they show, and the dashboard says they cannot show images.
3. Do not update labels before their AP: a 0.4.x AP cannot send 0.5.0 labels new screens.
4. Optional, C6 boards: flashing once from the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab (it keeps what the AP has stored) gives room for 7 label models instead of 2. Two is enough for the 2.9" and the 1.54".

**Going back** from 0.5.0 means the AP and the labels together. A 0.4.x AP holds at most 256 labels, and refuses layout files that use the larger limits of 0.5.0.

Every detail: [docs/release.md](https://github.com/mathiasbk/openESL/blob/master/docs/release.md#upgrading-from-04x-to-050).

## Labels flashed before 0.3.0

They have no bootloader, so they cannot take updates over the air. Flash them once more in the jig ([Flashing labels](Flashing-labels)); after that, updates go over the radio.
