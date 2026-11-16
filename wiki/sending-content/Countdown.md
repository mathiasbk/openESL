# Countdown

An AP with PSRAM (the [ESP32-S3 + C6](ESP32-S3-and-C6), [ESP32-S3 + wireless C6](ESP32-S3-with-wireless-C6) and [ESP32-C5](ESP32-C5) boards) can count the days to a date, or since one, on labels by itself: Christmas, a holiday, a birthday, a trip. Nothing is fetched from the internet, and a label only wakes up when its text changes. Needs AP firmware 0.11.0 or newer.

## Setting it up

In the **Dashboard** (AP firmware 0.11.0 or newer): tick the tags, choose **Price, countdown, news or calendar** under *Send to the ticked tags*, and fill in the dialog. The AP keeps the tags up to date from then on.

Or send the source to the AP's API (`PUT /api/sources`, see [HTTP API](HTTP-API)). The list replaces the AP's sources, so include the ones you have (read them first with `GET /api/sources`):

```bash
curl -X PUT http://openesl.local/api/sources \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"sources": [{"id": "christmas", "type": "countdown", "place": "Christmas",
        "date": "2026-12-24", "yearly": true, "lang": "en",
        "layout": "text", "tags": ["00000123456789B2"]}]}'
```

- `date` is `YYYY-MM-DD`, in local time. Set the time zone under Settings.
- `"time": "18:30"` makes the last day count down in hours, and in minutes in the last hour.
- `"yearly": true` counts to the next time the date comes round. With a birth date, `{years}` is the age the person turns.
- `"lang": "en"` gives English words; the default `nb` gives Norwegian.

## What a label can show

`{place}`, `{left}` (in words: "in 58 days", "tomorrow", "today", "in 5 h", "in 10 min", "10 days ago"), `{days}`, `{weeks}`, `{hours}`, `{since}`, `{date}`, `{weekday}` and `{years}`. Without `fields`, the built-in `text` layout shows the name, the time left and the day. Your own fields, for example:

```json
"fields": ["Ola turns {years}", "{left}", "{weekday} {date}"]
```

The Dashboard lists the source in its **Data sources** card. **Remove** stops it, and the labels keep what they show.

The details are in [docs/data-sources.md](https://github.com/mathiasbk/openESL/blob/master/docs/data-sources.md#a-countdown).
