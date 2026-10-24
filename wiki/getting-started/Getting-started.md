# Getting started

Everything is done from the web page at **[mathiasbk.github.io/openESL](https://mathiasbk.github.io/openESL/)**, which always serves the latest release. You need the [hardware](What-you-need) and Chrome or Edge on a computer.

## 1. Flash the access point

On the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab, follow the three steps: choose your AP, flash it (plug in each board when asked), then give it your WiFi and an API token over the same cable. **Keep a copy of the token**: every API request needs it, and it cannot be read back. The last step shows the AP's address and a button to its dashboard.

Details: [Flashing the AP](Flashing-the-access-point) and [WiFi and API token](WiFi-and-API-token).

## 2. Flash the labels

Put a label in the jig and flash it on the [Flash tag](https://mathiasbk.github.io/openESL/#tag) tab. The page backs up the label's original firmware first, then writes OpenESL. With "Keep going" on, it waits for the next label after each one.

When a flashed label gets its battery back, it shows its start screen with its MAC and firmware version, finds the AP and starts polling it about once a minute.

Details: [Flashing labels](Flashing-labels).

## 3. Pick or design a layout

The AP comes with the layouts `price` and `text`. To make your own, draw it on the [Design](https://mathiasbk.github.io/openESL/#design) tab and send it to the AP.

Details: [Label designer](Label-designer).

## 4. Send content

From any software that can make an HTTP request:

```bash
curl -X PUT http://openesl.local/api/tags/00000123456789A8 \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"layout": "price", "fields": ["Lettmelk 1 L", "24,90", "kr 24,90/L"]}'
```

The label shows the new content at its next poll, usually within a minute.

Details: [Sending content](Sending-content).

## 5. Keep an eye on it

Open `http://openesl-xxxx.local/dashboard`, with the four digits of its setup network ([finding the AP](WiFi-and-API-token#finding-the-ap-on-the-network)), to see every label, what it shows, its battery and signal, and what needs attention.

Details: [Dashboard](Dashboard).

## No hardware yet?

The [simulator](https://github.com/mathiasbk/openESL/blob/master/sim/README.md) runs the label firmware and the AP together on a PC, so you can try the API first.
