# Upgrading from older versions

Most releases are just [update the AP](Updating-the-access-point), then [update the labels](Updating-labels). This page lists the releases where the order or an extra step matters.

## From 0.5.x to 0.6.0

Update the AP first, then the labels, as usual. Either order works here: old and new labels and APs still understand each other.

- Layouts get lines, rounded and open boxes, icons and QR codes. After the update the AP sends every label that uses a layout file its screen once more. QR codes now keep a blank border inside their box, so a QR box holds a little less than before.
- Images can go to 0.6.0 labels with a new code that is often shorter, so they arrive faster.

**Going back** to 0.5.x: a 0.5.x AP refuses at start a layout file that uses icons, lines, QR codes or the new box options, and that model has no layouts until a file without them is uploaded. Pictures put on labels through the API are dropped too, though the labels keep showing them. Update the AP to 0.6.0 again and they come back, unless you uploaded a layout file or sent content to those labels in the meantime. The backup keeps the pictures as well. Take a backup (`esl_client.py backup ap.json`) before updating.

Every detail: [docs/release.md](https://github.com/mathiasbk/openESL/blob/master/docs/release.md#upgrading-from-05x-to-060).

## From 0.4.x to 0.5.0

In 0.5.0 the AP draws every screen and sends it to the label as an image. Before, the label drew the text itself.

1. Take a backup (`esl_client.py backup ap.json`), then update the **AP first**. A 0.5.0 AP still sends firmware to 0.4.x labels.
2. Then update the labels, one of each model first. Until then they keep the screen they show, and the dashboard says they cannot show images.
3. Do not update labels before their AP: a 0.4.x AP cannot send 0.5.0 labels new screens.
4. Optional, C6 boards: flashing once from the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab without "Erase device" gives room for 7 label models instead of 2. Two is enough for the 2.9" and the 1.54".

**Going back** from 0.5.0 means the AP and the labels together. A 0.4.x AP holds at most 256 labels, and refuses layout files that use the larger limits of 0.5.0.

Every detail: [docs/release.md](https://github.com/mathiasbk/openESL/blob/master/docs/release.md#upgrading-from-04x-to-050).

## Labels flashed before 0.3.0

They have no bootloader, so they cannot take updates over the air. Flash them once more in the jig ([Flashing labels](Flashing-labels)); after that, updates go over the radio.
