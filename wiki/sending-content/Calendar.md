# Calendar

An AP with PSRAM (the [ESP32-S3 + C6](ESP32-S3-and-C6), [ESP32-S3 + wireless C6](ESP32-S3-with-wireless-C6) and [ESP32-C5](ESP32-C5) boards) can show the next events of a calendar on labels by itself: a meeting room's door sign, the family's week on the fridge. Nothing else has to run. Needs AP firmware 0.11.0 or newer.

## Setting it up

1. Find the calendar's address in iCal format (`.ics`):
   - **Google Calendar**: Settings, the calendar, *Integrate calendar*, **Secret address in iCal format**.
   - **Outlook.com / Microsoft 365**: Settings, Calendar, *Shared calendars*, **Publish a calendar**, the ICS link.
   - **iCloud**: share the calendar as a **Public Calendar** and copy the `webcal://` link.
   - **Nextcloud**: the calendar's menu, **Share link**, *Copy subscription link*.
2. Send the source to the AP's API (`PUT /api/sources`, see [HTTP API](HTTP-API)). The list replaces the AP's sources, so include the ones you have (read them first with `GET /api/sources`):

```bash
curl -X PUT http://openesl.local/api/sources \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"sources": [{"id": "family", "type": "ical", "url": "webcal://calendar.example/family.ics",
        "layout": "text", "tags": ["00000123456789B2"]}]}'
```

- A secret address lets anyone who has it read the calendar. The AP keeps it, and `GET /api/sources` shows it, so protect the API with a token.
- Big calendars are fine: the AP reads the file as it comes (up to 4 MB).
- `every_min` is 15 to 1440 minutes. The labels only wake when what they show changes.
- Add `"lang": "en"` for English days and times.

## What a label can show

`{title}` (the calendar's name), and for the next six events `{day1}` to `{day6}` (`i dag`, `i morgen`, `tor. 29. okt`), `{time1}` to `{time6}` (`09:00`, `til 10:00` while it is on, `hele dagen`) and `{what1}` to `{what6}` (the summary). `{today}` counts today's remaining events. Without `fields` the built-in `text` layout shows the calendar's name and the next three events.

Repeating events, left-out and moved times, and all-day events are handled. Times with a time zone are shown in the AP's own time zone.

The Dashboard lists the source in its **Weather and pictures** card. **Remove** stops it, and the labels keep what they show. The details are in [docs/data-sources.md](https://github.com/mathiasbk/openESL/blob/master/docs/data-sources.md#the-next-events-of-a-calendar).
