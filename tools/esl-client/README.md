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
python3 esl_client.py --serial /dev/ttyACM0 status   # over the AP's USB port, no token
python3 esl_client.py --serial /dev/ttyACM0 serve    # bridge for the API tab of the web page
python3 esl_client.py --serial /dev/ttyACM0 wifi "Network" --password "..."   # set the WiFi without a browser
```

- Scheduled content: `--at WHEN` on `price`, `text` and `sync` (or an `at` column in the CSV, per row) makes the AP set it at that time by itself ("at" in the API). WHEN is local time (`"2026-10-19 07:00"`, `2026-10-19` = midnight), ISO with an offset, or Unix seconds. Before it schedules anything the client checks the AP's clock (`GET /api/time`) and sets it from the computer if the AP does not know the time (no WiFi, so no NTP), and warns if the clocks differ by more than 2 minutes. The same MAC (or item) may be in one file once per time, so a sale and the price after it can be one file. `schedule` lists what is waiting, `schedule-clear [MAC]` removes it, `time [--set]` shows or sets the AP's clock.
- `--wait` polls the tag until it reports the new content as shown (requirement G1: within 2 minutes).
- `sync` is the periodic full send: `PUT /api/tags` in chunks of 50. Unchanged tags cost nothing, since the AP compares `content_id`.
- CSV for `sync`: the header row names the columns, in any order. `mac` and `layout` are needed (`--layout price` instead gives rows without one a layout), `highlight` and `frame` are optional (1/0, true/false or empty), and every other column is a field, in column order. So `mac,layout,highlight,frame,name,price,info` from earlier versions still works.
- Exports from Excel work as they are: the separator is `,`, `;` or tab (whichever the header has most of), and the file may be UTF-8 or Windows-1252. Fields are sent as written, so `24,90` stays `24,90`.
- Tags per item: with `--tags tags.csv` (columns `mac,item`), the price file has an `item` column instead of `mac`, and each row goes to every tag bound to that item. The price list from the till or ERP can then be used without MACs in it; `tags.csv` changes only when tags are put up or moved. Items in `tags.csv` that the price file does not have are listed, and their tags are left as they are.
- The files are checked before anything is sent: a bad MAC, a MAC or item twice, or a row without a layout gives every such line (`prices.csv:12: ...`) and sends nothing. When the AP refuses a row (an unknown layout, text too long), the error names its line too. `--dry-run` checks the files and lists what would be sent.
- `--serial PORT` (or `OPENESL_SERIAL`) talks to the AP over its USB port instead of WiFi (Linux/macOS, see [ap/README.md](../../ap/README.md)). Opening the port does not reset the AP.
- `serve` listens on `http://localhost:8080` (`--port`) and passes `/api/` requests on to the AP, over the USB port or over WiFi with `--url`. It lets "Test Request" in the API tab of the web page reach a real AP, which a web page cannot do by itself. It only listens on localhost, and only pages from `https://mathiasbk.github.io` and `localhost` may use it (`--allow-origin` adds one); over WiFi the page's token is passed on, or `--token` when the page sends none.
- `wifi` shows the AP's WiFi state and the last scan; `wifi SSID [--password ...]` sets the network it joins (`PUT /api/wifi`), `wifi ""` forgets it. Over WiFi the AP drops the old network at once, so the reply may not arrive; use `--serial` or the setup page (`ap/README.md`, "WiFi setup").
- Network errors are retried with backoff; an HTTP error from the AP (401, 422, ...) is not.

Tests run against a fake AP: `python3 -m unittest test_esl_client`.
