# Supported labels

OpenESL runs on these labels. Each has its own page with photos, specifications, how to recognise it and what is known about it. Click a photo or a name to open it.

| Label | Product | Screen | Colours | Chip | Battery | Battery reading | Status | Photo |
|---|---|---|---|---|---|---|---|---|
| [SoluM 2.9"](SoluM-2.9-ST-GM29XX2) | ST-GM29XX2 | 296 × 128 | black/white | ZBS243 | two CR2450 | yes | supported | <a href="SoluM-2.9-ST-GM29XX2"><img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/tag-29-front.jpg" alt="The 2.9-inch label" width="140"></a> |
| [SoluM 1.54"](SoluM-1.54-ST-GM16XX2) | ST-GM16XX2 | 200 × 200 | black/white | ZBS243 | one CR2450 | no (shows 0) | supported | <a href="SoluM-1.54-ST-GM16XX2"><img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/tag-154-front.jpg" alt="The 1.54-inch label" width="70"></a> |
| [SoluM 2.9" red](SoluM-2.9-red-ST-GR29000) (**experimental**) | ST-GR29000 | 296 × 128 | black/white/red | ZBS243 | not checked | not tried | experimental | |

The SoluM labels talk 802.15.4 to the AP, are flashed once in the [jig](Flashing-jig) and then take new firmware over the air. The 2.9" and 1.54" black/white also work **without flashing**, on the firmware they came with (experimental): see [Labels without flashing](Labels-without-flashing). Every [template](Label-templates) has a version for each. One AP serves every kind at the same time.

The list is made from [shared/models.json](https://github.com/mathiasbk/openESL/blob/master/shared/models.json), the one place a label model is described; the [README](https://github.com/mathiasbk/openESL/blob/master/README.md#supported-tags) shows the same list, including the Bluetooth tag below.

## SoluM labels: flashed, or as they came

The SoluM 2.9" and 1.54" can run OpenESL's own firmware, or keep the firmware they came with. Both ways the labels get your layouts, fields, pictures and the same [HTTP API](HTTP-API). How to set up the AP for labels as they came: [Labels without flashing](Labels-without-flashing).

| Label | Firmware on the label | How you start | AP | Status |
|---|---|---|---|---|
| [SoluM 2.9"](SoluM-2.9-ST-GM29XX2) (ST-GM29XX2) | OpenESL | Flash once in the jig | Any AP board from the web page | Supported |
| [SoluM 2.9"](SoluM-2.9-ST-GM29XX2) (ST-GM29XX2) | Original, not flashed | Put a battery in | ESP32-C6 or C5 built with the SoluM option | Experimental, shown a layout on a real label |
| [SoluM 1.54"](SoluM-1.54-ST-GM16XX2) (ST-GM16XX2) | OpenESL | Flash once in the jig | Any AP board from the web page | Supported |
| [SoluM 1.54"](SoluM-1.54-ST-GM16XX2) (ST-GM16XX2) | Original, not flashed | Put a battery in | ESP32-C6 or C5 built with the SoluM option | Experimental, joins the AP; picture format tried from a test program |

**Original firmware, not flashed**
- Pros: no jig, no pogo pins, no flasher board; the label is never changed and can go back to a SoluM system as it is; the quickest way to try OpenESL with a box of second-hand labels.
- Cons: black and white only; no battery reading; no firmware updates over the air; the label picks its own check-in time (about every 5 minutes), so new content can take a few minutes; the AP has to use SoluM's channel 20 and PAN ID, so it serves only labels on original firmware; you build the AP firmware yourself for now (ESP32-C6 or C5 only).

**Flashed with OpenESL**
- Pros: firmware updates over the air; battery reading on the 2.9"; the AP sets the check-in time (default 60 s) and night mode; works with every AP board, also the S3 + C6 and the Linux AP; the [red 2.9"](SoluM-2.9-red-ST-GR29000) (experimental).
- Cons: each label goes in the [jig](Flashing-jig) once, which needs a flasher board and pogo pins; the original firmware is backed up first, and only that [backup](Label-backups) brings it back.

## Bluetooth tags

These keep the maker's firmware: openESL speaks their Bluetooth protocol instead of flashing them, with a separate BLE AP build. They do not talk 802.15.4, and the AP's renderer draws their red and yellow too.

<table>
  <tr>
    <td align="center" valign="top"><a href="Gicisky-2.9-BWRY"><img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/tag-29-bwry-2188.jpg" alt="The Gicisky 2.9-inch black/white/red/yellow tag NEMR92852188 showing the openESL demo card" width="296"></a><br><sub><a href="Gicisky-2.9-BWRY">Gicisky / PICKSMART 2.9" BWRY</a><br>296 × 128 · black/white/red/yellow · Bluetooth LE</sub></td>
    <td align="center" valign="top"><a href="Gicisky-2.9-BWRY"><img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/tag-29-bwry-5821.jpg" alt="The second Gicisky 2.9-inch tag, NEMR92835821, showing the same demo card" width="296"></a><br><sub>the second unit, NEMR92835821<br>FF:FF:92:83:58:21</sub></td>
  </tr>
</table>

The photos show the two units with the demo card the BLE AP sent them: red, yellow, black and white in one screen, each with its own tag name and address. The page tells how to build the BLE AP and send content.

## Which one?

<p align="center">
  <img src="https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/tags-29-and-154.jpg" alt="A 2.9-inch label with a big price and a QR code next to a 1.54-inch label with a small QR code" width="560">
</p>

- **The 2.9"** for shelf prices: a product name, a big price and a barcode or QR code side by side. Its battery level shows on the dashboard.
- **The 1.54"** where space is short, or for a single number or status, like a 3D printer's progress or a room's temperature.

## What the status means

- **Supported:** works on hardware and is in every release. Not every release is tested on every label model.
- **Experimental:** builds, but is not in a release yet or has not been tested on every label. Reports are welcome.
- **Planned:** someone has asked for it; nothing is built yet.

## Not supported yet

Labels that look like the ones above but are not the same inside do not work with this firmware. Check the product name on the back before you flash. Known look-alikes:

| Label | Why it does not work yet |
|---|---|
| SoluM 1.54" ST-GR16000 | Older label with a 152 × 152 screen and another display |

## Have a label that is not on the list?

OpenESL is meant to grow to more models and other makers than SoluM, also labels with other chips and radios. One other maker is already supported: the [Gicisky 2.9" BWRY Bluetooth tag](Gicisky-2.9-BWRY) above. [Suggest it in an issue](https://github.com/mathiasbk/openESL/issues/new?template=hardware.yml) with photos of the board and what is written on the chip and display. See [Adding hardware](Adding-hardware).
