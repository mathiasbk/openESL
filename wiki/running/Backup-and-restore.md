# Backup and restore

```sh
python3 esl_client.py backup ap.json     # to a file
python3 esl_client.py restore ap.json    # back onto an AP
python3 esl_client.py restore ap.json --dry-run
```

## What is in it

The default poll interval and the webhook, the layout files, each label's content and own settings, and the scheduled content.

**Not in it:** the WiFi password (it cannot be read back) and the API token. Set those again on a new AP ([WiFi and API token](WiFi-and-API-token)). Labels that have only polled, with no content, are left out; they come back by themselves at their next poll.

## When to take one

- Before every AP update.
- Before flashing as a **New** board on the Flash AP tab, or with the single `.bin` file, which wipe the AP.
- Before moving to a new AP.

## Moving to a new AP

1. `backup` from the old AP.
2. Flash the new one and set its WiFi and token.
3. `restore` onto it.
4. Turn the old one off. The labels find the new AP on the same channel at their next poll.

Restoring the same file twice changes nothing. Scheduled entries whose time has passed are skipped.
