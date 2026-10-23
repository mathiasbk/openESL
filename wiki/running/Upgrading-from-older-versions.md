# Upgrading from older versions

Most releases are just [update the AP](Updating-the-access-point), then [update the labels](Updating-labels). This page lists the releases where the order or an extra step matters.

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
