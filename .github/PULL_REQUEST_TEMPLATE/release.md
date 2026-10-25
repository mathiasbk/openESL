<!-- Release pull request: development -> master, titled "Release X.Y.Z". See docs/release.md. -->

<!-- A few sentences: what this release brings, and what a user must do to upgrade. -->

## Upgrade instructions

<!-- The section "Upgrading from A to X.Y.Z" of docs/release.md, as tools/release_notes.py prints it at the top:
     the order (AP, C6 radio, tags), what is not backward compatible, and what going back costs. -->

## Changes

<!-- The rest of: python3 tools/release_notes.py v<previous> origin/development -->

## Checklist

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
