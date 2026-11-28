# Labels without flashing

**Experimental.** A SoluM label that still runs the firmware it came with can be used with OpenESL as it is: no jig, no pogo pins, no flashing. Put a battery in it, and an AP built with the SoluM option answers it in SoluM's own radio protocol, draws its screen from your layouts and fields, and sends it the picture. Your software, the [HTTP API](HTTP-API), the [designer](Label-designer), the [dashboard](Dashboard) and [Home Assistant](Home-Assistant) work with it the same way as with a flashed label.

This is the quickest way to try OpenESL with a box of second-hand labels: you need no flashing hardware at all, and the label is never changed. The AP only answers the frames the label sends; it never writes to the label's memory.

**How far it has been tried:** a SoluM 2.9" on its original firmware, never flashed, found an ESP32-C6 AP built with the SoluM option, joined it, fetched a `text` layout and showed it correctly; the label itself reported the picture as shown. A SoluM 1.54" on its original firmware joined the same AP, and earlier showed a picture in SoluM's format sent from a test program. Not tried yet: WiFi, the dashboard and the HTTP API on this AP build (the test drove the AP over its USB serial API), a second update of the same label, a picture from the AP firmware on a 1.54", and the ESP32-C5. A report helps a lot (see the end of this page).

## Flashed or as it is?

| | Label as it came (original firmware) | Label flashed with OpenESL |
|---|---|---|
| **Getting started** | Put a battery in | Once in the [jig](Flashing-jig), with a backup of the original firmware first |
| **What you need** | An AP built with the SoluM option (see below) | Any [AP board](Access-point-boards) from the web page, plus a jig |
| **Labels** | SoluM 2.9" (shown a layout) and 1.54" (joins the AP), black and white | [Supported labels](Supported-labels), including the 2.9" red (experimental) |
| **Layouts, fields, pictures, API** | the same | the same |
| **Battery on the dashboard** | no: the original firmware does not report it | yes on the 2.9" |
| **Poll interval** | the label's own, about every 5 minutes on the 2.9", so new content can take a few minutes | set by the AP, also night mode |
| **Firmware updates over the air** | no: the label keeps its own firmware | yes ([Updating labels](Updating-labels)) |
| **Status** | experimental | supported |

You can still flash such a label later, if you want what only OpenESL's own firmware has.

## Which labels

| Label | As it came |
|---|---|
| [SoluM 2.9"](SoluM-2.9-ST-GM29XX2) (ST-GM29XX2, 296 × 128) | tried: joined a C6 AP built with the SoluM option and showed a text layout |
| [SoluM 1.54"](SoluM-1.54-ST-GM16XX2) (ST-GM16XX2, 200 × 200) | joins the AP; the picture format was tried from a C6 test program, a picture from the AP firmware not yet |
| Other SoluM labels, and other makers | no |

Only black and white is drawn on these labels. This is meant for labels that are not attached to a working SoluM gateway (for example from a shop whose system was taken down), which look for a gateway on their own when they get a battery. A label still attached to a SoluM gateway in a shop belongs to that system; leave it there.

## What you need

- One of the labels above, with a battery.
- An AP that can answer them: an **ESP32-C6** board (with or without a screen), an **ESP32-C5** board, an **ESP32-S3 + C6 on a cable** (the C6 with radio firmware from 1.0.1 on), or the [Linux AP](Linux-AP) with a C6 on USB. The S3 with wireless C6 radios cannot answer these labels yet.
- A computer to build the AP firmware yourself: the SoluM option is not in the ready-made firmware on the Flash AP tab yet. See [Building from source](Building-from-source) for the tools.

## Setting up the AP

These labels look for their gateway on SoluM's own network: radio channel **20** and PAN ID **0x1234**. Since 1.0.1 the AP listens on that PAN next to OpenESL's own (a second PAN on the same radio), so one AP serves labels on their original firmware and labels flashed with OpenESL together. Both kinds share the channel, so the AP has to be on channel 20; labels flashed with OpenESL follow it when it moves.

1. **Build the SoluM build of your board** in `ap/`: `python build.py --variant solum-stock c6-supermini` (or `c5-wroom-1u-n16r8`, `c6-supermini-lcd`, `s3-n16r8-c6`). It is the normal firmware with the SoluM option on (`"solum-stock"` in the board's `variants`, `ap/boards/README.md`). Leave "the gateway address they expect" (`CONFIG_ESL_SOLUM_STOCK_GW_ADDR`) empty: a label that is looking for a gateway then learns the AP's own address. Their PAN is `CONFIG_ESL_SOLUM_STOCK_PAN_ID` (0x1234).
2. **Flash it:** `idf.py -B build-c6-supermini-solum-stock -p <port> flash`. On an S3 + C6, update the C6 too (the S3 offers it on its Firmware page): a C6 with older radio firmware has no second PAN, and the S3 logs that it does not hear the SoluM labels.
3. **Set channel 20** on the AP's [Settings](Access-point-settings) page (or `CONFIG_ESL_CHANNEL=20` before building): a channel chosen there is kept and wins over the built-in one. Labels flashed with OpenESL move with the AP.

On the [Linux AP](Linux-AP) with a C6 on USB nothing has to be built: start it with `--solum-stock-pan 1234 --channel 20` (or `solum-stock-pan = 1234` in its config file). It makes a gateway address at the first start and keeps it in its data folder (`solum-stock-gw`). The SoluM gateway's own radio has no second PAN, so there the AP's `--pan` has to be 1234 and it serves only these labels. On an AP that already ran OpenESL, check the channel on its [Settings](Access-point-settings) page afterwards: a channel chosen there is kept and wins over the built-in one, so set it to 20.
4. **Give it WiFi and an API token** as for any AP ([WiFi and API token](WiFi-and-API-token)).

## Adding a label

1. Put a battery in the label, near the AP. It should find the AP and turn up on the [Dashboard](Dashboard), with **OEM** in the firmware column. Its address is the one the label sends on the radio.
2. Send it content as to any other label, for example with the [HTTP API](HTTP-API):

   ```bash
   curl -X PUT http://openesl.local/api/tags/<MAC> \
     -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
     -d '{"layout": "text", "fields": ["Hello", "no flashing"]}'
   ```

3. The label gets the picture at its next check-in, which on the 2.9" came about every 5 minutes. When it reports that the picture took, the dashboard stops showing the content as waiting.

If a label does not turn up, take its battery out for a minute and put it back, so it starts looking for a gateway again.

## How it works

The AP speaks two protocols on the same radio: OpenESL's own, and SoluM's for labels on their original firmware. For such a label it draws the screen with the same code as for a flashed label and sends it in SoluM's own picture format. The details for developers are in [ap/README.md](https://github.com/mathiasbk/openESL/blob/master/ap/README.md#solum-stock-firmware-tags-a-second-protocol), and where each fact comes from is in [docs/sources.md](https://github.com/mathiasbk/openESL/blob/master/docs/sources.md). The protocol code is OpenESL's own, independent implementation, with no code from other firmware or projects.

## Help us test it

This is new. A report helps a lot, whether it worked or not: which label (the product name on the back), which AP board, and what the dashboard and the label showed. [Open an issue](https://github.com/mathiasbk/openESL/issues/new/choose). A 1.54" getting a picture from the AP, and anything on the ESP32-C5, are the most useful reports right now.
