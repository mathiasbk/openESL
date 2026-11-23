# Labels without flashing

**Experimental.** A SoluM label that still runs the firmware it came with can be used with OpenESL as it is: no jig, no pogo pins, no flashing. Put a battery in it, and an AP built with the SoluM option answers it in SoluM's own radio protocol, draws its screen from your layouts and fields, and sends it the picture. Your software, the [HTTP API](HTTP-API), the [designer](Label-designer), the [dashboard](Dashboard) and [Home Assistant](Home-Assistant) work with it the same way as with a flashed label.

This is the quickest way to try OpenESL with a box of second-hand labels: you need no flashing hardware at all, and the label is never changed. The AP only answers the frames the label sends; it never writes to the label's memory.

## Flashed or as it is?

| | Label as it came (original firmware) | Label flashed with OpenESL |
|---|---|---|
| **Getting started** | Put a battery in | Once in the [jig](Flashing-jig), with a backup of the original firmware first |
| **What you need** | An AP built with the SoluM option (see below) | Any [AP board](Access-point-boards) from the web page, plus a jig |
| **Labels** | SoluM 1.54" (tried) and 2.9" (not tried yet), black and white | [Supported labels](Supported-labels), including the 2.9" red (experimental) |
| **Layouts, fields, pictures, API** | the same | the same |
| **Battery on the dashboard** | no: the original firmware does not report it | yes on the 2.9" |
| **Poll interval** | the label's own; the AP learns it from the label's check-ins | set by the AP, also night mode |
| **Firmware updates over the air** | no: the label keeps its own firmware | yes ([Updating labels](Updating-labels)) |
| **Status** | experimental | supported |

You can still flash such a label later, if you want what only OpenESL's own firmware has.

## Which labels

| Label | As it came |
|---|---|
| [SoluM 1.54"](SoluM-1.54-ST-GM16XX2) (ST-GM16XX2, 200 × 200) | tried: a label on its original firmware showed a picture sent in this format |
| [SoluM 2.9"](SoluM-2.9-ST-GM29XX2) (ST-GM29XX2, 296 × 128) | in the firmware, not tried on a label yet |
| Other SoluM labels, and other makers | no |

Only black and white is drawn on these labels. Tried so far: labels that are not attached to a working SoluM gateway (for example from a shop whose system was taken down), which look for a gateway on their own when they get a battery. A label still attached to a SoluM gateway in a shop belongs to that system; leave it there.

## What you need

- One of the labels above, with a battery.
- An **ESP32-C6** board (with or without a screen) or an **ESP32-C5** board as the AP. The ESP32-S3 + C6, the S3 with wireless C6 radios and the [Linux AP](Linux-AP) cannot answer these labels yet.
- A computer to build the AP firmware yourself: the SoluM option is not in the ready-made firmware on the Flash AP tab yet. See [Building from source](Building-from-source) for the tools.

## Setting up the AP

These labels look for their gateway on SoluM's own network: radio channel **20** and PAN ID **0x1234**. The AP has to use the same, so this AP serves only labels on their original firmware; labels flashed with OpenESL need an AP of their own on OpenESL's channel and PAN ID.

1. **Turn the option on.** In `ap/`, add these lines to the board's `boards/<board>/sdkconfig.defaults` (for example `boards/c6-supermini/sdkconfig.defaults`), or set the same in `idf.py menuconfig` under "OpenESL AP":

   ```
   CONFIG_ESL_SOLUM_STOCK_TAGS=y
   CONFIG_ESL_CHANNEL=20
   CONFIG_ESL_PAN_ID=0x1234
   ```

   Leave "the gateway address they expect" (`CONFIG_ESL_SOLUM_STOCK_GW_ADDR`) empty: a label that is looking for a gateway then learns the AP's own address.
2. **Build it:** `python build.py c6-supermini` (or your board).
3. **Flash it:** `idf.py -B build-c6-supermini -p <port> flash`. On an AP that already ran OpenESL, check the channel on its [Settings](Access-point-settings) page afterwards: a channel chosen there is kept and wins over the built-in one, so set it to 20.
4. **Give it WiFi and an API token** as for any AP ([WiFi and API token](WiFi-and-API-token)).

## Adding a label

1. Put a battery in the label, near the AP. Within a minute or so it finds the AP and turns up on the [Dashboard](Dashboard), with **OEM** in the firmware column. Its address is the one the label sends on the radio.
2. Send it content as to any other label, for example with the [HTTP API](HTTP-API):

   ```bash
   curl -X PUT http://openesl.local/api/tags/<MAC> \
     -H "Authorization: Bearer $TOKEN" \
     -d '{"layout": "text", "fields": ["Hello", "no flashing"]}'
   ```

3. The label gets the picture at its next check-in. When it reports that the picture took, the dashboard stops showing the content as waiting.

If a label does not turn up, take its battery out for a minute and put it back, so it starts looking for a gateway again.

## How it works

The AP speaks two protocols on the same radio: OpenESL's own, and SoluM's for labels on their original firmware. For such a label it draws the screen with the same code as for a flashed label and sends it in SoluM's own picture format. The details for developers are in [ap/README.md](https://github.com/mathiasbk/openESL/blob/master/ap/README.md#solum-stock-firmware-tags-a-second-protocol), and where each fact comes from is in [docs/sources.md](https://github.com/mathiasbk/openESL/blob/master/docs/sources.md). The protocol work is our own measurements and tests, not code from other firmware.

## Help us test it

This is new. A report helps a lot, whether it worked or not: which label (the product name on the back), which AP board, and what the dashboard and the label showed. [Open an issue](https://github.com/mathiasbk/openESL/issues/new/choose). A 2.9" label on its original firmware is the most useful report right now.
