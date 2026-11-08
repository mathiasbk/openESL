# Picture from a URL

An AP with PSRAM (the [ESP32-S3 + C6](ESP32-S3-and-C6), [ESP32-S3 + wireless C6](ESP32-S3-with-wireless-C6) and [ESP32-C5](ESP32-C5) boards) can fetch a PNG from a web address every few minutes and show it on labels by itself. The PNG can be a chart, a camera still, or a screen made by Home Assistant or a script. Nothing else has to run, and a label is only sent the picture again when it changed. Needs AP firmware 0.11.0 or newer.

## Setting it up

Send the source to the AP's API (`PUT /api/sources`, see [HTTP API](HTTP-API)). The list replaces the AP's sources, so include any weather places you have (read them first with `GET /api/sources`):

```bash
curl -X PUT http://openesl.local/api/sources \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"sources": [{"id": "chart", "type": "image",
        "url": "http://homeassistant.local:8123/local/chart-296x128.png",
        "every_min": 10, "tags": ["00000123456789A8"]}]}'
```

- The PNG must be exactly the label's screen size: 296x128 for a 2.9", 200x200 for a 1.54". Use one source for each size.
- `"dither": true` suits photos. Without it each pixel becomes the nearest colour the label has.
- `every_min` is 5 to 1440 minutes. The AP asks with `If-Modified-Since`, so an unchanged file costs almost nothing.
- Redirects are not followed. On your own network use `http://`: `https://` needs a certificate from a public authority.
- The address must work without a login. Files in Home Assistant's `www` folder are served at `/local/`.

The Dashboard lists the source in its **Weather** card, with its address and how the last fetch went. **Remove** stops it, and the labels keep what they show. Sending other content to a label takes it out of the source.

The details are in [docs/data-sources.md](https://github.com/mathiasbk/openESL/blob/master/docs/data-sources.md#a-picture-from-a-url).
