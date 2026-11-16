# Coming from OpenEPaperLink

Many people who find OpenESL already have SoluM labels running [OpenEPaperLink](https://github.com/OpenEPaperLink) (OEPL). This page is for you: what stays the same, what is different, which of your labels and access points work, how to move them over, and what changes in Home Assistant.

OpenESL is a separate project, written from scratch. It is not a fork of OEPL and shares no code with it, so the two do not talk to each other: an OpenESL label needs an OpenESL access point, and an OEPL label needs an OEPL access point. You can run both at the same time and move your labels over one by one.

## In short

| | OpenESL |
|---|---|
| **Labels** | SoluM M2 labels on the ZBS243 chip: the 2.9" and the 1.54" black/white, and the 2.9" black/white/red as experimental. [Supported labels](Supported-labels) |
| **Moving a label over** | Once in a [flashing jig](Flashing-jig), with its original firmware backed up first. After that, updates go over the air. |
| **Access point** | An ESP32-C6 is enough (about 400 labels); an S3 + C6 takes 1000 (the C5 too, experimental). [Choosing a board](Access-point-boards) |
| **Radio** | Its own protocol on 802.15.4, channel 25 by default. |
| **Content** | Your software sends text fields; the AP draws them into a layout you made in the [designer](Label-designer). A PNG works too. |
| **API** | A documented [HTTP API](HTTP-API) with a token ([OpenAPI](https://github.com/mathiasbk/openESL/blob/master/docs/openapi.yaml)). |
| **Home Assistant** | Its own [integration](Home-Assistant), installed with HACS. |
| **Licence** | [MIT](https://github.com/mathiasbk/openESL/blob/master/LICENSE): use it at home, in a shop or in a product. |

## What stays the same

The same SoluM labels on the ZBS243 chip, the same kind of jig with ZBS_Flasher, an ESP32 board as the access point, an 802.15.4 radio, and updates over the air once a label runs the firmware.

## How OpenESL does it

**The AP draws from layouts and fields.** You design a layout once (boxes for text, a price, a barcode or QR code, an icon, a date) in the [label designer](Label-designer), or pick one from the [template library](Label-templates). After that your software sends only the text: `{"layout": "price", "fields": ["Milk 1 L", "1.29", "1.29/L"]}`. The AP draws the screen, and the designer shows exactly the pixels the label will get. When you would rather draw the screen yourself, send a PNG of the label's size ([HTTP API](HTTP-API)).

**One token, one API.** Every change goes through the same HTTP API, protected by an API token you choose. The dashboard, the designer, [esl-client](esl-client), the [CSV price lists](Price-lists-from-CSV) and the Home Assistant integration all use it, so whatever they can do, your own script can do too.

**Updates in a safe order.** The release notes always say in which order to update the AP, its radio and the labels ([Upgrading from older versions](Upgrading-from-older-versions)).

**One backup file.** Most of the AP's settings, the layouts, fonts and every label's content are one backup file, and a new AP takes over the labels from it without flashing them again ([Backup and restore](Backup-and-restore), [Moving to a new AP](Moving-to-a-new-AP)).

**Try it first without hardware.** The [simulator](https://github.com/mathiasbk/openESL/blob/master/sim/README.md) runs the label firmware and the AP together on a PC, so you can try the API and your layouts before you flash anything.

## What OpenESL does not do (yet)

Be honest with yourself about these before you move a whole shop or house:

- **Fewer label models.** Only the labels on [Supported labels](Supported-labels). Other SoluM M2 sizes are next on the [Roadmap](Roadmap), and the 2.9" red is experimental; labels with other chips come later. A label that is not on the list stays on OEPL for now.
- **No buttons, LED or NFC** on the labels yet.
- **No moving a label over the air.** See [below](#why-the-jig).
- **Several APs do not work together yet.** Each AP has its own labels on its own channel; one AP with several radios is being tested ([#259](https://github.com/mathiasbk/openESL/issues/259)).
- **No security on the radio yet.** Anyone near you with the right hardware could send a label a screen. See [SECURITY.md](https://github.com/mathiasbk/openESL/blob/master/SECURITY.md) and [#260](https://github.com/mathiasbk/openESL/issues/260).
- **Fetching content is new.** From AP firmware 0.11.0, an AP with PSRAM can fetch the [weather](Weather), a [calendar](Calendar), [news headlines](News-headlines), the [electricity price](Electricity-price), an [exchange rate or the bitcoin price](Exchange-rate-and-bitcoin) and a [picture from a URL](Picture-from-a-URL) by itself, and [count down](Countdown) to a date. Anything else comes from your software or Home Assistant, which sends what the label should show.

## Which of your labels work

Look at the product name on the sticker on the back of the label:

| On the back | Works with OpenESL? |
|---|---|
| ST-GM29XX2 (2.9" black/white) | yes: [SoluM 2.9"](SoluM-2.9-ST-GM29XX2) |
| ST-GM16XX2 (1.54" black/white) | yes: [SoluM 1.54"](SoluM-1.54-ST-GM16XX2) |
| ST-GR29000 (2.9" black/white/red) | experimental: [SoluM 2.9" red](SoluM-2.9-red-ST-GR29000). Try one label first and tell us how it went. |
| ST-GR16000 (older 1.54", 152 × 152) | not yet |
| anything else | not yet. [Suggest it](https://github.com/mathiasbk/openESL/issues/new?template=hardware.yml), with photos of the board. |

Each supported model has been tried with one kind of display. If your label looks right but has another display inside, it may stay blank after flashing; it can always be flashed back from its backup.

## Which of your access points work

| Your AP | Works with OpenESL? |
|---|---|
| A plain ESP32-C6 board, with or without a small screen | yes: flash it on the Flash AP tab. [ESP32-C6](ESP32-C6), [ESP32-C6 with screen](ESP32-C6-with-screen) |
| An ESP32-C5 N16R8 board | experimental: [ESP32-C5](ESP32-C5) |
| An ESP32-S3 with a C6 you wired to it yourself | yes, if the S3 has 16 MB flash and 8 MB PSRAM (N16R8). OpenESL uses its own pins on both chips, so you will probably have to move the wires, and the C6 has to be flashed once over its own USB port. [ESP32-S3 and C6](ESP32-S3-and-C6) |
| Mini-AP v4 or Yellow AP (S3 + C6 on one board) | not yet, and not tested. It is the same pair of chips, but the C6 on the board needs its own radio build and a way to be flashed from the S3 ([#388](https://github.com/mathiasbk/openESL/issues/388)). If you have one and want to help test, say so there. |
| Other ready-made S3 + C6 boards | not yet: the same open points as the Mini-AP v4. |
| An AP whose radio is an ESP32-H2 or a Telink (TLSR) chip | no: the OpenESL radio runs on an ESP32-C6 or C5. |
| An AP whose radio is a ZBS243 module (the first Mini AP, Nano AP, PoE AP and similar) | no, for the same reason. |
| Anything else | probably not yet. [Choosing a board](Access-point-boards) lists every board OpenESL runs on. |

Whichever you use, keep your OEPL AP running on its own board until all your labels have moved. If the board you want to reuse is your only AP, get a C6 board for OpenESL first (they cost a few dollars), move your labels, and reflash the old AP after that.

## Moving your labels over

You need an OpenESL access point and a [flashing jig](Flashing-jig). The jig is a small board running atc1441's [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher) and pogo pins on the test pads behind the battery. If you already flash labels with ZBS_Flasher, that setup works as it is.

1. **Set up the access point.** Use a new board, or flash an ESP32 you used before on the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab. Flashing replaces everything on that board, so keep your OEPL AP on its own board while you move. [Flashing the AP](Flashing-the-access-point)
2. **Pick a channel.** OpenESL uses channel 25 unless you choose another. If your OEPL AP is on the same channel, move one of them, so they do not share the air. [AP settings](Access-point-settings)
3. **Flash one label.** Take out its battery, put it in the jig and use the [Flash tag](https://mathiasbk.github.io/openESL/#tag) tab. It backs up the whole firmware first, so you can go back to OEPL with it later. [Flashing labels](Flashing-labels)
4. **Check it.** Put the battery back. The label shows its start screen, finds the AP within about a minute, and turns up on the [Dashboard](Dashboard). Send it something.
5. **Do the rest.** With **Keep going** on, the Flash tag tab waits for the next label after each one. Export the backups when you are done ([Label backups](Label-backups)).

The label keeps its MAC: OpenESL reads the one OEPL already wrote to the label's info page and leaves it there, so the label has the same MAC in both systems. The barcode column stays empty for these labels, since the barcode is read from the original SoluM firmware.

### Why the jig

OpenESL does not take over labels over the air from OEPL. That would mean building on how OEPL's own firmware updates work, and OpenESL is kept a clean, separate implementation. The jig is also the safe way: the original firmware is backed up before anything is erased, and a label that goes wrong can always be flashed again. Each label goes in the jig once; after that, OpenESL updates it over the air ([Updating labels](Updating-labels)).

### Going back

Write the label's backup back with ZBS_Flasher, and the label is as it was before. Keep the backups somewhere safe: they are the only copy of each label's original firmware.

## Home Assistant

The OEPL integration and its actions do not work with OpenESL labels. Install the [OpenESL integration](Home-Assistant) next to it (HACS, custom repository), and move your automations over label by label:

| You want to | With OpenESL |
|---|---|
| put text on a label | `openesl.send` with a layout and its fields, optionally at a later time |
| draw a whole screen yourself | make a PNG of the label's size and send it with `openesl.send_image` |
| see battery, signal and last check-in | each label is a device with these sensors, plus a problem sensor |
| get told when a label goes silent | the problem sensor, or the AP's [webhook](Health-and-webhook) |

The usual way is to make a layout once in the [designer](Label-designer), with a box for each value, and send only the values from the automation. Ready-made layouts and automations (today's calendar, bin collection, weather, room climate, low battery) are in the [template library](Label-templates) and the [integration's wiki](https://github.com/mathiasbk/openesl-homeassistant/wiki).

```yaml
action: openesl.send
data:
  device_id: 4f0c6a...            # the label: pick it in the UI
  layout: text
  fields:
    - "Living room"
    - "{{ states('sensor.living_room_temperature') }} °C"
```

## Questions

**Can one AP serve both OEPL and OpenESL labels?** No. Each AP runs one of them, and a label only answers the AP of its own system.

**Do I need to update the labels' OEPL firmware first?** No. The jig writes OpenESL over whatever is on the label.

**Can I use my OEPL AP board for OpenESL?** If it is one of the [boards OpenESL supports](Access-point-boards), yes: flash it on the Flash AP tab. It stops being an OEPL AP. See [Which of your access points work](#which-of-your-access-points-work).

**Something is missing that you rely on in OEPL?** [Open an issue](https://github.com/mathiasbk/openESL/issues) and tell us what it is. It helps decide what comes next.

Next: [Getting started](Getting-started).
