# Exchange rate and bitcoin

An AP with PSRAM (the [ESP32-S3 + C6](ESP32-S3-and-C6), [ESP32-S3 + wireless C6](ESP32-S3-with-wireless-C6) and [ESP32-C5](ESP32-C5) boards) can show an exchange rate or the price of bitcoin on labels by itself, with a chart of the last 30 days. Nothing else has to run. Needs AP firmware 0.11.0 or newer.

- **Exchange rate:** today's rate of any currency [Norges Bank](https://www.norges-bank.no/en/topics/Statistics/exchange_rates/) publishes (about 40), in NOK or in another currency, with its change since the day before. Norges Bank publishes one rate each business day, at about 16:00 Norway's time.
- **Bitcoin:** the price now from [CoinGecko](https://www.coingecko.com/), in NOK, USD, EUR or another currency CoinGecko has, with its change today. Fetched every 15 minutes.

Both are free and need no key or account.

## Setting it up

In the **Dashboard**: tick the tags, choose **Price, countdown, news or calendar** under *Send to the ticked tags*, then **An exchange rate, with a chart** (give the currency, and what it is priced in) or **The bitcoin price, with a chart**, and the language of the words (Norwegian or English). The Dashboard adds the *Exchange rate* or *Bitcoin price* layout to the tags' layout files when they lack it (the layouts there stay), and the AP keeps the tags up to date from then on.

The layouts are also templates in the designer (**Design → + New → From template… → Home**), for 2.9" and 1.54" labels, if you want to change them.

Or send the source to the AP's API (`PUT /api/sources`, see [HTTP API](HTTP-API)). The list replaces the AP's sources, so include the ones you have (read them first with `GET /api/sources`):

```bash
curl -X PUT http://openesl.local/api/sources \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"sources": [{"id": "fx-usd", "type": "fx", "currency": "USD", "layout": "exchange-rate",
        "fields": ["{pair}", "{rate}", "{chg} ({pct} %)", "{lo} - {hi}", "{day}", "{chart}"],
        "tags": ["00000123456789B2"]}]}'
```

- `fx`: `currency` (`USD`), `in` (`NOK` by default), `days` (5 to 30 rates in the chart), `unit` (`1` or `100`; by default 100 when one is worth less than 0.1, such as 100 JPY).
- `btc`: `currency` (`nok` by default), `days` (7 to 30).
- `"lang": "en"` gives `9.5325` and `today`/`yesterday`; the default `nb` gives `9,5325` and `i dag`/`i går`.

## What a label can show

Exchange rate: `{pair}` (`USD/NOK`), `{rate}`, `{chg}` and `{pct}` (the change since the day before), `{lo}` and `{hi}`, `{day}` (`i går`, or the date), `{period}` (`30 dager`) and `{chart}`.

Bitcoin: `{cur}`, `{price}` (`658 464`), `{pct}` and `{today}` (the change since midnight UTC), `{lo}` and `{hi}`, `{updated}` (when the price is from), `{period}` and `{chart}`.

`{chart}` is the numbers for a layout's **chart** element (see [Layout elements and limits](Layout-elements-and-limits)). Any client can send such a field too, for example the temperature of the last hours from [Home Assistant](Home-Assistant).

The Dashboard lists the source in its **Data sources** card with how the last fetch went. **Remove** stops it, and the labels keep what they show.

Credit the source where you show it: the templates say *Norges Bank* and *Powered by CoinGecko*, as CoinGecko's terms ask. The details are in [docs/data-sources.md](https://github.com/mathiasbk/openESL/blob/master/docs/data-sources.md#an-exchange-rate).
