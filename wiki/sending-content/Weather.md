# Weather on a label

An AP with PSRAM can put the weather forecast on labels by itself, with no computer or Home Assistant running. It fetches the forecast for a place from MET Norway (the data behind yr.no) every half hour or so, and a label is only sent a new screen when the weather on it changed.

## Which APs can do it

| AP | Weather by itself |
|---|---|
| [ESP32-C6](ESP32-C6) | no: send the weather from Home Assistant instead (below) |
| [ESP32-C6 with screen](ESP32-C6-with-screen) | no: the same |
| [ESP32-C5](ESP32-C5) | yes (experimental board) |
| [ESP32-S3 + C6](ESP32-S3-and-C6) | yes |
| [ESP32-S3 + wireless C6](ESP32-S3-with-wireless-C6) | yes |

The C6 has no PSRAM, and fetching a forecast over HTTPS takes more memory than it has next to its labels. On a C6 the Dashboard simply does not show the weather card.

The AP must reach the internet and know the time (it gets it over NTP), and the time zone in its settings decides what "tomorrow" means.

## Setting it up

1. **Add the weather layout.** In the Design tab: **+ New → From template… → Home → Weather**, for each label size you use (2.9" and 1.54"), and save it to the AP. The symbol changes with the weather.
2. **Add a place.** On the Dashboard, open **Weather and data sources → Add a place**. Give it a name, the place and its latitude and longitude (right-click the place in a map to copy them; Oslo is 59.9139, 10.7522), choose the layout `weather` and tick the labels that should show it. Save.
3. The AP fetches the forecast at once, and the labels show it at their next check-in.

![The weather template on a 2.9" and a 1.54" label](https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/weather-template.png)

**Fetch now** asks the AP to fetch again. A forecast that has not expired yet is not fetched again: MET says in each answer when it has a new one, usually within the hour.

## What it shows

The template shows the place, the temperature now, a weather symbol, the weather in words, the lowest and highest temperature of the next 24 hours, wind and rain, and the credit "MET Norway". Under **Fields (advanced)** you can write your own fields from values such as `{temp}`, `{wind}`, `{dir}`, `{humidity}` or tomorrow's `{tmin}` and `{tmax}`, for this layout or any other. Every value is listed in [docs/data-sources.md](https://github.com/mathiasbk/openESL/blob/master/docs/data-sources.md#values).

## Things to keep in mind

- **Battery:** the forecast changes a few times a day, so a weather label redraws a few times a day. Leave `{updated}` out of your own fields: it changes with every new forecast and makes the label redraw about once an hour.
- **Credit:** MET's data is free under CC BY 4.0. Keep "MET Norway" on the label (the template has it).
- **One source per label:** a label can only be in one place's list. Text you send it yourself stays until the next forecast run replaces it.

## Without an AP that can fetch it

The weather template works on every AP: send its six fields yourself with the [HTTP API](HTTP-API) or from [Home Assistant](Home-Assistant). [docs/data-sources.md](https://github.com/mathiasbk/openESL/blob/master/docs/data-sources.md#without-a-data-source-home-assistant-and-other-clients) has a Home Assistant example.
