# Moving to a new AP

Use this to replace the AP with a new board, of the same kind or another (a C6 to an S3 + C6, a C6 with screen, a C5, and back). It is a [backup and restore](Backup-and-restore): the labels follow without being paired again.

## Before you start

- A PC with `esl_client.py` ([esl-client](esl-client)) on the same network as the old AP.
- The old AP's API token (`--token`, or `OPENESL_TOKEN`).
- The new board, and a USB cable for it.

## What happens to the labels

The labels need nothing: the radio channel and PAN ID are the same on every board, and a label does not remember which AP it belongs to. A label that polls the new AP before the restore keeps showing what it has, and after the restore it gets the same content back, usually without drawing its screen again (another AP firmware version may draw it slightly differently, and then it is drawn once). Labels that hear no AP for more than two hours (by default) put a small crossed-out signal mark in their top right corner (older label firmware shows "No AP found" instead); the mark goes away, or the content comes back, after the restore, so keep the move short.

## Steps

1. **Back up the old AP:** `esl_client.py --url http://openesl.local backup ap.json`. Check that the line it prints counts your labels with content.
2. **Turn the old AP off.** Two APs on the same channel would both answer the labels.
3. **Flash the new board** on the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab ([Flashing the access point](Flashing-the-access-point)). Going to an [S3 + C6](ESP32-S3-and-C6), the old C6 can be the radio: the page erases it and flashes the radio firmware, so take the backup first.
4. **Set WiFi and token** ([WiFi and API token](WiFi-and-API-token)). On an S3 or C5 over USB, use the port marked **USB**; without it, use the setup network `openesl-XXXX` from a phone.
5. **Restore:** `esl_client.py --url http://openesl-xxxx.local restore ap.json`, with the new AP's name. Take the token you set in step 4.
6. **Follow the `note:` lines:**
   - `wifi --hostname openesl` gives the new AP the old name, so scripts, Home Assistant and bookmarks keep working. The AP rejoins its WiFi to tell the router, so use the old name from then on.
   - With the [Home Assistant integration](Home-Assistant): a new AP with the old name is found again by itself, at its new address. With a new name it is a new AP to Home Assistant: add it, and the labels' devices follow it as they poll it. Remove the old AP from Home Assistant once it is gone.
   - A label firmware rollout that was not finished: send the same firmware again with `esl_client.py tag-ota openesl-tag-<model>-v<release>-ota.bin`, from the release whose firmware for that model has the version in the note (`tag-ota` shows the version before it sends). Labels that already have it are not sent it again. A rollout to chosen labels only: upload the file on the [Dashboard](Dashboard) and tick the same labels (or `PUT /api/tag-firmware?to=<MAC>`), since `tag-ota` sends to every label of the model. There is no note when the new AP already offers firmware for that model, or when every label had it.
7. **Run `restore` once more** after the labels have polled the new AP if it said pictures are waiting. That can take up to 10 minutes: labels that heard no AP while the old one was off poll less often, up to every 600 s, and `restore` says which are still waiting. A picture needs the label's model, which the AP learns from its first poll.

Restoring the same file twice changes nothing, so running it again in step 7 is safe.

## What does not move

- **WiFi and API token:** set them again in step 4. The WiFi password cannot be read back from the old AP.
- **The AP's name:** a new AP gets a name of its own (`openesl-xxxx.local`) until you run `wifi --hostname` in step 6.
- **Label firmware for updates:** the AP cannot give the file back. The note in step 6 says which release file to send again.

## If something goes wrong

- **A label shows the "no AP" mark or "No AP found":** it heard no AP for two hours (by default). After the restore it draws its content again without the mark at its next poll; give it up to 10 minutes.
- **`restore` says pictures are waiting:** the labels have not polled the new AP yet. Run `restore` again when they have; that can take up to 10 minutes.
- **The old AP is still on:** both answer the labels. Turn it off, or flash it as something else.
- **Going back:** the backup file goes onto the old AP just as well. Flash it back if it was erased, and restore the same file.
