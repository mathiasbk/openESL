# Roadmap

What OpenESL wants to get better at, roughly in order. Nothing here has a date: it is a spare-time project, and the order changes when hardware turns up or someone sends a pull request. Smaller items are [open issues](https://github.com/mathiasbk/openESL/issues).

## Next

- **More label models, with red first.** SoluM M2 labels, on the same ZBS243 chip as today's two, so the radio, updates and flashing stay the same and only the panel is new:
  - the 2.9" black/white/red (ST-GR29000) ([#255](https://github.com/mathiasbk/openESL/issues/255)),
  - the 4.2",
  - the 2.6" and 1.6".

  The image format, the AP's drawing and the designer already have a red plane; what is new for each model is its entry in `shared/models.json`, the panel settings in the label firmware, and testing on a real label. The larger screens also need fewer round trips for their bigger images ([#180](https://github.com/mathiasbk/openESL/issues/180)). Have one of these, or another label? See [Adding hardware](Adding-hardware).
- **Choosing the radio channel** on the AP's Settings page, instead of the fixed channel 25 ([#256](https://github.com/mathiasbk/openESL/issues/256); [#217](https://github.com/mathiasbk/openESL/issues/217) for the wireless C6).
- **Measured battery life.** The figures in the [power budget](https://github.com/mathiasbk/openESL/blob/master/docs/strombudsjett.md) are calculated; they will be measured on real labels and published, and the label can save more while the panel refreshes ([#261](https://github.com/mathiasbk/openESL/issues/261)).
- **A Home Assistant integration** that finds the AP, shows each label as a device with battery, signal and last check-in, and sends text and pictures, instead of the `rest_command` in [Home Assistant](Home-Assistant) ([#257](https://github.com/mathiasbk/openESL/issues/257)).

## Later

- **Labels that find the AP by themselves:** a label that loses the AP tries a few channels and stays on the one that answers ([#258](https://github.com/mathiasbk/openESL/issues/258)).
- **Several APs in one place:** one AP keeps the labels and the API, and extra radios cover more rooms ([#259](https://github.com/mathiasbk/openESL/issues/259)).
- **Security on the radio:** content and label firmware that only your AP can send (see [SECURITY.md](https://github.com/mathiasbk/openESL/blob/master/SECURITY.md) for what is not protected today) ([#260](https://github.com/mathiasbk/openESL/issues/260)).
- **Better range in practice:** signal history per label on the Dashboard, and measured range per antenna and transmit power ([#261](https://github.com/mathiasbk/openESL/issues/261)).
- **Layouts:** rotated text ([#176](https://github.com/mathiasbk/openESL/issues/176)), fonts without new firmware ([#177](https://github.com/mathiasbk/openESL/issues/177)), more fields and UTF-8 ([#182](https://github.com/mathiasbk/openESL/issues/182)), date and week in layouts ([#183](https://github.com/mathiasbk/openESL/issues/183)).

## Some day

- **Labels with other chips**, which also means flashing with an ordinary debug probe instead of the jig:
  - SoluM M3 "Newton" (SiLabs EFR32), also sold in black/white/red/yellow and with a button,
  - Hanshow Nebular (nRF52).
- **Buttons, LED and NFC** on the labels that have them, passed on to webhooks and Home Assistant.

## Want to help?

Testing on labels and boards we do not have is the most useful thing right now. Open an [issue](https://github.com/mathiasbk/openESL/issues/new/choose) or read [Contributing](Contributing).
