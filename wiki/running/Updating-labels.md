# Updating labels

Labels get new firmware over the radio, through the AP. The AP holds one image per label model and hands it to the labels of that model, block by block, when they poll. No jig needed.

## One first, then the rest

Give new firmware to **one label of each model first**:

1. On the [Dashboard](Dashboard), tick one label and send it the firmware (or `PUT /api/tag-firmware?to=<MAC>`).
2. Watch it until it has installed and checked in for at least 10 polls. Its start screen shows the new version.
3. Give it to the rest of that model: `POST /api/tag-firmware/{model}?to=all`, or tick them on the dashboard.

A label running older firmware than the web page has gets a small arrow in its firmware column on the dashboard.

## From the terminal

```sh
python3 esl_client.py tag-ota                      # each model: which image, how many labels have it
python3 esl_client.py tag-ota openesl-tag-1.54-<version>-ota.bin
```

`tag-ota <file>` sends to **every** label of that model at once, so use it only when you are sure. `tag-ota --delete MODEL` stops the updates.

## Good to know

- A release that does not change a model's firmware keeps the same version for it, so nothing is sent.
- An update costs some battery: the label downloads about 40 KB.
- To go back, upload the older release's label firmware the same way. A label takes any version that is not its own.
- A label that does not start after an update has to go back in the jig ([Flashing labels](Flashing-labels)).
- Labels flashed before 0.3.0 have no bootloader and need one pass in the jig first.

How it works inside: [docs/tag-ota.md](https://github.com/mathiasbk/openESL/blob/master/docs/tag-ota.md).
