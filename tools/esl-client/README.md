# esl-client

Example client for the AP's HTTP API ([docs/openapi.yaml](../../docs/openapi.yaml)). Python 3, standard library only.

```sh
export OPENESL_URL=http://openesl.local OPENESL_TOKEN=...
python3 esl_client.py status
python3 esl_client.py price 00000123456789A8 "Milk 1 L" 24.90 "NOK 24.90/L" --highlight --wait
python3 esl_client.py text 00000123456789A8 "Opening hours" "Mon-Fri 9-21" "Sat 9-18" --frame
python3 esl_client.py sync prices.csv
python3 esl_client.py --serial /dev/ttyACM0 status   # over the AP's USB port, no token
python3 esl_client.py --serial /dev/ttyACM0 serve    # bridge for the API tab of the web page
python3 esl_client.py --serial /dev/ttyACM0 wifi "Network" --password "..."   # set the WiFi without a browser
```

- `--wait` polls the tag until it reports the new content as shown (requirement G1: within 2 minutes).
- `sync` is the periodic full send: `PUT /api/tags` in chunks of 50. Unchanged tags cost nothing, since the AP compares `content_id`.
- CSV for `sync`: header `mac,layout,highlight,frame,field1,field2,...`; `highlight`/`frame` are 1/0, true/false or empty.
- `--serial PORT` (or `OPENESL_SERIAL`) talks to the AP over its USB port instead of WiFi (Linux/macOS, see [ap/README.md](../../ap/README.md)). Opening the port does not reset the AP.
- `serve` listens on `http://localhost:8080` (`--port`) and passes `/api/` requests on to the AP, over the USB port or over WiFi with `--url`. It lets "Test Request" in the API tab of the web page reach a real AP, which a web page cannot do by itself. It only listens on localhost, and only pages from `https://mathiasbk.github.io` and `localhost` may use it (`--allow-origin` adds one); over WiFi the page's token is passed on, or `--token` when the page sends none.
- `wifi` shows the AP's WiFi state and the last scan; `wifi SSID [--password ...]` sets the network it joins (`PUT /api/wifi`), `wifi ""` forgets it. Over WiFi the AP drops the old network at once, so the reply may not arrive; use `--serial` or the setup page (`ap/README.md`, "WiFi setup").
- Network errors are retried with backoff; an HTTP error from the AP (401, 422, ...) is not.

Tests run against a fake AP: `python3 -m unittest test_esl_client`.
