# Upgrading from older versions

Most releases are just [update the AP](Updating-the-access-point), then [update the labels](Updating-labels). Every release lists its upgrade instructions at the top of its [release notes](https://github.com/mathiasbk/openESL/releases), and this page has them too.

The usual order:

1. Take a backup (`esl_client.py backup ap.json`).
2. Update the **AP** over the air. Do not flash it with "Erase" ticked: that wipes the labels, layouts, token and WiFi.
3. If your AP is an ESP32-S3 with a C6 as its radio: flash the C6 only when the release says its radio changed.
4. Update the **labels** last, one of each model first, then a few more, then all. They keep working on the old firmware meanwhile, so with many labels this can take days.

## Skipping releases

Go straight to the newest release: you do not have to install the ones in between. Do the steps of each release you skip, in one pass. From 0.5.x to 0.9.0, for example:

1. Backup, then the AP to 0.9.0 over the air. It keeps `openesl.local`. Take another backup afterwards: it now holds the radio key, so keep it secret.
2. The C6 radio of an S3 on a cable keeps working. Flash it once more from the Flash AP tab if you want to update it over the cable from now on (0.8.2), or if it is the one with the 0.96" screen and its colours are wrong.
3. The labels to the label firmware of the 0.9.0 release, one of each model first.
4. Use the new layout elements (icons, lines, QR codes, rounded boxes) and pictures only when you will not go back to 0.5.x: a 0.5.x AP refuses such layout files. Pair labels or change the radio channel only once every label runs 0.9.0.

## From 0.8.2 to 0.9.0

Update the AP, then the labels; any order works, and old and new talk to each other. With an S3 and a C6 over WiFi, flash the C6 too if you want to change how long a label waits without the AP, or change the radio channel (below). Pair labels and change the channel only after that, when every label runs 0.9.0.

- Labels can now be paired with the AP, so only your AP can change what they show or give them firmware ([Security on the radio](Security-on-the-radio)). Nothing changes until you pair labels: unpaired labels work as before. Take a backup after the update: it now holds the radio key, so keep the file secret.
- The dashboard's Signal column shows the last check-ins of each label as bars, with lost check-ins and retries marked ([Dashboard](Dashboard#signal)). Labels need the 0.9.0 label firmware for the lost and retried check-ins; older ones show the signal the AP hears only.
- The 0.9.0 label firmware saves battery: it sleeps while the screen changes, measures its battery less often, and after a day without the AP checks only once an hour. Such a label can take up to an hour to show up when the AP comes back. `PUT /api/tags/{mac}/config` takes `long_after_min` and `long_poll_s` ([Access point settings](Access-point-settings)).
- A C6 access point holds about 510 labels instead of 700, since each label's signal history and radio security take memory.
- A label updated to 0.9.0 uses the built-in settings until its next check-in, then gets its own again.
- The radio channel can be changed under **Settings → Radio** ([Access point settings](Access-point-settings)). Only labels on 0.9.0 follow the AP to a new channel, so update every label (and a C6 over WiFi) first: the AP refuses to move while it hears an older label. Before going back to 0.8.2, set the channel back to 25.
- Prices can be shown in whole kroner with `,-` (`199,-`) in the designer. Use it only once the AP will stay on 0.9.0: an older AP refuses such a layout file.
- Layouts can show today's date or week number with date fields in a fixed text, such as "Week {week}" ([Layout elements and limits](Layout-elements-and-limits)). Use them only once the AP runs 0.9.0: an older AP shows `{week}` as written.
- Going back to 0.8.2: the AP answers every label unsigned again, and paired labels get no answer until it is back on 0.9.0. A label that goes back to older firmware forgets its key; turn "Only paired labels" off first.
- An S3 with a wireless C6 cannot pair labels yet.
- **Many labels?** Updating the AP is safe: it keeps every label, and labels on older firmware work as before. Three later steps can leave a label that only the flasher brings back, so take them slowly or not at all: pairing (a paired label answers only an AP with its key, so keep the backup and never flash the AP with "Erase" without restoring it), changing the channel, and the new label firmware (give it to a few labels for some days first).

Every detail: [docs/release.md](https://github.com/mathiasbk/openESL/blob/master/docs/release.md#upgrading-from-082-to-090).

## From 0.8.1 to 0.8.2

Update the AP. The labels' firmware has not changed. Going back to 0.8.1 keeps everything on the AP, and the radio sends at the built-in power again (a C6 over WiFi on 0.8.2 only after it restarts).

- S3 with a C6 on a cable: the S3 can now update the C6 radio over the cable, from the dashboard (the **Radio** line under Firmware) or with `esl_client.py ota openesl-ap-radio-esp32c6-<version>-ota.bin --wait`. The C6 needs one more flash over USB first, from the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab; until then it keeps working as before. See [Updating the access point](Updating-the-access-point). Going back to an older radio firmware needs the Flash AP tab over USB.
- The radio's transmit power can be set in the dashboard's Access point card ("Radio transmit power"), or with `esl_client.py config --tx-power 13`. Nothing changes until you set it. In the EU, stay at 13 dBm or below (less with an antenna that has gain); the dashboard warns above that. With an S3 and a C6 over WiFi, flash the C6 too for it to take the setting: in the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab pick "S3 + C6 over WiFi", skip the S3 and flash the C6 without "Erase".
- "Last seen" in the API and dashboard no longer jumps to a huge number when a label checks in while the AP answers.

Every detail: [docs/release.md](https://github.com/mathiasbk/openESL/blob/master/docs/release.md#upgrading-from-081-to-082).

## From 0.8.0 to 0.8.1

Update the AP. The labels' firmware has not changed, and neither has the C6 radio of an S3 on a cable. Going back to 0.8.0 keeps everything.

- Labels no longer keep polling every 2 s after an AP restart when the global poll interval is 0.
- A label in the middle of a firmware or picture download comes back after 5 s for the rest, so a firmware update takes minutes instead of four poll intervals.
- Labels that stayed "new content waiting", often many at once after a new AP, new labels or a new layout file, now get their screen. The AP answered them too late while it drew the screen; now it tells them to come back in 2 s and draws it in the meantime. Nothing to do on the labels.
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
