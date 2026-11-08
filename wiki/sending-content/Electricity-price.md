# Electricity price

An AP with PSRAM (the [ESP32-S3 + C6](ESP32-S3-and-C6), [ESP32-S3 + wireless C6](ESP32-S3-with-wireless-C6) and [ESP32-C5](ESP32-C5) boards) can show today's electricity spot price in a Norwegian price area on labels by itself, from [hvakosterstrommen.no](https://www.hvakosterstrommen.no/). Nothing else has to run, and the price "now" follows the clock. Needs AP firmware 0.11.0 or newer.

## Setting it up

In the **Dashboard** (AP firmware 0.11.0 or newer): tick the tags, choose **Price, countdown, news or calendar** under *Send to the ticked tags*, and fill in the dialog. The AP keeps the tags up to date from then on.

Or send the source to the AP's API (`PUT /api/sources`, see [HTTP API](HTTP-API)). The list replaces the AP's sources, so include the ones you have (read them first with `GET /api/sources`):

```bash
curl -X PUT http://openesl.local/api/sources \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"sources": [{"id": "power", "type": "power", "area": "NO1",
        "layout": "text", "tags": ["00000123456789B2"]}]}'
```

- `area` is `NO1` (East), `NO2` (South), `NO3` (Central), `NO4` (North) or `NO5` (West).
- 25 % VAT is added, except in `NO4`. Set `"vat": false` or `true` to choose yourself.
- It is the spot price only: no grid fee, no supplier markup, no electricity tax.
- The AP fetches the day's prices once and changes `{now}` at each quarter hour (`every_min`, 15 or more). Times are Norway's, whatever time zone the AP has.
- Each change of the text wakes the labels. For fewer updates (longer battery life), set `"every_min": 60` or show `{min}`, `{max}` and `{avg}`, which change once a day.
- `"lang": "en"` gives `2.88` and `low`/`normal`/`high`; the default `nb` gives `2,88` and `lav`/`normal`/`høy`.

## What a label can show

`{area}`, `{now}` (kr/kWh), `{nowore}` (øre/kWh), `{next}`, `{min}` and `{minat}` (the cheapest price and when), `{max}` and `{maxat}`, `{avg}`, `{level}` (low/normal/high against the day's average) and `{date}`. Without `fields` the built-in `text` layout shows four lines: the area and date, the price now, the level, and the day's range. Your own fields, for example:

```json
"fields": ["Strøm nå {now} kr", "Neste {next}", "Billigst {minat}: {min}"]
```

The Dashboard lists the source in its **Weather and pictures** card with how the last fetch went. **Remove** stops it, and the labels keep what they show.

The prices come from hvakosterstrommen.no (from ENTSO-E). Credit them where you show the price to others. The details are in [docs/data-sources.md](https://github.com/mathiasbk/openESL/blob/master/docs/data-sources.md#the-electricity-price).
