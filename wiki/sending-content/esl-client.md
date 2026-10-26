# esl-client

[esl-client](https://github.com/mathiasbk/openESL/blob/master/tools/esl-client/README.md) is a small Python client for the whole API. It needs only Python 3, nothing to install.

```sh
export OPENESL_URL=http://openesl.local OPENESL_TOKEN=...
python3 esl_client.py status
```

`--url` (or `OPENESL_URL`) takes a name or an IP address with or without `http://`: `--url 192.168.0.188` works too.

## Content

```sh
python3 esl_client.py price 00000123456789A8 "Milk 1 L" 24.90 "NOK 24.90/L" --highlight --wait
python3 esl_client.py text 00000123456789A8 "Opening hours" "Mon-Fri 9-21" "Sat 9-18" --frame
python3 esl_client.py date 00000123456789A8 "Velkommen" --days 7
python3 esl_client.py show 00000123456789A8
```

- `--wait` waits until the label reports that it shows the new content.
- `date` sends today's date in Norwegian with up to two lines of your own. With `--days 7`, the next days are scheduled on the AP, so the label changes date at midnight by itself.

## Price lists and schedules

`sync`, `--at` and `schedule`: see [Price lists from CSV](Price-lists-from-CSV) and [Scheduled prices](Scheduled-prices).

## Running the AP

| Command | Does |
|---|---|
| `health` | Labels that need attention; exits 1 if there are any |
| `config --poll-s 60` / `config --webhook URL` | Default poll interval / health webhook |
| `layouts`, `layout-put 2.9 FILE` | List or upload layout files |
| `ota FILE --wait` | New AP firmware, or the C6 radio's on an S3 + C6 ([Updating the AP](Updating-the-access-point)) |
| `tag-ota FILE` | New label firmware ([Updating labels](Updating-labels)) |
| `backup FILE`, `restore FILE` | [Backup and restore](Backup-and-restore) |
| `log week.csv`, `report week.csv` | Long-term test: a row per label per minute, then a summary |
| `mac 020663461013` | A label's MAC from the number under its barcode |

## Over USB

`--serial /dev/ttyACM0` talks to the AP over its USB port instead of WiFi, with no token (Linux and macOS).

```sh
python3 esl_client.py --serial /dev/ttyACM0 status
python3 esl_client.py --serial /dev/ttyACM0 wifi "Network" --password "..."
python3 esl_client.py --serial /dev/ttyACM0 serve    # bridge for the web page
```

`serve` listens on `http://localhost:8080` and passes requests on to the AP, so the web page's API tab and dashboard can reach it. With `--url http://openesl.local --token ...` it goes over WiFi instead.

Every command and option: [tools/esl-client/README.md](https://github.com/mathiasbk/openESL/blob/master/tools/esl-client/README.md).
