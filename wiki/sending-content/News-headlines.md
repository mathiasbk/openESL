# News headlines

An AP with PSRAM (the [ESP32-S3 + C6](ESP32-S3-and-C6), [ESP32-S3 + wireless C6](ESP32-S3-with-wireless-C6) and [ESP32-C5](ESP32-C5) boards) can show the newest headlines of a news feed (RSS or Atom) on labels by itself. Nothing else has to run. Needs AP firmware 0.11.0 or newer.

## Setting it up

In the **Dashboard** (AP firmware 0.11.0 or newer): tick the tags, choose **Price, countdown, news or calendar** under *Send to the ticked tags*, and fill in the dialog. The AP keeps the tags up to date from then on.

Or send the source to the AP's API (`PUT /api/sources`, see [HTTP API](HTTP-API)). The list replaces the AP's sources, so include the ones you have (read them first with `GET /api/sources`):

```bash
curl -X PUT http://openesl.local/api/sources \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"sources": [{"id": "news", "type": "rss", "url": "https://news.example/top.rss",
        "layout": "text", "tags": ["00000123456789B2"]}]}'
```

- Most news sites, blogs and status pages have a feed. Look for "RSS" on the site, or a link ending in `.rss`, `.xml` or `/feed`.
- The feed must work without a login. Of a very long feed the first 128 KB are read (the newest items).
- `every_min` is 15 to 1440 minutes. Each new headline wakes the labels, so a slower interval saves battery.

## What a label can show

`{title}` (the feed's name), `{h1}` to `{h6}` (the newest headlines) and `{count}`. A long headline is cut to 40 bytes and ends in `...`. Without `fields` the built-in `text` layout shows the feed's name and three headlines.

The Dashboard lists the source in its **Data sources** card. **Remove** stops it, and the labels keep what they show. The details are in [docs/data-sources.md](https://github.com/mathiasbk/openESL/blob/master/docs/data-sources.md#headlines-from-a-news-feed).
