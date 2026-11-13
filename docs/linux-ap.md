# The AP on Linux (`openesl-ap`)

`ap/linux/` builds the AP as an ordinary Linux program: the same core as the ESP32 boards (`ap/components/esl_core`: the radio protocol, the drawing, the API, scheduled content, tag firmware), with an HTTP server, files instead of NVS, and a radio chosen at run time. It runs on a Raspberry Pi, a router or gateway with Linux, a server, or a PC, and serves the same API and dashboard as the C6, so `esl-client`, Home Assistant and the web page work against it unchanged.

```
openesl-ap ─┬─ http.c        :80  /api/... → esl_api_handle()   /dashboard, /ui/... (packed from web/)
            ├─ main.c        the radio loop: frame → esl_link_handle() → reply; the scheduler every second
            ├─ store_files.c /var/lib/openesl: content/, cfg/, seen/, image/, sched/, layout/, font/, tagfw/
            └─ radio_*.c     --radio sim | (more backends: see below)
```

## Radios

| `--radio` | What | State |
|---|---|---|
| `sim[:N[,ms]]` | N simulated tags (default 2) in the program, polling every ms (default 2000), alternately 2.9" and 1.54". They take CONFIG and fetch images block by block like a tag, so the dashboard and the API can be tried without hardware. The tests use it. | done |
| `spi-modem:DEVICE[,promisc]` | An 802.15.4 modem with its own firmware behind a Linux SPI driver that frames its traffic as records (some ESL gateways have one): settings and frames go out as commands, received frames come back from `read()` as 132-byte records. Below. | done |
| `uart:DEVICE` | An ESP32-C6 running the radio firmware (`ap/radio/`), over the link of [ap-radio-link.md](ap-radio-link.md). The usual setup: a C6 Super Mini with radio board `c6-usb` on a USB port of the host (`uart:/dev/ttyACM0`). The radio board `c6` speaks the same link on its UART pins (GPIO16 TX, GPIO17 RX), for a host with a serial port or a USB serial adapter at 921600 baud (`uart:/dev/ttyUSB0`). | done |

### An SPI 802.15.4 modem as the radio

`--radio spi-modem:/dev/spidevB.C` drives a modem that keeps its own firmware and takes, over SPI:

| | |
|---|---|
| `[cmd, 0xFF, len LE16, data]` | settings: `0x83` PAN, `0x85` channel, `0x86` short address, `0x87` IEEE address, `0x8C` promiscuous, `0x8B` receive on |
| `[0x88, 0x01, n+1 LE16, frame (n bytes, no FCS), 0xFF ^ XOR(frame)]` | transmit at 250 kbps; the modem adds the FCS. A plain XOR is taken but nothing is sent |
| `[0x02]`, then 2 bytes read | the status of the last transmit (0 = sent), or of receive on, which the modem waits for before it receives |
| `read()` | blocks until frames came in; each is a 132-byte record `[0, len, frame (no FCS), rssi, lqi]` |

The driver sets up its receive queue on every `open()`, so a second open of the device (another program, a test tool) leaves a `read()` that was waiting stuck for good, and that `read()` cannot be interrupted. The backend therefore keeps one descriptor for the whole run, reads in a thread of its own, and sends and sets up through the same descriptor. Nothing else may open the device while `openesl-ap` runs; stop the system's own radio daemon first. Start it after the modem has booted (reset it beforehand if something else used it).

At a stop the receive thread may still wait in `read()`, which only a frame ends: the backend turns promiscuous on as it stops, so any frame on the channel does. On the gateway it was tried on, a stop took 0.3 s, and a restart without a reset of the modem (as `systemctl restart` does it) received at once. On a channel where nothing at all is sent, a stop waits for the next frame. A change of channel (the dashboard's channel move) sets the channel and turns receiving on again with its status; tried by moving to channel 15 and back, the tags polling again on 25 right after.

Tried on 2026-11-13 on an ARMv7 Linux gateway with such a modem (the armv7 build, data in `/opt/openesl`): the test tags polled every 60 s with no lost rounds and no failed replies, and content set through the API was drawn, sent in 4 blocks and shown.

### A C6 on USB as the radio

1. Flash the C6 with the radio board `c6-usb`: `openesl-ap-radio-esp32c6-usb-<version>.bin` from a release, written at 0 with `esptool.py write_flash 0x0 ...` (the web flasher does not offer it), or `cd ap && python build.py radio/c6-usb`. Its log goes to its UART pins, since the USB port is the link.
2. Plug it into the host and start `openesl-ap --radio uart:/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...` (the service's user needs the `dialout` group, which the unit file gives it). Use the name `ls /dev/serial/by-id/` shows rather than `/dev/ttyACM0`: it stays the same across reboots, replugging and other USB devices.
3. The log says `radio 1.1 up on channel 25, PAN 0x4553, 10 dBm` once the C6 has its settings. A C6 that restarts gets them again within a few seconds, and one that has said nothing for 15 s is configured again. A port that goes away (the C6 unplugged) is opened again every 2 s until it is back; frames the C6 kept from before it has its settings are ignored. A radio that says nothing at all (it hangs, or sits in its download mode) is reset: 10 s after the port is opened, or 15 s after it was last heard once it was up through the port's RTS line, as esptool does, at most every 30 s; DTR stays off, so it starts its firmware.

Tried on 2026-11-13 with a C6 Super Mini on a Fedora PC: three test tags (2.9" and 1.54") polled through it, took CONFIG, and showed content set through the API, with no lost rounds and no failed replies.

Try a new install on another channel than the APs in use (`--channel`): a new data directory starts with an empty table and adopts every lost tag on its channel for 14 days, as a new C6 does, and logs a warning when it starts doing so.

## Building

```
make -C ap/linux                    # build/host/openesl-ap with cc (gcc or clang), for this machine
make -C ap/linux test               # and run ap/linux/test.py against it
make -C ap/linux TARGET=armv7       # static builds with zig (pip install ziglang): armv7 (Cortex-A9
make -C ap/linux TARGET=aarch64     # and newer, Raspberry Pi 2/3 in 32-bit), aarch64 (Pi 3/4/5
make -C ap/linux TARGET=x86_64      # in 64-bit), x86_64; build/<target>/openesl-ap runs on any distribution
```

The dashboard is packed from `web/` at build time (`ap/main/webui.py`, as for the C6), so the binary is the whole AP: nothing else has to be installed.

## Running

```
openesl-ap --data ~/openesl --listen :8080 --radio sim
```

The first start makes an API token and keeps it in `<data>/token`; give your own with `--token`. Open `http://<host>:8080/dashboard` and paste the token. `openesl-ap --help` lists the options; every option can also go in a file read with `--config` (`ap/linux/openesl-ap.conf` is an example), and the command line wins over the file.

| Option | Default | |
|---|---|---|
| `--data DIR` | `/var/lib/openesl` | the tags, their content and settings, layouts, fonts, scheduled content, tag firmware, the AP id and the token |
| `--listen ADDR` | `:80` | `[host]:port`; port 80 needs root or `CAP_NET_BIND_SERVICE` |
| `--radio SPEC` | `sim` | above |
| `--channel N` | 25 | the built-in channel; the dashboard can move the AP and its tags to another (kept in `<data>/channel`) |
| `--pan HEX` | 4553 | |
| `--token TOKEN` | made at the first start | |
| `--next-poll S` | 0 | poll interval sent to tags; 0 = each tag keeps its own |
| `--max-tags N` | 1000 | |
| `-v` | | log every request and frame |

### As a service

`ap/linux/openesl-ap.service` runs it with systemd as its own unprivileged user (`DynamicUser`), with `/var/lib/openesl` as its state directory and `CAP_NET_BIND_SERVICE` for port 80; the comment at its top has the install commands. The log goes to the journal (`journalctl -u openesl-ap`). The service user must be able to read the config file, so keep the token out of it (the default, `<data>/token`) unless the file is readable by root and that user only.

The HTTP server takes 32 connections at a time (more get 503), one request each; a request's head must arrive within 10 s and its body at 16 KB/s or faster. An API reply is made in memory and sent after the API's lock is let go, so a slow client holds up nobody else.

## Data directory

Each file holds what the C6 keeps in one NVS entry, in the same format (`esl_persist.h`). It is written to a temporary name of its own, synced, renamed over the old one and the directory synced, so a power cut leaves the old file or the new one. `seen/` (when each tag was last heard, saved every hour) skips the syncs and the file system is synced once after the whole round. An `ap_id` that exists but cannot be read (its owner changed, an I/O error) is not replaced: the AP then runs without an id, as a C6 does, rather than make a new one that would turn its own tags away. A backup is a copy of the directory while the program is stopped, or `esl-client backup` while it runs; `esl-client restore` moves the AP's tags to a C6 or back.

| Path | |
|---|---|
| `token` | the API token, readable by its user only |
| `ap_id`, `prev`, `adopt` | the AP id (#390, [protocol.md](protocol.md) "AP id"): made at the first start, so the tags it configures are this AP's. Started on an empty table, the AP adopts every lost tag for 14 days, as a new C6 does |
| `content/<slot>`, `cfg/<slot>`, `seen/<slot>`, `image/<slot>` | per tag: content, settings, what was last heard from it, a picture set with `PUT /api/tags/{mac}/image` |
| `sched/<slot>` | scheduled content |
| `layout/<model>`, `font/<slot>` | layout files and font files |
| `tagfw/<model>.bin`, `tagfw/<model>.to` | tag firmware and which tags it is for |
| `next_poll`, `channel` | the AP-wide settings changed through the API |

## The clock

Scheduled content needs the time. Once the system clock is past 2024 (NTP on the host, as on any networked Linux), the AP takes it, again every minute. On a device without a clock it waits for `PUT /api/time`, as a C6 without NTP does.

## What it does not do (yet)

WiFi setup, OTA updates of the AP itself (update the binary instead), the data sources of the S3 boards, network radios and the webhook: `/api/wifi`, `/api/sources` and `/api/radios` answer 404, `/api/ota` 501, and `PUT /api/config` with a `"webhook"` 422.

## Tests

`ap/linux/test.py` starts the binary with simulated tags on a free local port and a fresh data directory, and checks the token, both tag models getting content drawn, sent and reported, a restart keeping the content, the AP id and the token, the dashboard and the HTTP server's limits. It then runs `--radio uart` against a radio it plays itself on a pseudo-terminal: CONFIG with the right channel and PAN, HELLO, a POLL as RX answered with a TX to the tag, a frame in two reads, noise, a HELLO with other settings, the port going away. `ap/linux/test_spimodem.c` checks the SPI modem's framing without a device: the transmit command with its check byte, and the records a `read()` returns. `python tools/check.py linux-ap` and CI run it with AddressSanitizer and UndefinedBehaviorSanitizer.
