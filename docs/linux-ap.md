# The AP on Linux (`openesl-ap`)

`ap/linux/` builds the AP as an ordinary Linux program: the same core as the ESP32 boards (`ap/components/esl_core`: the radio protocol, the drawing, the API, scheduled content, tag firmware), with an HTTP server, files instead of NVS, and a radio chosen at run time. It runs on a Raspberry Pi, a router or gateway with Linux, a server, or a PC, and serves the same API and dashboard as the C6, so `esl-client`, Home Assistant and the web page work against it unchanged.

**Status: experimental.** Each radio below has run on hardware with a few test tags, not yet for long or with many tags. The user guide is the wiki's [Linux AP](https://github.com/mathiasbk/openESL/wiki/Linux-AP) page.

**Getting it:** releases from 0.11.0 carry static binaries, `openesl-ap-linux-armv7-<version>` (Cortex-A9 and newer, a Raspberry Pi 2/3 on a 32-bit system), `openesl-ap-linux-aarch64-<version>` (a Pi 3/4/5 on a 64-bit system) and `openesl-ap-linux-x86_64-<version>`, built by CI (`.github/workflows/firmware.yml`, job `linux-ap`) with the Dashboard and the Design tab packed in. They run on any distribution; or build it yourself (below).

```
openesl-ap ─┬─ http.c        :80  /api/... → esl_api_handle()   /dashboard, /ui/... (packed from web/)
            ├─ main.c        the radio loop: frame → esl_link_handle() → reply; the scheduler every second
            ├─ store_files.c /var/lib/openesl: content/, cfg/, seen/, image/, sched/, layout/, font/, tagfw/
            ├─ netradio.c    --netradio: C6 radios over the network, beside its own (below)
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
| `[cmd, 0xFF, len LE16, data]` | settings: `0x83` PAN, `0x85` channel, `0x86` short address, `0x87` IEEE address, `0x8C` promiscuous, `0x8B` receive on (1) or off (0) |
| `[0x88, 0x01, n+1 LE16, frame (n bytes, no FCS), 0xFF ^ XOR(frame)]` | transmit at 250 kbps; the modem adds the FCS. A plain XOR is taken but nothing is sent |
| `[0x02]`, then 2 bytes read | the status of the last transmit (0 = sent), or of receive on, which the modem waits for before it receives |
| `read()` | blocks until frames came in; each is a 132-byte record `[0, len, frame (no FCS), rssi, lqi]` |

The driver sets up its receive queue on every `open()`, so a second open of the device (another program, a test tool) leaves a `read()` that was waiting stuck for good, and that `read()` cannot be interrupted. The backend therefore keeps one descriptor for the whole run, reads in a thread of its own, and sends and sets up through the same descriptor. Nothing else may open the device while `openesl-ap` runs; stop the system's own radio daemon first. Start it after the modem has booted (reset it beforehand if something else used it).

At a stop the receive thread may still wait in `read()`, which only a frame ends: the backend turns promiscuous on as it stops, so any frame on the channel does. On the gateway it was tried on, a stop took 0.3 s, and a restart without a reset of the modem (as `systemctl restart` does it) received at once. On a channel where nothing at all is sent, a stop waits for the next frame. A change of channel (the dashboard's channel move) turns receiving off and runs the whole set-up again on the new channel, ending with receive on and its status, as at the start (setting only the channel left the modem deaf after the first frame); a set-up that fails is tried three times. Tried on 2026-11-14 by moving 25 → 15 → 20 → 15 while running, every test tag polling on each new channel after every reply.

Tried on 2026-11-13 on an ARMv7 Linux gateway with such a modem (the armv7 build, data in `/opt/openesl`): the test tags polled every 60 s with no lost rounds and no failed replies, and content set through the API was drawn, sent in 4 blocks and shown.

### A C6 on USB as the radio

1. Flash the C6 with the radio board `c6-usb`: on the web page's Flash AP tab pick **Linux + C6 radio** and **C6 on USB** (the page erases the C6 first), or write `openesl-ap-radio-esp32c6-usb-<version>.bin` from a release at 0 with `esptool.py write_flash 0x0 ...`, or `cd ap && python build.py radio/c6-usb`. The page lists this image apart from the S3's radios (`linux_radios` in its `info.json`), so it is never offered to the C6 of an S3. Its log goes to its UART pins, since the USB port is the link.
2. Plug it into the host and start `openesl-ap --radio uart:/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...` (the service's user needs the `dialout` group, which the unit file gives it). Use the name `ls /dev/serial/by-id/` shows rather than `/dev/ttyACM0`: it stays the same across reboots, replugging and other USB devices.
3. The log says `radio 1.1 up on channel 25, PAN 0x4553, 10 dBm` once the C6 has its settings. A C6 that restarts gets them again within a few seconds, and one that has said nothing for 15 s is configured again. A port that goes away (the C6 unplugged) is opened again every 2 s until it is back; frames the C6 kept from before it has its settings are ignored. A radio that says nothing at all (it hangs, or sits in its download mode) is reset: 10 s after the port is opened, or 15 s after it was last heard once it was up through the port's RTS line, as esptool does, at most every 30 s; DTR stays off, so it starts its firmware.

Tried on 2026-11-13 with a C6 Super Mini on a Fedora PC: three test tags (2.9" and 1.54") polled through it, took CONFIG, and showed content set through the API, with no lost rounds and no failed replies.

Try a new install on another channel than the APs in use (`--channel`): a new data directory starts with an empty table and adopts every lost tag on its channel for 14 days, as a new C6 does, and logs a warning when it starts doing so.

## Network radios

`--netradio [host:]port` lets C6 boards running the network radio firmware (`ap/netradio/`,
[ap-netradio.md](ap-netradio.md)) connect to the AP, as they do to an S3, to cover more ground than
its own radio reaches. The port they look for is 4553 (`--netradio 4553`, or `--netradio
192.168.1.20:4553` for one interface). It is the S3's "Several radios" with one difference: the AP's
own radio (`--radio`) stays in place 0, on the AP's channel, and follows its moves; the network radios
take places 1–3, each on a channel of its own picked when it first connects (15, 20, 26, 11, ...,
never the AP's), kept in `<data>/radios`.

- **Which radio serves a tag:** the one that last heard it, the AP's own included. A tag that loses
  its radio looks on the other channels and stays with the first radio that answers, which tells it
  its own channel; the other radios drop it. Images are drawn by the AP and sent to the radio that
  serves the tag.
- **API and dashboard:** `GET /api/radios` lists the AP's own radio as radio 0 (`"via":"local"`, its
  id is the AP id) and the network radios after it; each tag has `"radio"`. `DELETE
  /api/radios/{id}` forgets a network radio that is not connected; the AP's own answers 409.
- **Setting up a C6:** on the web page's Flash AP tab pick **Linux + C6 radio** and **Extra C6 over
  WiFi**: it flashes the C6 and sends it the WiFi and the AP's address. By hand: flash
  `openesl-ap-netradio-esp32c6-<version>.bin` from a release (`out/openesl-ap-netradio-c6.bin` when
  built) and give it the WiFi and the AP's address on its USB console (`ssid`, `pass`, `host
  <address>`, `reboot`; ap-netradio.md, "Setting it up").
  The Linux AP does not announce itself on mDNS, so `host` is needed. The address can be another
  host that forwards the port, for an AP the radios cannot reach directly, e.g. `socat
  TCP-LISTEN:4553,fork,reuseaddr TCP:<ap>:4553`.
- **The AP still needs a radio of its own.** There is no `--radio none` yet: without `--radio` the
  AP runs `sim` beside the network radios, with simulated tags in its table. Give it a C6 on USB or
  an SPI modem.
- **Not secure**, as on the S3: plain TCP, and any device that reaches the port can connect as a
  radio. Keep the port to the network the radios are on (a firewall, or the address in
  `--netradio`).

## Building

```
make -C ap/linux                    # build/host/openesl-ap with cc (gcc or clang), for this machine
make -C ap/linux test               # and run ap/linux/test.py against it
make -C ap/linux TARGET=armv7       # static builds with zig (pip install ziglang): armv7 (Cortex-A9
make -C ap/linux TARGET=aarch64     # and newer, Raspberry Pi 2/3 in 32-bit), aarch64 (Pi 3/4/5
make -C ap/linux TARGET=x86_64      # in 64-bit), x86_64; build/<target>/openesl-ap runs on any distribution
```

The web page's tabs are packed from `web/` at build time (`ap/main/webui.py`, as for the ESP32 boards), so the binary is the whole AP: nothing else has to be installed. With the renderer built (`python3 web/build.py --wasm`, Emscripten 3.1.74, into `web/dist/wasm/`, or `make WASM=DIR`) it packs the Dashboard and the Design tab, as the S3 boards have them; without it, the Dashboard alone, and the build says so. The page has no link to `/setup`: WiFi and the token are the host's (`<data>/token`).

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
| `--netradio ADDR` | off | `[host:]port` for network radios (above); 4553 is the port they look for |
| `-v` | | log every request and frame |

### As a service

`ap/linux/openesl-ap.service` runs it with systemd as its own unprivileged user (`DynamicUser`), with `/var/lib/openesl` as its state directory and `CAP_NET_BIND_SERVICE` for port 80; the comment at its top has the install commands. The log goes to the journal (`journalctl -u openesl-ap`). The service user must be able to read the config file, so keep the token out of it (the default, `<data>/token`) unless the file is readable by root and that user only.

The HTTP server takes 32 connections at a time (more get 503), one request each; a request's head must arrive within 10 s and its body at 16 KB/s or faster. An API reply is made in memory and sent after the API's lock is let go, so a slow client holds up nobody else.

## In Docker

The binary is static, so it runs in an empty image. Every release from 0.11.0 publishes one, `ghcr.io/mathiasbk/openesl-ap:<version>` and `:latest`, for `linux/amd64`, `linux/arm64` and `linux/arm/v7` (a `FROM scratch` image with the release's binary alone, `ap/linux/Dockerfile.release`, job `docker` in release.yml):

```
mkdir -p ~/openesl
docker run -d --name openesl --restart unless-stopped -p 80:80 \
    --user "$(id -u):$(id -g)" --group-add "$(getent group dialout | cut -d: -f3)" \
    -v ~/openesl:/var/lib/openesl -v /etc/localtime:/etc/localtime:ro \
    --device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...:/dev/ttyACM0 \
    ghcr.io/mathiasbk/openesl-ap:latest --radio uart:/dev/ttyACM0
cat ~/openesl/token
```

The arguments after the image are `openesl-ap`'s options; without any it runs `--radio sim`. To build the image from source instead, `docker build -f ap/linux/Dockerfile -t openesl-ap .` (Alpine and musl, for the platform being built); CI builds that one and checks that it serves the dashboard.

- **Data:** `/var/lib/openesl` in the container; a host folder (as above) keeps it across new images and makes the token easy to read. Make the folder first, and run as your own user (`--user`): the program keeps the token and its folders readable by their owner only, so files written as root could only be read with `sudo`. The numeric `dialout` group (`--group-add`) lets that user open the C6, as the image has no `/etc/group` (on Arch the serial group is `uucp`: `getent group uucp`). A named volume works too.
- **Time zone:** the image has no zone data, so mount the host's (`-v /etc/localtime:/etc/localtime:ro`) or give a POSIX `TZ` (`-e TZ=CET-1CEST,M3.5.0,M10.5.0/3`). Without either it runs in UTC, and night mode, the update window and dates on the tags are off by the zone's offset.
- **The C6 on USB:** `--device` with its `/dev/serial/by-id/` name, so it is the same C6 after a reboot. A C6 that is unplugged and plugged in again comes back with a new device node, which a running container does not see: restart the container, or use `--device-cgroup-rule` with a bind mount of `/dev/serial` if it must survive that.
- **Ports:** 80 for the API and the dashboard, 4553 when network radios connect (`-p 4553:4553` and `--netradio 4553`). The radios need the host's address, not the container's.
- **The Design tab** is in the published image. One built from source has it only when `web/dist/wasm/` was built before `docker build` (`python3 web/build.py --wasm`, Emscripten); otherwise the Dashboard tab alone.
- **Updating:** `docker pull` the new version and start a new container with the same data folder.

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
| `radios` | the network radios' places and channels (`--netradio`) |

## The clock

Scheduled content needs the time. Once the system clock is past 2024 (NTP on the host, as on any networked Linux), the AP takes it, again every minute. On a device without a clock it waits for `PUT /api/time`, as a C6 without NTP does.

## What it does not do (yet)

WiFi setup, OTA updates of the AP itself (update the binary instead), the data sources of the S3 boards and the webhook: `/api/wifi` and `/api/sources` answer 404 (`/api/radios` too without `--netradio`), `/api/ota` 501, and `PUT /api/config` with a `"webhook"` 422. The transmit power of network radios is their own (`ESL_TX_POWER_DBM`).

## Tests

`ap/linux/test.py` starts the binary with simulated tags on a free local port and a fresh data directory, and checks the token, both tag models getting content drawn, sent and reported, a restart keeping the content, the AP id and the token, the dashboard and the HTTP server's limits. It then runs `--radio uart` against a radio it plays itself on a pseudo-terminal: CONFIG with the right channel and PAN, HELLO, a POLL as RX answered with a TX to the tag, a frame in two reads, noise, a HELLO with other settings, the port going away. Then `--netradio` beside the simulated radio, with network radios it plays over TCP: their places and channels, a tag one of them heard served by it with its channel and image, a tag the AP's own radio hears again going back to it, a fifth radio turned away, forgetting one, and the places kept across a restart. `ap/linux/test_spimodem.c` checks the SPI modem's framing without a device: the transmit command with its check byte, and the records a `read()` returns. `python tools/check.py linux-ap` and CI run it with AddressSanitizer and UndefinedBehaviorSanitizer.
