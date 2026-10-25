# Home Assistant

OpenESL works with Home Assistant through a plain `rest_command`: no integration to install. The example in the repository shows a 3D printer's progress on a label, but the same pattern fits anything Home Assistant knows about: temperatures, the bin collection day, who is home.

![3D printer status on a label](https://raw.githubusercontent.com/mathiasbk/openESL/master/docs/images/ha-3d-printer.png)

## In short

1. Put the AP's address and token in `secrets.yaml`.
2. Add a `rest_command` that calls `PUT /api/tags/{mac}` with a layout and fields.
3. Call it from an automation when the state changes.

The full example, with YAML ready to copy, a layout with a progress bar, and how to test it from Developer tools: **[docs/home-assistant.md](https://github.com/mathiasbk/openESL/blob/master/docs/home-assistant.md)**.

## Things to keep in mind

- **Battery:** every new screen costs power. Send at most every few minutes (the 3D printer example sends every 10 minutes while printing), or round the numbers (5 % or 5 minutes) when the automation runs on every change of a sensor, so the label is not redrawn for every small change. Sending the same text again is free.
- **Timing:** the label shows the change at its next poll, about a minute with the default interval. Do not lower the poll interval to make it faster; that costs battery all the time.
- **Monitoring:** point the AP's [webhook](Health-and-webhook) at a Home Assistant webhook to get told when a label goes silent or its battery runs low.
