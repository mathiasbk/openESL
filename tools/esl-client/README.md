# esl-client

Example client for the AP's HTTP API ([docs/openapi.yaml](../../docs/openapi.yaml)): send prices and text, sync a price list, schedule content, update firmware, back up an AP, and bridge the web page to an AP.

- **Run:** `python3 esl_client.py <command>`. Python 3, standard library only; nothing to install.
- **Test:** `python3 -m unittest test_esl_client`, against a fake AP.
- **Start in the code:** everything is in `esl_client.py`: `Client` talks to the AP over HTTP and `SerialClient` over the USB port, and `main()` at the bottom has the commands.

## Quick start

```sh
export OPENESL_URL=http://openesl-xxxx.local OPENESL_TOKEN=...   # your AP: the digits of its setup network, or openesl.local on one set up with 0.6.0 or older
python3 esl_client.py status
python3 esl_client.py price 00000123456789A8 "Milk 1 L" 24.90 "NOK 24.90/L" --highlight --wait
python3 esl_client.py text 00000123456789A8 "Opening hours" "Mon-Fri 9-21" "Sat 9-18" --frame
python3 esl_client.py date 00000123456789A8 "Velkommen"            # "Lørdag", "17. oktober 2026", "Velkommen"
python3 esl_client.py date 00000123456789A8 "Velkommen" --days 7   # ... and the next 6 days, set by the AP at midnight
python3 esl_client.py sync prices.csv
python3 esl_client.py sync prices.csv --tags tags.csv --dry-run   # price list by item number
python3 esl_client.py sync prices.csv --at "2026-10-19 07:00"     # the AP sets the new prices Monday 07:00
python3 esl_client.py schedule
python3 esl_client.py log week.csv --every 60   # long-term test: a row per tag per minute, until Ctrl-C
python3 esl_client.py report week.csv           # gaps, battery, problems and AP restarts from that log
python3 esl_client.py battery week.csv          # battery use per tag from that log: voltage steps, days until low, charge estimate
python3 esl_client.py ota build/esl_ap.bin      # new AP firmware over the API (the AP restarts into it)
python3 esl_client.py tag-ota openesl-tag-1.54-v0.3.0-ota.bin --to 00000123456789A8   # new tag firmware to one tag first
python3 esl_client.py tag-ota --model 1.54 --all  # ... then to every 1.54 tag, once that one runs it
python3 esl_client.py image 00000123456789A8 chart.png   # a picture (PNG of the screen's size) as the content
python3 esl_client.py draw 00000123456789A8 layouts.json --layout room --field room="Room 2"   # a one-off design
python3 esl_client.py backup ap.json            # the AP's tags, settings, layout files and schedule to a file
python3 esl_client.py restore ap.json           # ... and back onto an AP (a new one, or after erase-flash)
python3 esl_client.py --serial /dev/ttyACM0 status   # over the AP's USB port, no token
python3 esl_client.py --serial /dev/ttyACM0 serve    # bridge for the API tab of the web page
python3 esl_client.py --serial /dev/ttyACM0 wifi "Network" --password "..."   # set the WiFi without a browser
```

Network errors are retried with backoff; an HTTP error from the AP (401, 422, ...) is not.

## Sending content to one tag

- `price` and `text` send content with the built-in layouts. `--highlight` inverts the price, `--frame` draws a frame.
- `date MAC [LINE [LINE]]` sends today's date with layout `text`: the weekday and the date in Norwegian, then up to two lines of your own. With `--days N` the next N-1 days are scheduled on the AP for local midnight (as `--at`), so the tag changes date by itself without this computer; `--days 7` once a week keeps it going.
- `image MAC FILE` sends a picture as the tag's content (`PUT /api/tags/{mac}/image`): a PNG of exactly the tag's screen size (296x128 for the 2.9", 200x200 for the 1.54"), or the raw image bytes. The AP takes the nearest of the tag's colours for each pixel; `--dither` mixes them for shades of grey, for photos.
- `draw MAC FILE` sends a one-off design (`PUT /api/tags/{mac}/draw`, AP 0.11.0 and later): the AP draws it once and keeps it as the tag's picture, so no layout file has to be uploaded first. FILE is a layout file from the designer (pick the layout with `--layout NAME`; its icons go along), one layout (`{"fields": [...], "elements": [...]}`) or a whole body (`{"layout", "icons", "fields"}`). `--field NAME=TEXT` sets a field (repeat it). Date fields are filled in when it is drawn and are not redrawn every day; use a layout file for that.
- `--wait` polls the tag until it reports the new content as shown (requirement G1 in [docs/requirements.md](../../docs/requirements.md): within 2 minutes).
- `show MAC` and `delete MAC` read or remove one tag.

## Price lists: `sync`

`sync` is the periodic full send: `PUT /api/tags` in batches of at most 50 tags, smaller when the tags have many fields, so each body stays inside the AP's 1,024 JSON tokens and 24 KB. Unchanged tags cost nothing, since the AP compares `content_id`.

**The CSV file**

- The header row names the columns, in any order. `mac` and `layout` are needed (`--layout price` gives rows without one a layout).
- `highlight` and `frame` are optional (1/0, true/false or empty). Every other column is a field, in column order. So `mac,layout,highlight,frame,name,price,info` from earlier versions still works.
- Exports from Excel work as they are: the separator is `,`, `;` or tab (whichever the header has most of), and the file may be UTF-8 or Windows-1252. Fields are sent as written, so `24,90` stays `24,90`.

**Tags per item** (`--tags tags.csv`, columns `mac,item`)

- The price file then has an `item` column instead of `mac`, and each row goes to every tag bound to that item. The price list from the till or ERP can be used without MACs in it; `tags.csv` changes only when tags are put up or moved.
- Items in `tags.csv` that the price file does not have are listed, and their tags are left as they are.

**Checks**

- The files are checked before anything is sent: a bad MAC, a MAC or item twice, or a row without a layout gives every such line (`prices.csv:12: ...`) and sends nothing.
- When the AP refuses a row (an unknown layout, text too long), the error names its line too.
- `--dry-run` checks the files and lists what would be sent.

## Scheduled content

- `--at WHEN` on `price`, `text` and `sync` (or an `at` column in the CSV, per row) makes the AP set the content at that time by itself (`at` in the API).
- WHEN is local time (`"2026-10-19 07:00"`, `2026-10-19` = midnight), ISO with an offset, or Unix seconds.
- Before it schedules anything the client checks the AP's clock (`GET /api/time`) and sets it from the computer if the AP does not know the time (no WiFi, so no NTP). It warns if the clocks differ by more than 2 minutes.
- The same MAC (or item) may be in one file once per time, so a sale and the price after it can be one file.
- `schedule` lists what is waiting, `schedule-clear [MAC]` removes it, `time [--set]` shows or sets the AP's clock.

## The AP's settings and layouts

- `layouts` lists the layout files and `layout-put {2.9,1.54} FILE` uploads one.
- `config` shows or sets:
  - `--poll-s`: the AP's default poll interval (`0` = tags keep their own, `null` = the built-in value);
  - `--webhook URL` (`""` turns it off);
  - `--tx-power DBM`: the radio's transmit power (`null` = the built-in value);
  - `--channel 20`: the radio's channel (`null` = the built-in one). The AP tells its tags and moves once they have it, or at once with `--channel-now`;
  - `--tz`: the time zone, a POSIX TZ string such as `CET-1CEST,M3.5.0,M10.5.0/3` (`""` = UTC);
  - `--ntp`: the NTP server (`""` = the built-in one).

## Monitoring

- `health` lists the tags that need attention (`GET /api/health`, thresholds `--silent-s` and `--battery-mv`) and exits 1 if there are any.
- `log FILE` is the long-term test: every `--every` seconds (default 60) it reads `GET /api/tags` and `GET /api/health` and appends one CSV row per tag (time, AP uptime and reset reason, seen, battery, RSSI, content, pending, draw failed, problems). A round the AP did not answer gets a row of its own. The file can be appended to across restarts of the client, and `--rounds N` stops after N rounds.
- `report FILE` sums such a log up: rounds, outages and AP restarts (uptime went down), and per tag the longest time without a poll, the lowest battery, and the rounds with problems or a failed drawing. It exits 1 when there is anything to look at.
- `battery FILE` reads the same log and looks at the battery over time, per tag: the first time each lower reading was reached, the rate in mV per day worked out from the steps below 2.6 V, and the time left until `--low-mv` (default 2500, the same as `/api/health`) at that rate. The first reading in a log has no known time (the tag may have been at that level for weeks), so it is shown as "since the log started" and the rate needs two steps seen in the log. Below that it adds a rough charge count with the figures of `tools/power-budget/power_budget.py` ([docs/power-budget.md](../../docs/power-budget.md), estimates, not measurements): the polls over the logged period (`--poll-s`, default 60, the same for every tag, also while the AP was down), one screen update per change of the shown content, and the assumed sleep current; the result is mAh, the share of the cell(s), the average current and the years that rate and the cells' self-discharge give (a ceiling: the cells' shelf life is about 10 years). `--csv FILE` writes one row per tag. What to expect: the 2.9" panel's detector reports 100 mV steps up to "2600 = 2.6 V or more", and the tag reads it only every `BATT_EVERY` polls (`tag/src/hal/zbs243/hal_zbs243.c`), so a fresh CR2450 (about 2.9-3.0 V) shows 2600 for most of its life and the steps only appear towards the end, each up to that many polls late; the charge count is what there is before that. A reading that bounces back up under a lighter load is not a new step, only the first time a lower level is reached counts, so one low reading stays in the rate. The 1.54" cannot measure its battery and reports 0 ([docs/sources.md](../../docs/sources.md)), so it gets the charge count only. `--html FILE` writes a page with the table and a step chart per tag (the 2.6 V ceiling shaded, the low line dashed, a dot per step, a triangle per screen update) to open in a browser.

## Firmware

### `ota`: the AP

- `ota IMAGE` sends new AP firmware (`build/esl_ap.bin`, or the `...-ota.bin` of a release) with `POST /api/ota`. Over the USB port it goes in parts of 16 KB.
- `--wait` then asks `GET /api/ota` until the AP answers again (`--wait-s`, default 120), and says whether the new image runs or the bootloader fell back to the old slot (exit 1).
- `ota` alone shows which app slot runs and what the other holds, and on an ESP32-S3 + C6 what the radio runs.
- The C6 radio's image (`openesl-ap-radio-esp32c6-<version>-ota.bin`) goes the same way: the AP passes it on to the radio over the cable, and `--wait` asks until the AP hears the radio again with the new version ([docs/ap-radio-link.md](../../docs/ap-radio-link.md#updating-the-radio-over-the-cable)).

### `tag-ota`: the tags

New firmware goes to one tag of each model first, since a tag that does not start again has to go back in the jig ([docs/release.md](../../docs/release.md), "Rollout to the tags"). So without `--to` or `--all` nothing is sent.

- `tag-ota FILE --to MAC[,MAC]` uploads tag firmware (`openesl-tag-<model>-<version>-ota.bin` from a release, or `tag/build/fw/openesl-ota.bin`) with `PUT /api/tag-firmware` in parts of 8 KB, for these tags only (at most 16). They take it at their next polls ([docs/tag-ota.md](../../docs/tag-ota.md)).
  - First it checks that the AP knows each tag, that it is of the file's model, and that the AP firmware can offer it to chosen tags (0.4.4 and newer).
  - Afterwards it checks that the AP really offers it to them alone: an AP that offers it to the whole model has the update stopped again.
- `tag-ota FILE --all` sends it to every tag of the model at once.
- Without a file: `--to MAC` offers the image the AP has to more tags, and `--model MODEL --all` to every tag of the model. That is refused while no tag runs it yet (`--force` to do it anyway).
- `tag-ota` alone shows the image per model, who it is for, and how many tags run it, still need it, rejected it, cannot take updates or were not chosen. Once the chosen tags run it, it prints the command for the rest.
- `tag-ota --delete MODEL` stops the updates for a model.

## Backup and restore

For replacing an AP (also with another kind of board) or flashing one with `erase-flash`: `backup` before, `restore` after. The dashboard's Settings page (Backup) reads and writes the same file.

**`backup FILE`** saves what the AP keeps that the API can read back, as one JSON file (`-` for stdout):

- the default poll interval, the webhook, the time zone and NTP server (from an AP that has them);
- the layout files;
- each tag's content and own settings (`GET /api/tags/{mac}` for every tag with content, and the raw image of a picture). Tags that have only polled are left out, since they are added back when they poll again;
- the scheduled content;
- the AP's hostname and the tag firmware it offers (`GET /api/tag-firmware`: model, version, crc, who it is for, how many tags still wait; not the bytes, which the AP cannot give back).

Not in it: the WiFi (its password cannot be read) and the token.

**`restore FILE`** puts it onto an AP:

- in the order the AP needs: config, layout files, tag settings, content in batches of at most 50, pictures, then the schedule;
- tags the file does not have are left as they are, and the same file twice changes nothing;
- scheduled entries whose time has passed are skipped, since the AP would set them at once;
- a picture needs the tag's model, so on a fresh AP it waits until its tag has polled: `restore` says which and exits 1, and running it again later sends them;
- it prints a `note:` line when the new AP has another name, or offers no firmware for a model whose rollout was not finished, with what sets it again (`wifi --hostname`; `tag-ota` with the release's `openesl-tag-<model>-v<release>-ota.bin`, or the dashboard for a rollout to chosen tags). Older backup files without these restore as before;
- `--dry-run` shows what would be restored.

## Over the USB port

- `--serial PORT` (or `OPENESL_SERIAL`) talks to the AP over its USB port instead of WiFi, with no token (Linux and macOS, see [ap/README.md](../../ap/README.md)). Opening the port does not reset the AP.
- `serve` listens on `http://localhost:8080` (`--port`) and passes `/api/` requests on to the AP, over the USB port or over WiFi with `--url`. It lets the web page (the API tab's "Test Request", the Dashboard, the designer) reach a real AP, which a web page cannot do by itself.
  - It only listens on localhost, and only pages from `https://mathiasbk.github.io` and `localhost` may use it (`--allow-origin` adds one).
  - Over WiFi the page's token is passed on, or `--token` when the page sends none.
- `wifi` shows the AP's WiFi state and the last scan.
  - `wifi SSID [--password ...]` sets the network it joins (`PUT /api/wifi`); `wifi ""` forgets it.
  - `--new-token TOKEN` also sets the API token (alone, it changes only the token), as the web page does over USB right after flashing. The state line says when no token is set yet.
  - `--hostname NAME` sets the AP's name on the network (what the router shows, and `NAME.local`; `openesl` unless changed).
  - Over WiFi the AP drops the old network at once, so the reply may not arrive; use `--serial` or the setup page (`ap/README.md`, "WiFi setup").

## Other

- `mac 020663461013 [--model 2.9|1.54]` prints the MAC of the tag whose label shows that number under the barcode (both sizes without `--model`). It needs no AP; the rule is in [docs/tag-firmware.md](../../docs/tag-firmware.md), "Tag identity".
