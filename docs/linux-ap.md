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
| `uart:/dev/ttyACM0` | An ESP32-C6 running the radio firmware (`ap/radio/`) on USB or a serial port, over the link of [ap-radio-link.md](ap-radio-link.md). The usual setup: a Raspberry Pi and a C6 Super Mini. | planned |

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

`ap/linux/test.py` starts the binary with simulated tags on a free local port and a fresh data directory, and checks the token, both tag models getting content drawn, sent and reported, a restart keeping the content, the AP id and the token, and the dashboard. `python tools/check.py linux-ap` and CI run it with AddressSanitizer and UndefinedBehaviorSanitizer.
