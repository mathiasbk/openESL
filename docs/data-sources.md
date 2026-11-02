# Data sources: the weather on a tag

An AP with PSRAM (the ESP32-S3 and ESP32-C5 boards) can fetch content for its tags by itself, so no client has to run. A
*data source* is something on the internet the AP reads every so often. The first type is
the weather forecast for a place from [MET Norway](https://api.met.no/) (the data behind
yr.no). The AP turns each answer into values such as the temperature and a weather symbol,
fills the source's fields with them and gives the result to the source's tags, just as if a
client had sent `PUT /api/tags`.

A tag only gets new content when the text came out different: the AP compares it with what
the tag already has, so a forecast that did not change does not wake the tag or cost
battery.

## Which APs have it

| AP ([access-points.md](access-points.md)) | Board in `ap/boards/` | Data sources |
|---|---|---|
| ESP32-C6 Super Mini | `c6-supermini` | no |
| ESP32-C6 with 0.96" screen | `c6-supermini-lcd` | no |
| ESP32-C5 N16R8 | `c5-wroom-1u-n16r8` | yes (built, not tried on a board) |
| ESP32-S3 N16R8 + ESP32-C6 (with or without screen) | `s3-n16r8-c6` | yes |
| ESP32-S3 N16R8 + wireless ESP32-C6 | `s3-n16r8-netradio` | yes |

It is `ESL_DATASOURCES` in menuconfig, on in these boards' `sdkconfig.defaults`. It needs
PSRAM: an answer from MET is up to 128 KB, and TLS needs tens of KB of RAM more while it fetches.
The C6 has no PSRAM and no RAM to spare next to its tags, so there `/api/sources` answers
`404` and the Dashboard does not show the card. On a C6, send the weather yourself instead
(below): the weather template works on every AP.

What the AP needs:
- **The time.** It gets it from NTP. The local time zone decides what "tomorrow", `{h3}`,
  `{h6}` and `{updated}` mean: set it under Settings → General on the Dashboard (`tz` in
  `PUT /api/config`); without one it is UTC.
- **The internet.** It fetches `https://api.met.no/` over TLS.
- **A layout** for each tag's model with the name the source gives, for example the
  `weather` template from the Design tab, or the built-in `text` layout.

## Setting it up on the Dashboard

1. Tick the tags in the Dashboard's tag table and choose **Send → The weather (MET Norway)**,
   then **Choose the place …**.
2. In the dialog, pick a place the AP has, or **A new place**: start typing the place and pick
   it in the list of suggestions (arrow keys and Enter work too). The browser asks two services
   as you type, after a short pause: Kartverket's
   [place names](https://www.kartverket.no/api-og-data/stedsnavndata) for Norway (towns,
   villages, municipalities, islands and mountains, found from the first letters: `Berg` gives
   Bergen) and Open-Meteo's [geocoding](https://open-meteo.com/en/docs/geocoding-api) (GeoNames)
   for the rest of the world (found by the whole name). Both are CC BY 4.0. **Show the weather**
   with a name typed but none picked takes the first suggestion. The place's name is shown on
   the tag; only the name and the coordinates (4 decimals) go to the AP. Without the internet on
   the computer, open **Coordinates** and paste them as a map copies them (right-click the place
   in Google Maps or norgeskart.no: `59.9139, 10.7522`).
3. If the AP's layout file for a ticked tag's model has no layout called `weather`, the dialog
   offers to add the Weather template to it (the file's other layouts stay). The template is
   packed in the AP's firmware, so this works without the internet on the computer.
4. **Show the weather.** The AP fetches the forecast at once, and the tags show it at their
   next poll. A tag that was in another place leaves it; a place left without tags is
   removed.

The **Weather** card under the tag table lists the places, how many tags each has and how its
last fetch went, with **Remove** (stop fetching it; its tags keep what they show) and
**Fetch now**, which asks the AP to fetch every place again. A source whose last answer has not
expired yet is not fetched again (MET's terms, below): its tags' content is made again from
that answer.

## How often it fetches

Every `every_min` minutes, 30 by default and at least 30. Never before the `Expires` of the
last answer: MET says in each answer when it has a new forecast, usually within the hour.
The AP adds a random 0 to 5 minutes to that, so the APs that ask for the same place do not
all ask in the same second.
The AP sends `If-Modified-Since`, so an unchanged forecast comes back as `304 Not Modified`
with no data. When MET gives no answer at all (mostly a WiFi or DNS hiccup) it tries again
after 1 minute, then 3, then every 10; after an answer with an error, in 10 minutes.

A forecast changes slowly, so most fetches change nothing on the tags. The temperature now,
the symbol and the day's lowest and highest change a few times a day.

## Values

A field is a template: text with values in braces, such as `{temp}°` or
`{wind} m/s {dir}`. These are the values of `met`:

| Value | What | Example |
|---|---|---|
| `place` | The source's `place` | `Oslo` |
| `temp` | The temperature now, whole degrees, digits only | `7`, `-3` |
| `symbol` | The weather now as an icon name (below) | `partsun` |
| `desc` | The weather now in words (`lang`: `nb` or `en`) | `Lett regn`, `Light rain` |
| `min`, `max` | The lowest and highest temperature of the next 24 hours | `3`, `9` |
| `wind` | Wind speed now, m/s | `4` |
| `dir` | Where the wind comes from, as a compass point | `SV`, `SW` |
| `rain` | Rain in the next 6 hours, mm | `0`, `0,6`, `12` |
| `rain1h` | Rain in the next hour, mm | `0,2` |
| `humidity` | Relative humidity now, % | `81` |
| `h3`, `t3`, `s3` | In 3 hours: the hour (local time), the temperature, the icon name | `19`, `5`, `moon` |
| `h6`, `t6`, `s6` | The same in 6 hours | `22`, `4`, `rain` |
| `tmin`, `tmax`, `tsym` | Tomorrow (local time): lowest, highest, and the icon around noon | `2`, `8`, `cloud` |
| `updated` | When MET made the forecast, local `HH:MM` | `16:04` |

A value MET did not give is empty. Rain uses a decimal comma below 10 mm.

`updated` changes with every new forecast, so a field with it makes the tags redraw about
once an hour even when the weather stays the same. Leave it out to spare the batteries.

The text goes to the tags in Latin-1, so `°`, `æ`, `ø` and `å` work. The fields share 256
bytes, as for any content, and a source has at most 6 field templates.

## The weather template

`templates/home/weather.json` (Design tab → + New → From template… → Home → Weather) is made for a data
source, with variants for 2.9" and 1.54". Its six fields are the source's default fields:

| Field | Template |
|---|---|
| `place` | `{place}` |
| `temp` | `{temp}` |
| `symbol` | `{symbol}` |
| `desc` | `{desc}` |
| `range` | `{min}° / {max}°` |
| `detail` | `{wind} m/s  {rain} mm` |

The symbol is an *icon named by a field*: the layout holds all ten icons, and the text of
the `symbol` field picks the one to draw. The icon names are `sun`, `moon`, `partsun`,
`partmoon`, `cloud`, `fog`, `rain`, `sleet`, `snow` and `thunder`; MET's symbols map onto
them (night symbols to `moon` and `partmoon`, showers to `rain`, `sleet` or `snow`, and
anything with thunder to `thunder`). Any layout can use this: in the Design tab, choose
**Icon → Named by <field>** on an icon element.

The temperature is drawn with the large digits font, which has no `°`: the template draws
the degree sign as fixed text next to it.

## Your own fields and layouts

A source with `fields` uses those instead. For the built-in `text` layout (four lines):

```json
{"sources": [{
  "id": "cabin", "type": "met", "lat": 61.1153, "lon": 10.4662, "place": "Lillehammer",
  "lang": "en", "every_min": 60, "layout": "text",
  "fields": ["{place} {temp}°", "{desc}", "{min}° to {max}°", "Tomorrow {tmin}°/{tmax}°"],
  "tags": ["00000123456789A8"]
}]}
```

A tag whose model has no layout with the source's name, or whose content does not fit that
layout, keeps what it shows; the card shows the error and `tags_failed` counts it.

## The API

`GET /api/sources` lists the sources with their status, `PUT /api/sources` replaces the
whole list, and `POST /api/sources/refresh` fetches now. Every key, limit and status field is
in [openapi.yaml](openapi.yaml) under *Data sources*. In short:

- At most 8 sources, 32 tags each. A tag can be in one source only.
- The list is saved in NVS (`cfg/sources`) and survives a restart. The last answers are
  not saved: after a restart each source is fetched again.
- Changing a source's layout, fields or tags keeps its last answer, so it does not ask MET
  again. A new place does.
- A tag that leaves a source keeps what it shows until it gets other content.
- Content sent to a tag in a source (`PUT /api/tags`, `PUT /api/tags/{mac}`, scheduled or
  not, or a picture) takes the tag out of the source, so the next forecast does not
  overwrite it; a source left without tags goes. The list is saved. To give it the weather
  again, add it back (Send → The weather on the Dashboard).

Back up the list with `GET /api/sources` and restore it with `PUT` (the `status` of each
source may be sent back as it came; it is ignored).

## MET Norway's terms

The AP follows MET's [terms of service](https://api.met.no/doc/TermsOfService):

- It identifies itself with `User-Agent: openESL/<version> github.com/mathiasbk/openESL`.
- It asks no more often than every 30 minutes, and never before an answer's `Expires`, plus
  a random few minutes to spread the requests.
- It sends `If-Modified-Since` and keeps the last answer.
- It sends the position with 4 decimals at most.
- An answer `203` (this version of the API is deprecated, it ends about a month later) is
  used as data, and the card shows it as the place's error: update the AP's firmware.
- It does not ask for gzip or follow redirects yet; MET answers without them today.

MET's data is free under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) (and the
Norwegian NLOD). **Credit "MET Norway" where the forecast is shown.** The weather template
has the credit in small text at the bottom; a layout of your own for a source should too.

## Without a data source: Home Assistant and other clients

The weather template works with any client: send its six fields with
`PUT /api/tags/{mac}` and `"layout": "weather"`. With Home Assistant's weather integration
(e.g. *Met.no*), [home-assistant.md](home-assistant.md) shows the REST command; the fields
could be:

```yaml
fields: >-
  {{ ["Oslo",
      state_attr('weather.home', 'temperature') | round(0) | int | string,
      {'sunny': 'sun', 'clear-night': 'moon', 'partlycloudy': 'partsun', 'cloudy': 'cloud',
       'fog': 'fog', 'rainy': 'rain', 'pouring': 'rain', 'snowy': 'snow',
       'snowy-rainy': 'sleet', 'lightning': 'thunder', 'lightning-rainy': 'thunder'}
       .get(states('weather.home'), 'cloud'),
      states('weather.home') | replace('-', ' ') | capitalize,
      "", ""] }}
```

This is also the way on an AP without data sources (the C6 alone).

## How it is built

- `ap/components/esl_core/esl_source.c` (plain C, tested on the PC in
  `ap/test/test_source.c`): what every type shares. The configuration and its checks, the
  API, when each source runs (Expires, retries, the random spread), the templates, giving the
  content to the tags, and taking a tag out when it gets other content.
- `ap/components/esl_core/esl_source_met.c`: what is particular to `met`, as an
  `esl_source_def_t` (`esl_source.h`): its own settings (`lat`, `lon`), its URL, how MET's
  JSON becomes values, and its least interval (30 minutes, MET's terms).
- `ap/main/datasrc.c`: a task that fetches with `esp_http_client` (TLS with the certificate
  bundle) when a source is due, keeps the last answers in PSRAM by URL, and saves the list in
  NVS. It does not know what it fetches.
- `web/js/apsources.js`: the Dashboard's weather (the Send choice, the place dialog and the
  card).

A new type is a new file like `esl_source_met.c` and its line in `types[]` in
`esl_source.c`: fetching, caching, templates, the API and saving come with it. What the
engine does not do yet: requests that need a key or a header, answers other than text values
(a chart, for example), and answers over 128 KB.
