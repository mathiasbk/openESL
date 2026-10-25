# Backup and restore

```sh
python3 esl_client.py backup ap.json     # to a file
python3 esl_client.py restore ap.json    # back onto an AP
python3 esl_client.py restore ap.json --dry-run
```

## What is in it

The default poll interval and the webhook, the layout files, each label's content (pictures too) and own settings, and the scheduled content.

**Not in it:** the WiFi password (it cannot be read back) and the API token. Set those again on a new AP ([WiFi and API token](WiFi-and-API-token)). Labels that have only polled, with no content, are left out; they come back by themselves at their next poll.

**Noted, not restored:** the AP's name on the network (`openesl.local`, say) and which label firmware the AP offers. A new AP gets a name of its own, and the AP cannot give the firmware file back. `restore` prints a `note:` line for each with the command that sets it again.

## When to take one

- Before every AP update.
- Before **Erase and flash as new** on the Flash AP tab, or flashing the single `.bin` file, which wipe the AP.
- Before moving to a new AP, or to another kind of board.

## Moving to a new AP, or another board

The backup does not care which board it came from: a C6 backup goes onto an S3 + C6, a C6 with screen or a C5, and back. The labels need nothing: the radio channel and PAN ID are the same on every board, and a label does not remember which AP it belongs to. A label that polls the new AP before the restore keeps showing what it has, and after the restore it gets the same content back, usually without drawing its screen again (another AP firmware version may draw it slightly differently, and then it is drawn once). Labels that hear no AP for more than two hours show "No AP found"; they get their content back and drawn after the restore, so keep the move short.

1. **Back up the old AP:** `esl_client.py --url http://openesl.local backup ap.json`. Check that the line it prints counts your labels with content.
2. **Turn the old AP off.** Two APs on the same channel would both answer the labels.
3. **Flash the new board** on the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab ([Flashing the access point](Flashing-the-access-point)). Going to an [S3 + C6](ESP32-S3-and-C6), the old C6 can be the radio: the page erases it and flashes the radio firmware, so take the backup first.
4. **Set WiFi and token** ([WiFi and API token](WiFi-and-API-token)). On an S3 or C5 over USB, use the port marked **USB**; without it, use the setup network `openesl-XXXX` from a phone.
5. **Restore:** `esl_client.py --url http://openesl-xxxx.local restore ap.json`, with the new AP's name. Take the token you set in step 4.
6. **Follow the `note:` lines:**
   - `wifi --hostname openesl` gives the new AP the old name, so scripts, Home Assistant and bookmarks keep working. The AP rejoins its WiFi to tell the router, so use the old name from then on.
   - A label firmware rollout that was not finished: send the same firmware again with `esl_client.py tag-ota openesl-tag-<model>-v<release>-ota.bin`, from the release whose firmware for that model has the version in the note (`tag-ota` shows the version before it sends). Labels that already have it are not sent it again. A rollout to chosen labels only: upload the file on the [Dashboard](Dashboard) and tick the same labels (or `PUT /api/tag-firmware?to=<MAC>`), since `tag-ota` sends to every label of the model. There is no note when the new AP already offers firmware for that model, or when every label had it.
7. **Run `restore` once more** after the labels have polled the new AP if it said pictures are waiting. That can take up to 10 minutes: labels that heard no AP while the old one was off poll less often, up to every 600 s, and `restore` says which are still waiting. A picture needs the label's model, which the AP learns from its first poll.

Restoring the same file twice changes nothing. Scheduled entries whose time has passed are skipped.
