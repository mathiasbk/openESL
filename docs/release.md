# Making a release

How a release goes from `development` to the tags: the pull requests, the checks on real hardware, the rollout and what to do when a release is bad. How versions work is in the README, "Releases".

## 1. Pull requests

1. Branch `version-X.Y.Z` from `development`, change [VERSION](../VERSION) and add the section "Upgrading from A to X.Y.Z" to [Upgrade instructions](#4-upgrade-instructions) (and the user guide's page), and merge it to `development`. CI fails without that section.
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
- [ ] The notes start with the upgrade instructions (docs/release.md, "Upgrading from A to X.Y.Z"): the order, what is not backward compatible, what going back costs
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

Every release has a section "Upgrading from A to X.Y.Z" below, also a patch release where nothing special is needed (then it says so: update the AP, then the tags). The section says the order, what is not backward compatible, and what going back costs. `tools/release_notes.py` puts it at the top of the release notes and stops with an error when the section for the version in [VERSION](../VERSION) is missing. CI checks it on every pull request that changes more than docs (`tools/test_release_notes.py`), and the release job first (`python3 tools/release_notes.py --check`), so a release cannot go out without it. The user guide's page [Upgrading from older versions](../wiki/running/Upgrading-from-older-versions.md) says the same in fewer words.

The usual order, unless a section says otherwise:

1. `esl-client backup ap.json`, with the esl-client of the new release (it also works with older APs).
2. The AP first, over the air (`esl-client ota ... --wait`), then check that `GET /api/ota` says confirmed and that tags check in. Not with "Erase" in the Flash AP tab: that wipes the tags, layouts, token and WiFi.
3. A C6 radio next to an S3 is flashed by hand (the S3 cannot update it yet), and only when the section says it changed.
4. The tags last, one of each model first (Rollout, above). The tags may stay on the old firmware for days: an AP talks to the tags of the release before it.

### Upgrading over more than one release

Go straight to the newest release, in one pass: AP, then radio, then tags, with the steps of every release in between. Nothing has to pass through a release on the way. The AP keeps its layout files as JSON text and reads them again at every start, and it converts nothing it has stored, so an AP that skips a release ends up with the same data as one that took each. The tags take any image that is not their own version.

From 0.5.x to 0.8.1, for example, with many tags:

1. `esl-client backup ap.json` with the 0.8.1 esl-client.
2. The AP: `openesl-ap-<board>-v0.8.1-ota.bin` over the air. It keeps `openesl.local`, since its WiFi is saved. At their next poll, every tag with a file layout gets its screen once more (layout format 2), so the radio is busy for about one poll interval. Then check `content_error` in `GET /api/tags`: content that does not fit its layout file is held back since 0.8.0 (below).
3. ESP32-S3 with a C6 on a cable: the C6 radio has not changed since 0.5.0, apart from the screen of the C6 with a 0.96" LCD (colours and lines, #168). Flash it from the Flash AP tab only for that. The S3 with a C6 over WiFi (netradio) is new in 0.6.0, so a 0.5.x setup has none.
4. The tags: the tag image of the 0.8.1 release, which is version 0.6.0 (tag firmware is unchanged since 0.6.0). One tag of each model, at least 10 polls, a few more chosen tags, then `to=all`. Until a tag has it, it gets image format 1 and works as before.
5. Use the new layout elements (icons, lines, QR codes, round and open boxes) and `PUT /api/tags/{mac}/image` only once the AP will stay on 0.6.0 or newer: a 0.5.x AP refuses such layout files (below).

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

## 5. A bad release

- **Fix forward:** the normal way. Fix it on `development` and make a patch release (X.Y.Z+1) the same way as above.
- **AP:** send the previous release's `openesl-ap-...-ota.bin` with `esl-client ota`. Its tag content, layouts and settings stay, but only if the bad release did not change what the AP stores (a one-way change, see CONTRIBUTING.md): an older AP may read a newer format wrong. Then fix forward, and take `esl-client backup ap.json` before every AP update so the data can be put back.
- **Tags:** upload the previous release's `openesl-tag-<model>-...-ota.bin` to the AP. A tag takes any version that is not its own and has not failed on it, so this also goes back. A tag that does not start must be flashed again with the flasher.
- Write what went wrong in the issue for it, and add a line to the checklist above if a check would have caught it.

Not tried on hardware yet: the automatic rollback of the tag after 9 failed starts, and of the AP ([tag-ota.md](tag-ota.md), "Still to do").
