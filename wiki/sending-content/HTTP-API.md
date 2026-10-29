# HTTP API

The reference for every request is the [API tab](https://mathiasbk.github.io/openESL/#api) of the web page, made from [docs/openapi.yaml](https://github.com/mathiasbk/openESL/blob/master/docs/openapi.yaml). There you can also try the requests against your own AP, through the bridge in [esl-client](esl-client) (`esl_client.py --serial /dev/ttyACM0 serve`).

## The token

Every call needs `Authorization: Bearer <token>` ([WiFi and API token](WiFi-and-API-token)). Five wrong tokens in a row lock the API for 60 seconds.

## The calls you use most

| Call | Does |
|---|---|
| `PUT /api/tags/{mac}` | Set what one label shows |
| `PUT /api/tags` | Set up to 50 labels in one request |
| `GET /api/tags` | Every label: what it shows, last poll, battery, signal, firmware |
| `GET /api/tags/{mac}` | One label, with its scheduled content |
| `DELETE /api/tags/{mac}` | Forget a label |
| `GET /api/health` | Labels that need attention ([Health and webhook](Health-and-webhook)) |
| `PUT /api/layouts/{model}` | Upload a layout file ([Label designer](Label-designer)) |
| `GET /api/schedule` | Content waiting for its time ([Scheduled prices](Scheduled-prices)) |

## One label

```bash
curl -X PUT http://openesl.local/api/tags/00000123456789A8 \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"layout": "price", "fields": ["Lettmelk 1 L", "24,90", "kr 24,90/L"], "price_highlight": true}'
```

- `layout`: `price` (3 fields), `text` (4 fields) or one of your own (up to 6 fields).
- `fields`: the layout's text fields, in order. æøå work.
- `price_highlight` draws the price inverted with `price`; `frame` draws a frame with `text`.
- `at`: a time to show it, instead of now ([Scheduled prices](Scheduled-prices)).

## Many labels

`PUT /api/tags` takes a list of up to 50 labels. Unchanged labels are skipped, so you can send the whole price list every time.

## Reading only what changed

A client that reads `GET /api/tags` every minute or two, such as Home Assistant, does not have to fetch every label each time. Add `?since=` the first time; the reply has `"since": "5eed0001-17"` and `"full": true`. Send that value next time (`GET /api/tags?since=5eed0001-17`): with `"full": false` the reply holds only the labels that changed since then (a poll, new content, settings, a drawing, firmware), and the next `since`. With `"full": true` it holds every label again, after the AP restarted or a label was deleted, and the client starts over with that list. `seen_s_ago` going up by itself is no change, but every poll is: the saving is largest when the labels poll less often than the client reads (`poll_s` 600 and a read every 2 minutes: about a fifth of the labels each time; with the built-in 60 s nearly all of them). An AP without this ignores `since` and sends the whole list.

## Errors

- `401`: wrong or missing token. `429`: locked after wrong tokens, wait 60 seconds.
- `422`: the content cannot be shown (unknown layout, too many fields, text too long, bad MAC), with the reason. The label keeps what it shows.
- `507`: the AP has no room left to save it.

## Over USB, without WiFi

The same API answers on the AP's USB port, with no token. [esl-client](esl-client) speaks it with `--serial`.

More detail: [ap/README.md](https://github.com/mathiasbk/openESL/blob/master/ap/README.md#http-api) and [docs/protokoll.md](https://github.com/mathiasbk/openESL/blob/master/docs/protokoll.md) part 2.
