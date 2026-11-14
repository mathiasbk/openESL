# Linux AP

**Status:** experimental. It has run on hardware with a few test labels per radio (below), not yet for weeks or with many labels. **Labels:** set by the computer it runs on. **Release files:** `openesl-ap-linux-<arch>-<version>` (from 0.11.0), and `openesl-ap-radio-esp32c6-usb-<version>.bin` for its C6 radio.

`openesl-ap` is the access point as an ordinary Linux program. It runs on a Raspberry Pi, a mini PC, a server or another Linux computer, and has the same HTTP API and dashboard as the ESP32 boards, so the web page, [esl-client](esl-client) and [Home Assistant](Home-Assistant) work with it unchanged. The labels are the same, with the same firmware.

The computer has no 802.15.4 radio of its own, so the program needs one:

| Radio | What it is | Tried on hardware |
|---|---|---|
| **ESP32-C6 on USB** | A C6 Super Mini with the radio firmware for USB, plugged into the computer. The usual choice. | yes: three test labels (2.9" and 1.54") on a PC, unplugging and plugging the C6 back in included |
| **The modem of an ESL gateway** | Some shop gateways that run Linux have an 802.15.4 modem behind an SPI driver. `openesl-ap` can use it, so the gateway runs openESL itself. | yes: test labels on one such gateway, including moving channel while running |
| **Extra ESP32-C6 radios over WiFi** | Up to three [network radios](ESP32-S3-with-wireless-C6) beside the radio above, each on its own channel, for rooms the first radio does not reach. Same firmware and same limits as with an S3, and not secure yet. | yes: a few test labels on one PC with network radios |
| **Simulated labels** (`--radio sim`) | Labels inside the program, to try the dashboard and the API with no hardware. | runs in CI |

The program always needs one radio of its own (the C6 on USB, or a gateway's modem); radios over WiFi are added to it.

## Compared with the ESP32 boards

- **More room:** how many labels depends on the computer, not on a small chip; the label designer on the AP's own page (Design tab), and data in files: back it up from the dashboard as on any AP, or copy its data folder while it is stopped.
- **No WiFi setup and no OTA:** the computer has its own network, and you update the AP by replacing the program. The API token is made at the first start and kept in a file.
- **Not there yet:** the webhook, data sources such as [weather](Weather) or [electricity price](Electricity-price), and updating the AP from the dashboard. Labels get firmware updates as usual.

## Setting it up

1. **Flash the C6 radio.** On the [Flash AP](https://mathiasbk.github.io/openESL/#ap) tab, pick **Linux + C6 radio** and **C6 on USB**. The page flashes the C6 and then shows the next steps.
2. **Get the program.** From the [latest release](https://github.com/mathiasbk/openESL/releases/latest), download `openesl-ap-linux-aarch64-<version>` for a Raspberry Pi 3, 4 or 5 with a 64-bit system, `openesl-ap-linux-armv7-<version>` for a 32-bit system, or `openesl-ap-linux-x86_64-<version>` for a PC. It is one file with nothing else to install. Rename it and make it executable, for example `mv openesl-ap-linux-aarch64-v0.11.0 openesl-ap && chmod +x openesl-ap`.
3. **Start it** with the C6 plugged in. `ls /dev/serial/by-id/` shows the C6's name:

   ```
   ./openesl-ap --data ~/openesl --listen :8080 \
       --radio uart:/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...
   ```

   The log says `radio 1.1 up on channel 25` when the C6 is ready. Your user needs to be in the `dialout` group to open the port.
4. **Open the dashboard** at `http://<computer>:8080/dashboard` and paste the token from `~/openesl/token`.
5. **Run it as a service** so it starts with the computer: the [systemd unit and its install commands](https://github.com/mathiasbk/openESL/blob/master/ap/linux/openesl-ap.service).

Start a new Linux AP on another radio channel than any AP you already use (`--channel`): a new AP takes over any lost label on its channel for its first 14 days.

**In Docker:** from 0.11.0 every release publishes an image for amd64, arm64 and armv7 (a Raspberry Pi too). Give it the C6 and a folder for its data:

```
docker run -d --name openesl --restart unless-stopped -p 80:80 -v ~/openesl:/var/lib/openesl \
    --device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...:/dev/ttyACM0 \
    ghcr.io/mathiasbk/openesl-ap:latest --radio uart:/dev/ttyACM0
```

The token is then in `~/openesl/token`, and the dashboard at `http://<computer>/dashboard`. Updating is `docker pull` and a new container with the same folder. More, including network radios and building the image yourself: [docs/linux-ap.md](https://github.com/mathiasbk/openESL/blob/master/docs/linux-ap.md#in-docker).

**Radios over WiFi:** start the program with `--netradio 4553` as well, then on the Flash AP tab pick **Linux + C6 radio** and **Extra C6 over WiFi** for each radio. Type the computer's address when the page asks: the Linux AP does not announce itself on the network the way an S3 does.

**Moving from an ESP32 AP:** take a backup on the old AP and restore it with `esl-client restore`. The labels follow without being paired again ([Moving to a new AP](Moving-to-a-new-AP)).

All the options, the data directory and the details of each radio: [docs/linux-ap.md](https://github.com/mathiasbk/openESL/blob/master/docs/linux-ap.md).
