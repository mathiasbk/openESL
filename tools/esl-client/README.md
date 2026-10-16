# esl-client

Example client for the AP's HTTP API ([docs/openapi.yaml](../../docs/openapi.yaml)). Python 3, standard library only.

```sh
export OPENESL_URL=http://openesl.local OPENESL_TOKEN=...
python3 esl_client.py status
python3 esl_client.py price 00000123456789A8 "Milk 1 L" 24.90 "NOK 24.90/L" --highlight --wait
python3 esl_client.py text 00000123456789A8 "Opening hours" "Mon-Fri 9-21" "Sat 9-18" --frame
python3 esl_client.py sync prices.csv
python3 esl_client.py sync prices.csv --tags tags.csv --dry-run   # price list by item number
python3 esl_client.py sync prices.csv --at "2026-10-19 07:00"     # the AP sets the new prices Monday 07:00
python3 esl_client.py schedule
python3 esl_client.py log week.csv --every 60   # long-term test: a row per tag per minute, until Ctrl-C
python3 esl_client.py report week.csv           # gaps, battery, problems and AP restarts from that log
python3 esl_client.py ota build/esl_ap.bin      # new AP firmware over the API (the AP restarts into it)
python3 esl_client.py tag-ota openesl-tag-1.54-v0.3.0-ota.bin   # new tag firmware, sent to the tags over the radio
python3 esl_client.py backup ap.json            # the AP's tags, settings, layout files and schedule to a file
python3 esl_client.py restore ap.json           # ... and back onto an AP (a new one, or after erase-flash)
python3 esl_client.py --serial /dev/ttyACM0 status   # over the AP's USB port, no token
python3 esl_client.py --serial /dev/ttyACM0 serve    # bridge for the API tab of the web page
python3 esl_client.py --serial /dev/ttyACM0 wifi "Network" --password "..."   # set the WiFi without a browser
```

- Scheduled content: `--at WHEN` on `price`, `text` and `sync` (or an `at` column in the CSV, per row) makes the AP set it at that time by itself ("at" in the API). WHEN is local time (`"2026-10-19 07:00"`, `2026-10-19` = midnight), ISO with an offset, or Unix seconds. Before it schedules anything the client checks the AP's clock (`GET /api/time`) and sets it from the computer if the AP does not know the time (no WiFi, so no NTP), and warns if the clocks differ by more than 2 minutes. The same MAC (or item) may be in one file once per time, so a sale and the price after it can be one file. `schedule` lists what is waiting, `schedule-clear [MAC]` removes it, `time [--set]` shows or sets the AP's clock.
- Other commands: `show MAC` and `delete MAC` read or remove one tag; `layouts` lists the layout files and `layout-put {2.9,1.54} FILE` uploads one; `health` lists the tags that need attention (`GET /api/health`, thresholds `--silent-s` and `--battery-mv`) and exits 1 if there are any; `config` shows or sets the AP's default poll interval (`--poll-s`, `0` = tags keep their own, `null` = the built-in value) and the webhook (`--webhook URL`, `""` turns it off).
- `--wait` polls the tag until it reports the new content as shown (requirement G1: within 2 minutes).
- `sync` is the periodic full send: `PUT /api/tags` in chunks of 50. Unchanged tags cost nothing, since the AP compares `content_id`.
- CSV for `sync`: the header row names the columns, in any order. `mac` and `layout` are needed (`--layout price` instead gives rows without one a layout), `highlight` and `frame` are optional (1/0, true/false or empty), and every other column is a field, in column order. So `mac,layout,highlight,frame,name,price,info` from earlier versions still works.
- Exports from Excel work as they are: the separator is `,`, `;` or tab (whichever the header has most of), and the file may be UTF-8 or Windows-1252. Fields are sent as written, so `24,90` stays `24,90`.
- Tags per item: with `--tags tags.csv` (columns `mac,item`), the price file has an `item` column instead of `mac`, and each row goes to every tag bound to that item. The price list from the till or ERP can then be used without MACs in it; `tags.csv` changes only when tags are put up or moved. Items in `tags.csv` that the price file does not have are listed, and their tags are left as they are.
- The files are checked before anything is sent: a bad MAC, a MAC or item twice, or a row without a layout gives every such line (`prices.csv:12: ...`) and sends nothing. When the AP refuses a row (an unknown layout, text too long), the error names its line too. `--dry-run` checks the files and lists what would be sent.
- `log FILE` is the long-term test (phase 8 of the work plan): every `--every` seconds (default 60) it reads `GET /api/tags` and `GET /api/health` and appends one CSV row per tag (time, AP uptime and reset reason, seen, battery, RSSI, content, pending, draw failed, problems); a round the AP did not answer gets a row of its own. The file can be appended to across restarts of the client, and `--rounds N` stops after N rounds. `report FILE` sums it up: rounds, outages and AP restarts (uptime went down), and per tag the longest time without a poll, the lowest battery, the rounds with problems or a failed drawing; it exits 1 when there is anything to look at.
- `ota IMAGE` sends new AP firmware (`build/esl_ap.bin`, or the `...-ota.bin` of a release) with `POST /api/ota`; over the USB port it goes in parts of 16 KB. `--wait` then asks `GET /api/ota` until the AP answers again (`--wait-s`, default 120) and says whether the new image runs or the bootloader fell back to the old slot (exit 1). `ota` alone shows which app slot runs and what the other holds.
- `tag-ota FILE` uploads tag firmware (`openesl-tag-<model>-<version>-ota.bin` from a release, or `tag/build/fw/openesl-ota.bin`) with `PUT /api/tag-firmware` in parts of 16 KB; the AP then offers it to every tag of that model, which takes it at its next polls ([docs/tag-ota.md](../../docs/tag-ota.md)). `tag-ota` alone shows the image per model and how many tags run it, still need it, rejected it or cannot take updates. `tag-ota --delete MODEL` stops the updates for a model.
- `backup FILE` saves what the AP keeps that the API can read back: the default poll interval and the webhook, the layout files, each tag's content and own settings (`GET /api/tags/{mac}` for every tag with content), and the scheduled content, as one JSON file (`-` for stdout). Tags that have only polled are left out, since they are added back when they poll again. Not in it: the WiFi (its password cannot be read) and the token. `restore FILE` puts it onto an AP in the order the AP needs (config, layout files, tag settings, content in chunks of 50, then the schedule), and leaves tags the file does not have as they are; the same file twice changes nothing. Scheduled entries whose time has passed are skipped, since the AP would set them at once. `--dry-run` shows what would be restored. For replacing an AP, or flashing one with `erase-flash`: `backup` before, `restore` after.
- `--serial PORT` (or `OPENESL_SERIAL`) talks to the AP over its USB port instead of WiFi (Linux/macOS, see [ap/README.md](../../ap/README.md)). Opening the port does not reset the AP.
- `serve` listens on `http://localhost:8080` (`--port`) and passes `/api/` requests on to the AP, over the USB port or over WiFi with `--url`. It lets "Test Request" in the API tab of the web page reach a real AP, which a web page cannot do by itself. It only listens on localhost, and only pages from `https://mathiasbk.github.io` and `localhost` may use it (`--allow-origin` adds one); over WiFi the page's token is passed on, or `--token` when the page sends none.
- `wifi` shows the AP's WiFi state and the last scan; `wifi SSID [--password ...]` sets the network it joins (`PUT /api/wifi`), `wifi ""` forgets it. `--new-token TOKEN` also sets the API token (alone, it changes only the token), as the web page does over USB right after flashing; the state line says when no token is set yet. Over WiFi the AP drops the old network at once, so the reply may not arrive; use `--serial` or the setup page (`ap/README.md`, "WiFi setup").
- `mac 020663461013 [--model 2.9|1.54]` prints the MAC of the tag whose label shows that number under the barcode (both sizes without `--model`). It needs no AP; the rule is in `docs/tag-firmware.md`, "Tag identity".
- Network errors are retried with backoff; an HTTP error from the AP (401, 422, ...) is not.

Tests run against a fake AP: `python3 -m unittest test_esl_client`.
