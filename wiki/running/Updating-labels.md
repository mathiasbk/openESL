# Updating labels

Labels get new firmware over the radio, through the AP. The AP holds one image per label model and hands it to the labels of that model, block by block, when they poll. No jig needed.

## One first, then the rest

Give new firmware to **one label of each model first**:

1. On the [Dashboard](Dashboard), tick one label and send it the firmware (or `PUT /api/tag-firmware?to=<MAC>`).
2. Watch it until it has installed and checked in for at least 10 polls. Its start screen shows the new version, and the Firmware card on the dashboard says "Tried: runs on 1 label" with the polls so far, then "safe for the rest" once it has polled 10 times without problems.
3. Give it to the rest of that model: `POST /api/tag-firmware/{model}?to=all`, or tick them on the dashboard.

The dashboard helps with this: when firmware is about to go to several labels of a model and no label has polled 10 times on that version without problems, it asks first, with **Send to one first** as the default. That picks one of the labels (no problems, best signal) and sends the firmware to it alone. It asks again before more labels get a version that labels have had problems with (drawing failed, silent, content cannot be shown, or the label rejected the image). The dashboard counts the polls it sees itself, in that browser, from the first time it sees a label on the version, so a browser that was not open during the update counts from when it is opened.

A label running older firmware than the web page has gets a small arrow in its firmware column on the dashboard.

## From the terminal

```sh
python3 esl_client.py tag-ota openesl-tag-1.54-<version>-ota.bin --to <MAC>   # one label first
python3 esl_client.py tag-ota                      # each model: which image, for whom, how many labels have it
python3 esl_client.py tag-ota --to <MAC>,<MAC>     # a few more labels
python3 esl_client.py tag-ota --model 1.54 --all   # every 1.54 label, once the first one runs it
```

`tag-ota <file>` sends nothing unless you say who gets it: `--to` with one or more MACs (at most 16), or `--all` for every label of that model at once. `--model ... --all` without a file is refused while no label runs the new version yet. `tag-ota --delete MODEL` stops the updates.

## Good to know

- A release that does not change a model's firmware keeps the same version for it, so nothing is sent.
- An update costs some battery: the label downloads about 40 KB.
- A label fetches about 11 KB per wake-up, and between the parts it comes back after a few seconds instead of after its poll interval, so an update takes a few minutes whatever the interval is. Many labels at once keep the radio busy for longer, and the dashboard can feel slower meanwhile on an ESP32-C6 AP.
- To go back, upload the older release's label firmware the same way. A label takes any version that is not its own.
- A label that does not start after an update has to go back in the jig ([Flashing labels](Flashing-labels)).
- Labels flashed before 0.3.0 have no bootloader and need one pass in the jig first.

How it works inside: [docs/tag-ota.md](https://github.com/mathiasbk/openESL/blob/master/docs/tag-ota.md).
