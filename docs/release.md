# Making a release

How a release goes from `development` to the tags: the pull requests, the checks on real hardware, the rollout and what to do when a release is bad. How versions work is in the README, "Releases".

## 1. Pull requests

1. Branch `version-X.Y.Z` from `development`, change only [VERSION](../VERSION), and merge it to `development`.
2. Open the pull request `development` → `master` titled "Release X.Y.Z". Its body is the output of

   ```
   python3 tools/release_notes.py v<previous> origin/development
   ```

3. Do the checklist below, then merge with a merge commit (not squash), so `master` keeps the history of `development`.

The usual way [release.yml](../.github/workflows/release.yml) starts is a merge to `master` that changes `VERSION`, so step 1 must be merged before step 3. It can also be started by hand or by pushing a tag `v<VERSION>` (README, "Releases"). It builds the firmware, publishes the GitHub release with the files and `SHA256SUMS.txt`, and publishes the web page again. The same push to `master` publishes `wiki/` to the GitHub wiki ([wiki.yml](../.github/workflows/wiki.yml)).

## 2. Checklist before merging to master

Copy this into the release pull request and tick it off.

```
- [ ] CI green on the release pull request
- [ ] python tools/check.py passes on the development HEAD, ap-fw included
- [ ] The notes from tools/release_notes.py read right, and say which parts changed
- [ ] One-way changes in this release are listed, and each was tested on hardware
- [ ] The wiki (wiki/) describes this release: new features, changed steps, upgrade notes
AP (if the AP firmware changed):
- [ ] OTA to a real AP (esl-client ota ... --wait); GET /api/ota says confirmed
- [ ] The dashboard loads, tags check in, sending text to a tag works
Tags (for each model whose firmware changed: 2.9" and 1.54"):
- [ ] OTA to one tag of the model (see Rollout); its start screen shows the new version
- [ ] The tag shows its screen as the dashboard does and checks in for at least 10 polls
- [ ] Battery and RSSI in the dashboard look as before
```

A part whose firmware did not change in this release ("unchanged" in the notes) needs no hardware check.

## 3. Rollout to the tags

New tag firmware goes to one tag of each model first, not to all at once:

1. Upload the image for one tag: pick the tag in the dashboard, or `PUT /api/tag-firmware?to=<MAC>`. Not `esl-client tag-ota <file>`: it sends without `to`, so every tag of the model gets the image at once.
2. Watch it until it has installed and checked in for at least 10 polls (`esl-client tag-ota` shows how far it is).
3. Give it to the rest: `POST /api/tag-firmware/{model}?to=all`.

Tags that are being used for a test of other firmware are kept out until the test is done: give the image to chosen MACs instead of `all`.

The AP is updated with `esl-client ota openesl-ap-...-ota.bin --wait`. If the new image never gets WiFi or an API request within three minutes, the AP goes back to the old one by itself.

### Upgrading from 0.4.x to 0.5.0

In 0.5.0 the AP draws every screen and sends it to the tag as an image. The tag no longer gets texts and a layout file (CONTENT and LAYOUT are retired, [protokoll.md](protokoll.md)). So the order matters:

1. Take `esl-client backup ap.json`, then update the AP first. The new AP still answers the POLL of 0.4.x tag firmware and still sends it firmware (FW).
2. A tag on 0.4.x gets no new screens from a 0.5.0 AP: it keeps the screen it shows, and `GET /api/tags` says in its `content_error` that it cannot show images. Update the tags to 0.5.0, one of each model first (above).
3. A 0.4.x AP does not draw images, so 0.5.0 tags get no new screens from it. Do not update tags before their AP.
4. Optional, C6 boards only: the `tagfw` partition grew from 2 to 7 tag models in 0.5.0. An AP updated over the air keeps 2, which is enough for the 2.9" and the 1.54". The Flash AP tab without "Erase device" gives it 7 and keeps what it has stored.

Going back from 0.5.0 means the AP and the tags together, to the previous release's AP and tag images. A 0.4.x AP holds at most 256 tags (tags in slots above that are not loaded), and it refuses a layout file at boot that uses the larger limits of 0.5.0 (more than 16 text lines or 10 boxes per layout, or more than 768 bytes compiled). Put such a file back from the backup after making it fit.

### Upgrading from 0.5.x to 0.6.0

Images to the tags get a second code, image format 2 ([protokoll.md](protokoll.md), "The 2D image code"). Either order works: a 0.6.0 AP sends image format 2 only to tags whose POLL has bit7, and then only when it is shorter than image format 1, so 0.5.x tags keep getting image format 1, and 0.6.0 tags still take image format 1 from a 0.5.x AP. Update the AP first as usual, then the tags.

Layout files compile to layout format 2 (lines, round and open boxes, icons, QR codes). The AP sends every tag with a file layout its screen again once after the update, because the file's version changes, and QR codes now keep a quiet zone inside their box, so a QR box holds a smaller version than before. Going back to 0.5.x: a 0.5.x AP refuses at boot a stored file that uses `icons` or the `line` and `qr` types, or `radius`, `border`, `direction` or `width`, and the layouts of that model are gone until a file without them is uploaded. It also drops a picture put on a tag with `PUT /api/tags/{mac}/image` ("ignoring stored entry" in its log). Neither is erased: updating that AP to 0.6.0 again brings the layout files and pictures back, and the tags show them again (tried on hardware).

## 4. A bad release

- **Fix forward:** the normal way. Fix it on `development` and make a patch release (X.Y.Z+1) the same way as above.
- **AP:** send the previous release's `openesl-ap-...-ota.bin` with `esl-client ota`. Its tag content, layouts and settings stay, but only if the bad release did not change what the AP stores (a one-way change, see CONTRIBUTING.md): an older AP may read a newer format wrong. Then fix forward, and take `esl-client backup ap.json` before every AP update so the data can be put back.
- **Tags:** upload the previous release's `openesl-tag-<model>-...-ota.bin` to the AP. A tag takes any version that is not its own and has not failed on it, so this also goes back. A tag that does not start must be flashed again with the flasher.
- Write what went wrong in the issue for it, and add a line to the checklist above if a check would have caught it.

Not tried on hardware yet: the automatic rollback of the tag after 9 failed starts, and of the AP ([tag-ota.md](tag-ota.md), "Still to do").
