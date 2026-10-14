# esl-client

Example client for the AP's HTTP API ([docs/protokoll.md](../../docs/protokoll.md) part 2). Python 3, standard library only.

```sh
export OPENESL_URL=http://openesl.local OPENESL_TOKEN=...
python3 esl_client.py status
python3 esl_client.py price 00000123456789A8 "Milk 1 L" 24.90 "NOK 24.90/L" --highlight --wait
python3 esl_client.py text 00000123456789A8 "Opening hours" "Mon-Fri 9-21" "Sat 9-18" --frame
python3 esl_client.py sync prices.csv
```

- `--wait` polls the tag until it reports the new content as shown (requirement G1: within 2 minutes).
- `sync` is the periodic full send: `PUT /api/tags` in chunks of 50. Unchanged tags cost nothing, since the AP compares `content_id`.
- CSV for `sync`: header `mac,layout,highlight,frame,field1,field2,...`; `highlight`/`frame` are 1/0, true/false or empty.
- Network errors are retried with backoff; an HTTP error from the AP (401, 422, ...) is not.

Tests run against a fake AP: `python3 -m unittest test_esl_client`.
