# Making a release

How a release goes from `development` to the tags: the pull requests, the checks on real hardware, the rollout and what to do when a release is bad. How versions work is in the README, "Releases".

## 1. Pull requests

1. Branch `version-X.Y.Z` from `development`, change [VERSION](../VERSION), add the section "Upgrading from A to X.Y.Z" to [Upgrade instructions](#4-upgrade-instructions) (and the user guide's page) and "Summary of X.Y.Z" to [Summaries](#6-summaries), and merge it to `development`. CI fails without those sections.
2. Open the pull request `development` → `master` titled "Release X.Y.Z" with the release template: add `?template=release.md` to the address of the new pull request ([.github/PULL_REQUEST_TEMPLATE/release.md](../.github/PULL_REQUEST_TEMPLATE/release.md)). Its notes are the output of

   ```
   python3 tools/release_notes.py v<previous> origin/development
   ```

3. Do the checklist below, then merge with a merge commit (not squash), so `master` keeps the history of `development`.

The usual way [release.yml](../.github/workflows/release.yml) starts is a merge to `master` that changes `VERSION`, so step 1 must be merged before step 3. It can also be started by hand or by pushing a tag `v<VERSION>` (README, "Releases"). It builds the firmware, publishes the GitHub release with the files and `SHA256SUMS.txt`, and publishes the web page again. The same push to `master` publishes `wiki/` to the GitHub wiki ([wiki.yml](../.github/workflows/wiki.yml)).

## 2. Checklist before merging to master

The release template has this list; tick it off in the release pull request. Change both together.

```
- [ ] CI green on the release pull request
- [ ] python tools/check.py passes on the development HEAD, ap-fw included
- [ ] The notes from tools/release_notes.py read right, and say which parts changed
- [ ] The notes read: a short summary, the changes, then short upgrade instructions (docs/release.md, "Summary of X.Y.Z" and the first paragraph of "Upgrading from A to X.Y.Z": the order and what to watch out for)
- [ ] One-way changes in this release are listed, and each was tested on hardware
- [ ] The wiki (wiki/) describes this release: new features, changed steps, upgrade notes
AP (if the AP firmware changed):
- [ ] OTA to a real AP (esl-client ota ... --wait); GET /api/ota says confirmed
- [ ] The dashboard loads, tags check in, sending text to a tag works
Tags (for each model whose firmware changed: 2.9" and 1.54"):
- [ ] OTA to one tag of the model (see Rollout); its start screen shows the new version
- [ ] The tag shows its screen as the dashboard does and checks in for at least 10 polls
- [ ] Battery and RSSI in the dashboard look as before
Upgrade:
- [ ] The upgrade path: the previous release on every AP type and on tags, then upgraded as the upgrade instructions say
- [ ] A new install of this release on every AP type
```

A part whose firmware did not change in this release ("unchanged" in the notes) needs no hardware check.

## 3. Rollout to the tags

New tag firmware goes to one tag of each model first, not to all at once:

1. Upload the image for one tag: pick the tag in the dashboard, or `PUT /api/tag-firmware?to=<MAC>`. Not `esl-client tag-ota <file>`: it sends without `to`, so every tag of the model gets the image at once.
2. Watch it until it has installed and checked in for at least 10 polls (`esl-client tag-ota` shows how far it is).
3. Give it to the rest: `POST /api/tag-firmware/{model}?to=all`. With many tags, a few more first: `POST /api/tag-firmware/{model}?to=<MAC>` adds one tag at a time, up to 16.

Tags that are being used for a test of other firmware are kept out until the test is done: give the image to chosen MACs instead of `all`.

The AP is updated with `esl-client ota openesl-ap-...-ota.bin --wait`. If the new image never gets WiFi or an API request within three minutes, the AP goes back to the old one by itself.

## 4. Upgrade instructions

Every release has a section "Upgrading from A to X.Y.Z" below, also a patch release where nothing special is needed (then it says so: update the AP, then the tags). The section says the order, what is not backward compatible, and what going back costs. Its first paragraph is the short version, a few sentences: the order and what to watch out for. `tools/release_notes.py` puts only that paragraph at the end of the release notes, after the changes, with a link to the whole section, and stops with an error when the section for the version in [VERSION](../VERSION) is missing. CI checks it on every pull request that changes more than docs (`tools/test_release_notes.py`), and the release job first (`python3 tools/release_notes.py --check`), so a release cannot go out without it. The user guide's page [Upgrading from older versions](../wiki/running/Upgrading-from-older-versions.md) says the same in fewer words.

The usual order, unless a section says otherwise:

1. `esl-client backup ap.json`, with the esl-client of the new release (it also works with older APs).
2. The AP first, over the air (`esl-client ota ... --wait`), then check that `GET /api/ota` says confirmed and that tags check in. Not with "Erase" in the Flash AP tab: that wipes the tags, layouts, token and WiFi.
3. A C6 radio on a cable next to an S3, only when the section says it changed: since 0.8.2 over the air through the S3 (`esl-client ota openesl-ap-radio-esp32c6-<version>-ota.bin --wait`, or the dashboard). A radio flashed before 0.8.2 needs one more flash over USB first (Flash AP tab), which gives it the second app slot ([ap-radio-link.md](ap-radio-link.md#updating-the-radio-over-the-cable)).
4. The tags last, one of each model first (Rollout, above). The tags may stay on the old firmware for days: an AP talks to the tags of the release before it.

### Upgrading over more than one release

Go straight to the newest release, in one pass: AP, then radio, then tags, with the steps of every release in between. Nothing has to pass through a release on the way. The AP keeps its layout files as JSON text and reads them again at every start, and it converts nothing it has stored, so an AP that skips a release ends up with the same data as one that took each. The tags take any image that is not their own version.

From 0.5.x to 0.9.0, for example, with many tags:

1. `esl-client backup ap.json` with the 0.9.0 esl-client. Take another one after the update: from 0.9.0 the backup holds the radio key, so keep it secret.
2. The AP: `openesl-ap-<board>-v0.9.0-ota.bin` over the air. It keeps `openesl.local`, since its WiFi is saved. At their next poll, every tag with a file layout gets its screen once more (layout format 2), so the radio is busy for about one poll interval. Then check `content_error` in `GET /api/tags`: content that does not fit its layout file is held back since 0.8.0 (below).
3. ESP32-S3 with a C6 on a cable: the C6 radio works as before, so flashing it is optional. Flashed once more over USB from the Flash AP tab, it takes later updates over the cable (0.8.2, below), and the one with a 0.96" LCD shows its colours and lines right (#168). The S3 with a C6 over WiFi (netradio) is new in 0.6.0, so a 0.5.x setup has none.
4. The tags: the tag image of the 0.9.1 release, which is version 0.9.1 (the first new tag firmware since 0.6.0; not the 0.9.0 tag image, which draws faded screens, see 0.9.1 below). One tag of each model, at least 10 polls, a few more chosen tags, then `to=all`. Until a tag has it, it gets image format 1 and works as before.
5. Use the new layout elements (icons, lines, QR codes, round and open boxes) and `PUT /api/tags/{mac}/image` only once the AP will stay on 0.6.0 or newer: a 0.5.x AP refuses such layout files (below). Pair the tags, and change the radio channel, only once every tag runs 0.9.0 (below).

### Upgrading from 0.4.x to 0.5.0

In 0.5.0 the AP draws every screen and sends it to the tag as an image. The tag no longer gets texts and a layout file (CONTENT and LAYOUT are retired, [protokoll.md](protokoll.md)). So the order matters:

1. Take `esl-client backup ap.json`, then update the AP first. The new AP still answers the POLL of 0.4.x tag firmware and still sends it firmware (FW).
2. A tag on 0.4.x gets no new screens from a 0.5.0 AP: it keeps the screen it shows, and `GET /api/tags` says in its `content_error` that it cannot show images. Update the tags to 0.5.0, one of each model first (above).
3. A 0.4.x AP does not draw images, so 0.5.0 tags get no new screens from it. Do not update tags before their AP.
4. Optional, C6 boards only: the `tagfw` partition grew from 2 to 7 tag models in 0.5.0. An AP updated over the air keeps 2, which is enough for the 2.9" and the 1.54". The Flash AP tab gives it 7 and keeps what it has stored.

Going back from 0.5.0 means the AP and the tags together, to the previous release's AP and tag images. A 0.4.x AP holds at most 256 tags (tags in slots above that are not loaded), and it refuses a layout file at boot that uses the larger limits of 0.5.0 (more than 16 text lines or 10 boxes per layout, or more than 768 bytes compiled). Put such a file back from the backup after making it fit.

### Upgrading from 0.5.x to 0.6.0

Order: the AP, then the tags (either order works, see below). The C6 radio of an ESP32-S3 on a cable has not changed; the one with a 0.96" LCD shows its colours and lines right after it is flashed with 0.6.0 (#168), which is optional. The S3 with a C6 over WiFi (netradio) is new: flash both from the Flash AP tab ([ap-netradio.md](ap-netradio.md)). Not backward compatible: layout files that use the new elements, and pictures, when the AP goes back to 0.5.x (below).

Images to the tags get a second code, image format 2 ([protokoll.md](protokoll.md), "The 2D image code"). Either order works: a 0.6.0 AP sends image format 2 only to tags whose POLL has bit7, and then only when it is shorter than image format 1, so 0.5.x tags keep getting image format 1, and 0.6.0 tags still take image format 1 from a 0.5.x AP. Update the AP first as usual, then the tags.

Layout files compile to layout format 2 (lines, round and open boxes, icons, QR codes). The AP sends every tag with a file layout its screen again once after the update, because the file's version changes, and QR codes now keep a quiet zone inside their box, so a QR box holds a smaller version than before. Going back to 0.5.x: a 0.5.x AP refuses at boot a stored file that uses `icons` or the `line` and `qr` types, or `radius`, `border`, `direction` or `width`, and the layouts of that model are gone until a file without them is uploaded. It also drops a picture put on a tag with `PUT /api/tags/{mac}/image` ("ignoring stored entry" in its log). Neither is erased while the AP runs 0.5.x, and the tags keep showing what they had: updating the AP to 0.6.0 again brings back the layout files and pictures that were not replaced in the meantime (tried on hardware). A layout file uploaded, or content sent to a tag, under 0.5.x wins, and a picture can then be lost. `esl-client backup` keeps the pictures too.

### Upgrading from 0.6.x to 0.7.0

Order: only the AP. The tag firmware has not changed (the tag image of this release is 0.6.0): tags on 0.6.0 need nothing, tags on 0.5.x take it as in "Upgrading over more than one release". Neither C6 radio has changed (on a cable or over WiFi), so neither needs flashing; the dashboard now shows the link to the C6 over WiFi. Nothing is one-way: an AP that goes back to 0.6.0 keeps its data and its name.

A new AP is `openesl-xxxx.local` on the network, with the four digits of its setup network, so two APs on one network get their own names. An AP that already has its WiFi saved keeps `openesl.local`, so bookmarks and Home Assistant keep working. Two such APs on one network still share the name until one is renamed: `esl_client.py --serial <port> wifi --hostname openesl-s3`, or "Name" in the WiFi card. An AP flashed with "Erase" ticked in the Flash AP tab (new in 0.7.0; without it the tab keeps what the AP has stored) is a new AP too, and gets `openesl-xxxx.local`: change the address in Home Assistant's `rest_command` and in `OPENESL_URL`, or give it back its old name.

### Upgrading from 0.7.x to 0.8.0

Order: the AP, then, only with an S3 and a C6 over WiFi (netradio), the C6 if you want a browser that opens its address sent to the S3's dashboard (#229). The tag firmware has not changed (the tag image of this release is 0.6.0): tags on 0.6.0 need nothing, tags on 0.5.x take it as in "Upgrading over more than one release". The C6 radio on a cable has not changed. Nothing is one-way: an AP that goes back to 0.7.0 keeps its data, its layout files and its name, and the C6 radio over WiFi of 0.7.0 and of 0.8.0 each work with either S3. Content that 0.8.0 held back because it does not fit its file (below) is drawn again by 0.7.0, with that code left blank.

The AP checks the content of every tag against its model's layout file, at boot and when a file is uploaded (#221), as it already did for new content in `PUT /api/tags`. A tag whose barcode, QR code or number does not fit its box in the file keeps the screen it shows, and its content is held back until it gets content that fits or a file it fits: `content_error` in `GET /api/tags` says so, and `PUT /api/layouts/{model}` lists such tags in `content_errors`. So after the update, look for `content_error` in `GET /api/tags` (or the dashboard) and send those tags content that fits; under 0.7.0 such a tag got its screen with that code left blank, and nothing said so.

The C6 radio over WiFi now sends a browser that opens its address to the S3's dashboard (#229). It is optional and changes nothing else: in the Flash AP tab, pick "S3 + C6 over WiFi", skip the S3 and flash the C6 without "Erase" (it keeps its WiFi and the S3's address).

`esl-client backup` notes the AP's name and the tag firmware, and the user guide describes moving to another AP board (#222). The designer has a template library (#230); on the AP it reads the templates from the GitHub Pages site, so the browser needs internet for them, else it offers the AP's example layouts as before.

### Upgrading from 0.8.0 to 0.8.1

Order: the AP, then, only with an S3 and a C6 over WiFi (netradio), the C6. A patch release: update the AP over the air as usual. The tag firmware has not changed (the tag image of this release is 0.6.0), and neither has the C6 radio on a cable. Nothing is one-way: an AP that goes back to 0.8.0 keeps its data, its layout files and its name. Do not go back while tags are part way through a firmware download with the global poll interval at 0: such a tag keeps the 5 s it was told, since a 0.8.0 AP answers its polls with 0 (#234) until it gets a new image. Set a global poll interval for a while if it happens.

Two fixes to the poll interval the AP tells the tags:

- With the global poll interval at 0 (the default: tags use their own), an image tag that had been told to come back in 2 s while its image was being drawn, and then had nothing to fetch (as after an AP restart), kept polling every 2 s. It now gets the built-in 60 s back (#234).
- A tag part way through a firmware or image download is told to come back in 5 s for the next blocks, instead of after its normal interval, so a firmware update no longer takes three or four poll intervals per tag (30 to 40 minutes at 600 s). The last block brings the normal interval back (#236).

The C6 over WiFi answers the tags from its own cache, so it gives the 5 s for downloads only once it runs 0.8.1: in the Flash AP tab, pick "S3 + C6 over WiFi", skip the S3 and flash the C6 without "Erase" (it keeps its WiFi and the S3's address). Without it, downloads through it work as in 0.8.0. The fix for the 2 s polling (#234) is on the S3 and needs nothing on the C6.

A fix for tags that stay "new content waiting" (#238). The AP drew a tag's screen while the tag waited for its reply, which can take longer than the tag listens (20 ms). With many tags waiting at once (a new AP, many new tags, a new layout file) the reply came too late every time, and those tags never got their screen. The AP now tells such a tag to come back in 2 s and draws the screen in the meantime. Tried on a C6 with a screen: 17 tags that had waited for hours had their screens within one poll interval. The fix is in the AP's own radio (a C6 AP, or an S3 with a C6 on a cable) and needs nothing on the C6; an S3 with a C6 over WiFi answers the tags from the C6's cache and works as in 0.8.0. `GET /api/tags` shows how the AP's own radio answers under `ap.replies` (`slow` counts replies later than 5 ms).

`esl-client --url` takes an address without `http://` (#235), and the user guide has its own page for moving to a new AP.

### Upgrading from 0.9.0 to 0.9.1

Order: only the tags. **Every tag on 0.9.0 firmware should get the tag image of 0.9.1**, one of each model first as usual ([section 3](#3-rollout-to-the-tags)). Nothing the AP or either C6 radio does has changed, so neither needs an update: their images say 0.9.1 only because the AP builds some of the tag's source files too (`tools/fw_version.py`), and the file that changed (`tag/src/epd.c`) is not one of them. Updating the AP anyway does no harm. Nothing is one-way: a tag on 0.9.1 talks to the AP as a 0.9.0 tag does, keeps its settings, radio key and channel, and takes older firmware as before.

The fix (#283): tag firmware 0.9.0 let the core sleep a second at a time while the panel refreshed (#261). On hardware the panel then stops partway: the image comes out grey and half drawn with the old one showing through, on the 1.54" and the 2.9", while the tag tells the AP it was shown. Tag firmware 0.9.1 waits awake through the refresh again, as 0.8.2 did; everything else of 0.9.0 (signal history, long backoff, rarer battery reading, channel, pairing) stays. A display update now costs about what it did in 0.8.2, so "about half the charge of a display update" under 0.9.0 below no longer holds (4.6 µA on average and about 9.3 years in [strombudsjett.md](strombudsjett.md)).

A tag on 0.9.0 still polls and takes firmware normally, so the update goes over the air like any other. After the update the tag has not shown anything yet, so the AP offers it its screen again at its next poll and the tag draws it anew (from the copy in its own flash, without downloading it again): the faded image is replaced by a sharp one, and nothing has to be sent by hand. A tag that never got 0.9.0 skips it and goes straight to 0.9.1 ("Upgrading from 0.8.2 to 0.9.0" below, with the tag image of 0.9.1). Do not give tags the 0.9.0 tag image.

### Upgrading from 0.8.2 to 0.9.0

Order: the AP, then a few test tags for some days, then the rest of the tags. Take a backup after the AP update: it now holds the radio key, which a paired tag needs, so never flash the AP with "Erase" without restoring it. Pair tags and change the radio channel only once every tag runs 0.9.0, if at all. With an S3 and a C6 over WiFi, update the C6 too before you change the channel or set the long backoff per tag.

Any order works for the update itself: a 0.9.0 tag talks to a 0.8.2 AP and the other way round, and a C6 on a cable next to an S3 needs nothing new for this. **Change the radio channel only after everything runs 0.9.0** (below).

Signal history (#261): every POLL from 0.9.0 tag firmware carries a link block (the tag's RSSI and LQI of the AP's last answer, rounds lost since and a retry flag; POLL is 18 bytes, [protokoll.md](protokoll.md#signal)). The AP keeps the last 8 POLLs per tag in RAM (32 on boards with PSRAM), shown as `signal` in `GET /api/tags` and as bars in the dashboard's Signal column. Older tags show the AP's own RSSI only. A tag slot in RAM grows from 88 to 112 bytes (120 with the radio security below), so a C6 AP next to WiFi holds about 510 tags instead of about 700 before `table_full`; the S3 and C5 boards are not affected in practice. The history starts empty after every restart.

Power saving in the tag (#261): the core sleeps while the panel refreshes (about half the charge of a display update), the battery is read every 240th POLL instead of every 60th (every 4 hours at the default 60 s), and after a day without an answer (`long_after_min`, 1440) the tag polls only every hour (`long_poll_s`, 3600) until it hears the AP again. CONFIG grows from 12 to 16 bytes with those two settings (`PUT /api/tags/{mac}/config`); a 0.9.0 tag takes the 12 bytes of an older AP (the long backoff then has its defaults), and a 0.8.2 tag ignores the 4 new bytes. The AP's per-tag settings blob is version 2 (19 bytes); 0.9.0 reads version 1, and an AP that goes back to 0.8.2 drops only the per-tag settings that were saved by 0.9.0 (the tags keep using them until the AP sends new ones). A tag updated to 0.9.0 forgets its stored settings once (settings record format 5; format 4 from the test builds is read) and gets them again with the next CONFIG, so it uses the built-in values for one poll. With an S3 and a C6 over WiFi, a C6 from before 0.9.0 sends tags the long backoff's defaults, and a tag given its own `long_poll_s` or `long_after_min` then shows `config_pending: true` for good (the old C6 does not pass those on). Flash the C6 before setting them per tag; the signal history works with an old C6 as it is.

The radio channel can be changed while the AP runs (#256): "Channel" under Settings → Radio in the dashboard, `PUT /api/config {"channel": 20}` or `esl-client config --channel 20` (`null` goes back to the built-in 25). Nothing changes until it is set: with the built-in channel CONFIG and `cfg_ver` stay as they were, so the update itself sends no tag anything new for it. Only 0.9.0 tag firmware follows the AP to a new channel; an older tag acknowledges the new settings but stays where it is, and would show "No AP found" after the move. So on a site with tags from earlier releases:

1. Update the AP.
2. With an S3 and a C6 over WiFi, update the C6 too: an older C6 retunes when the S3 tells it, but does not pass the channel on to the tags.
3. Update the tag firmware for every model over the air ([section 3](#3-rollout-to-the-tags)).
4. Check in the dashboard that every tag runs 0.9.0.
5. Only then change the channel. The AP tells each tag at its next poll and moves once they have all acknowledged, at the latest after twice the longest poll interval plus a minute (21 minutes at `poll_s` 600).

The AP checks steps 2 and 4 itself: while it hears a tag with older firmware, or an old C6 over WiFi is connected, it refuses to move off the built-in channel (`409`; Settings → Radio says how many tags; `"force": true` moves anyway and leaves those tags behind). Right after the AP starts it also waits until the tags in its table have polled (at most three poll intervals and a minute, 31 minutes at `poll_s` 600), since it cannot know their firmware before, and an old tag that shows up while a move waits calls the move off. An AP that restarts during the wait (an OTA update, a power cut) comes up on the old channel and announces the move again. A 0.9.0 tag that loses its AP tries the announced channel at once and the built-in 25 from its third miss in a row, so an AP set back to 25 is always found; from that third miss it also tries 15, 20, 26 and 11, then one more of 11-26 each round (#266), so a tag that missed the move finds the AP on its new channel by itself. While the AP is down that costs the tag a little battery (about 0.45 µA more until the long backoff takes over after a day). Going back to 0.8.2: set the channel back to the built-in one first; an AP on 0.8.2 ignores `cfg/channel` and starts on 25, where 0.9.0 tags find it after three missed polls.

Date fields in layouts (#183): a fixed text may hold `{weekday}`, `{date}`, `{date_short}` and `{week}`, which the AP fills in with its local date when it draws ([protokoll.md](protokoll.md), part 3). The date is the AP's local one, in the time zone set under Settings, General (UTC until one is set), and a layout with date fields is not drawn until the AP knows the time (NTP, or `PUT /api/time`), so after a restart its tags keep their image until then. A tag with such a layout is drawn again when the day changes (only the week for `{week}` alone) and gets the image only when it came out different. Only the AP changes; the tag firmware and the layout format stay as they were. A 0.8.2 AP takes such a file too and draws the fields as written, so send layouts with them only once the AP runs 0.9.0.

Prices in whole kroner (#278): a text element's `decimals` takes `"0-dash"`, which rounds to the nearest krone and always draws `,-` (`199.49` shows as `199,-`, so only for prices that are whole kroner). Only the AP changes. A 0.8.2 AP refuses a layout file with it (`decimals must be ...`), and an AP that goes back to 0.8.2 drops such a file at boot until one without it is uploaded, so send layouts with `"0-dash"` only once the AP will stay on 0.9.0.

Radio security (#260): tags can be paired with the AP, and a paired tag and the AP then sign every message (AES-128 CMAC, [protokoll.md](protokoll.md#security)), so only this AP can change what the tag shows or give it firmware. Nothing changes until you pair a tag: the AP answers every tag as before, and a tag on the new firmware works unpaired exactly like one on the old. The AP makes a radio key at its first start on 0.9.0 and stores it with the settings (`cfg/radio_key`, and `cfg/radio_req` for "Only paired tags"). Take a backup after the update: the backup now holds the key, so keep the file secret. A C6 over WiFi (netradio) answers the tags itself and has no keys, so pairing is refused with an S3 and a wireless C6 (`409`); nothing changes there. The tag keeps its key, a restart counter and its settings in one record (format 5) in two flash pages in turn, so a battery pulled during a write cannot lose the key.

A site with many tags (a shop with 200, say) can take it in steps: the AP first, then the tag firmware rollout as usual (one tag of each model, a few, then all), and pairing only once the whole site runs 0.9.0, or never. Pairing goes per tag at its next poll, so ticking all of them in the dashboard pairs the site within one poll interval; the lock in the tag list shows which are done. Turn on "Only paired tags" only when every tag shows the lock: a tag that is not paired then gets nothing and shows "No AP found" after its `lost_ap_s`, and once the switch is off again it can take several minutes (about 8 on hardware) to be heard, since it searches less often.

To use it: update the tags, pair them from the dashboard (tick them, Pair) or with `esl-client pair MAC`, wait for the lock in the tag list (`secure: true` in `GET /api/tags`), then turn on "Only paired tags" (Settings, Radio, or `esl-client config --radio-secure on`). Pairing sends each tag its key once unsigned: do it where nobody listens in. A paired tag takes content and firmware only from an AP with its key: after a full erase of the AP, put the key back from the backup (restore does it) or the paired tags must be flashed again.

A site that cannot afford to flash tags again by hand: three steps can leave a tag that only the flasher brings back, and none of them is needed for the update. Pairing (a paired tag answers only an AP with its key: keep the backup, and never flash the AP with "Erase" without restoring it), changing the channel (a tag on older firmware, or one out of range during the move, stays behind; with 0.9.0 tag firmware it finds the AP again by scanning), and the new tag firmware itself (the tag's automatic rollback after 9 failed starts has not been tried on hardware yet). So update the AP, leave the tags on the firmware they have, and give the 0.9.0 tag image to a few test tags for some days before the rest. Pair and change the channel last, if at all.

Going back: a tag on 0.8.2 firmware forgets the long backoff and polls every 600 s without the AP as before; an AP on 0.8.2 ignores the link block, and draws date fields as written (`{week}`), so tags with such layouts show the braces after their next redraw. An AP on 0.8.2 ignores the radio key and "Only paired tags" and answers every tag unsigned, so paired tags get no answers from it until it is on 0.9.0 again; a tag that goes back to older firmware loses its key and sends unsigned, so turn "Only paired tags" off first.

### Upgrading from 0.8.1 to 0.8.2

Order: the AP, then, with an S3 and a C6 on a cable, the C6 if you want to update it over the cable from now on, or with an S3 and a C6 over WiFi, the C6 if you want to set the transmit power. The tag firmware has not changed (the tag image of this release is 0.6.0). The C6 over WiFi (netradio) needs a flash only to take a transmit power set on the S3 (below). The AP stores one new setting, the transmit power (`cfg/tx_dbm`), which an AP that goes back to 0.8.1 ignores: it keeps its data, its layout files and its name, and sends at the built-in 10 dBm again (a 0.8.2 C6 over WiFi keeps the power it was set to until it restarts).

The S3 with a C6 on a cable can update its C6 radio over the cable (#240): the release has `openesl-ap-radio-esp32c6-<version>-ota.bin` (and `-lcd-`), sent like AP firmware with `esl-client ota ... --wait` or from the dashboard, which shows the radio under Firmware. This needs the radio on 0.8.2 once, over USB: a C6 flashed before 0.8.2 has one app slot and link v1 (`radio.ota` false in `GET /api/ota`). It keeps working with a 0.8.2 S3, so this flash can wait. The Flash AP tab erases the radio's flash anyway; pick the `-lcd` image for a radio with the screen. One-way for the radio only (#240: a new partition table and link v2): going back to an older radio image means the Flash AP tab, since older releases have no radio `-ota.bin`. Host and radio of link v1 and v2 work together in both directions. The S3 + plain C6 on a cable is supported now (tried on hardware on 2026-10-26); the C6 with screen as the radio is still experimental.

The radio's transmit power can be set while the AP runs (#243), from -24 to 20 dBm: "Radio transmit power" in the dashboard's Access point card, `PUT /api/config {"tx_power": 13}` or `esl-client config --tx-power 13` (`null` goes back to the built-in 10 dBm). Nothing changes until it is set. The EU allows about 13 dBm EIRP for 802.15.4 (10 dBm/MHz), less with an antenna that has gain; the dashboard warns above 13. It reaches the radio of a C6 AP and of an S3 with a C6 on a cable as it is. An S3 with a C6 over WiFi passes it on in a new optional byte that a C6 from before 0.8.2 ignores: flash the C6 from the Flash AP tab ("S3 + C6 over WiFi", skip the S3, no "Erase") for it to take the setting.

`seen_s_ago` in `GET /api/tags` and `/api/health` no longer shows a huge number (an unsigned wrap of about 2^64 ms) when a tag polls while the AP builds the reply (#244). The same wrap could flag such a tag `silent` and show it as long gone in the dashboard. Clients that turned it into a date (Prisjakt did) got a year far in the past.

## 5. A bad release

- **Fix forward:** the normal way. Fix it on `development` and make a patch release (X.Y.Z+1) the same way as above.
- **AP:** send the previous release's `openesl-ap-...-ota.bin` with `esl-client ota`. Its tag content, layouts and settings stay, but only if the bad release did not change what the AP stores (a one-way change, see CONTRIBUTING.md): an older AP may read a newer format wrong. Then fix forward, and take `esl-client backup ap.json` before every AP update so the data can be put back.
- **Tags:** upload the previous release's `openesl-tag-<model>-...-ota.bin` to the AP. A tag takes any version that is not its own and has not failed on it, so this also goes back. A tag that does not start must be flashed again with the flasher.
- Write what went wrong in the issue for it, and add a line to the checklist above if a check would have caught it.

Not tried on hardware yet: the automatic rollback of the tag after 9 failed starts, and of the AP ([tag-ota.md](tag-ota.md), "Still to do").

## 6. Summaries

Every release has a section "Summary of X.Y.Z" here: a few sentences on what the release brings, for the top of the release notes. `tools/release_notes.py` stops with an error without it, like the upgrade instructions. The notes then list the changes, and the short upgrade instructions come last.

### Summary of 0.9.1

A patch release for the tags. Tag firmware 0.9.0 could draw a grey, half-finished image with the old one showing through, while it told the AP the image was shown; 0.9.1 waits awake through the display refresh again, as 0.8.2 did (#283). Every tag on 0.9.0 should get it. The AP and the C6 radios have not changed.

### Summary of 0.9.0

Radio security: tags can be paired with the AP, so only that AP can change them, and the AP's radio key is shown after flashing and kept in the backup. The radio channel can be changed, and tags that missed the move find the AP again by scanning. Tags use less battery and report their signal, and layouts get date fields and whole-krone prices (`199,-`).
