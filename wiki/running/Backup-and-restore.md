# Backup and restore

On the [dashboard](Dashboard): **Settings → Backup → Download backup**, and **Restore from a file …** to put it back, with a choice of settings, layout files, labels and scheduled content. Or with [esl-client](esl-client):

```sh
python3 esl_client.py backup ap.json     # to a file
python3 esl_client.py restore ap.json    # back onto an AP
python3 esl_client.py restore ap.json --dry-run
```

## What is in it

The default poll interval, the webhook, the time zone and NTP server, the layout files, each label's content (pictures too) and own settings, and the scheduled content.

**Not in it:** the WiFi password (it cannot be read back) and the API token. Set those again on a new AP ([WiFi and API token](WiFi-and-API-token)). Labels that have only polled, with no content, are left out; they come back by themselves at their next poll.

**Noted, not restored:** the AP's name on the network (`openesl.local`, say) and which label firmware the AP offers. A new AP gets a name of its own, and the AP cannot give the firmware file back. `restore` prints a `note:` line for each with the command that sets it again.

## When to take one

- Before every AP update.
- Before **Erase and flash as new** on the Flash AP tab, or flashing the single `.bin` file, which wipe the AP.
- Before moving to a new AP, or to another kind of board ([Moving to a new AP](Moving-to-a-new-AP)).

## Moving to a new AP, or another board

See [Moving to a new AP](Moving-to-a-new-AP): the same backup and restore, step by step, with what to do about the AP's name and label firmware.

The dashboard and esl-client write the same file, so one made with either goes back with the other. Restoring the same file twice changes nothing. Scheduled entries whose time has passed are skipped.
