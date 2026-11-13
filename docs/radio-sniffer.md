# The radio sniffer

An ESP32-C6 (or C5) with the sniffer build of the AP firmware hears every 802.15.4 frame on one channel and writes each one to its USB port; `tools/sniffer/sniff.py` reads them and decodes them. It never transmits. Use it to see what an AP really sends and when: whether its answers reach the air at all, how long after the POLL they go out, and whether they have the form the tag accepts ([tag-radio.md](tag-radio.md)). It is also how the [SoluM remote](solum-remote.md) was mapped.

## Getting the sniffer onto a C6

Build it for a board with its own radio (`c6-supermini`, `c6-supermini-lcd`, `c5-wroom-1u-n16r8`):

```
cd ap
python build.py --sniffer c6-supermini
```

That writes `out/openesl-ap-sniffer.bin` (everything, for a USB flash at 0) and `out/openesl-ap-sniffer-ota.bin` (the app, for an update over the air). CI builds the C6 one with the AP boards: the `ap-build-c6-supermini-sniffer` artifact of a Firmware run. Use `build.py --sniffer`, or give `idf.py -DSNIFFER=1` a build folder of its own (`-B build-sniffer`): a folder whose sdkconfig was made for the sniffer keeps building sniffers, and CMake warns when it does. It is the AP firmware with `CONFIG_ESL_RADIO_SNIFFER=y` (`ap/sniffer.sdkconfig.defaults`): no WiFi, no API, no answers, starting on `ESL_CHANNEL` (25).

There are two ways onto a C6:

- **For a while, on a C6 that is an AP:** `esl-client ota out/openesl-ap-sniffer-ota.bin` (over WiFi, or `--serial` on its USB port). The AP's table and settings stay in NVS. The sniffer never confirms itself, so the next reset (power, the reset button, or a terminal that raises DTR and RTS) takes the C6 back to its AP firmware. `sniff.py` holds DTR and RTS low for that reason; open the port the same way in any other terminal. While it sniffs, the AP's tags get no answers and back off ([tag-radio.md](tag-radio.md#how-often-it-polls)), so use a test AP, not one that serves tags people need.
- **For good, on a spare C6:** flash `openesl-ap-sniffer.bin` at 0 with esptool or the web flasher. Flashing the AP firmware the same way turns it back into an AP.

## Reading it

```
python tools/sniffer/sniff.py /dev/ttyACM0                        # every frame on channel 25
python tools/sniffer/sniff.py /dev/ttyACM0 --channel 20           # tune to 20 first
python tools/sniffer/sniff.py /dev/ttyACM0 --hop 300              # 11-26, 300 ms each, 10 s where it hears something
python tools/sniffer/sniff.py /dev/ttyACM0 --mac 0000021234567890 --pcap tag.pcap --log tag.log
python tools/sniffer/sniff.py --read tag.log                      # decode saved lines again
```

`--pan` and `--mac` only filter what is printed (the sniffer itself hears everything). `--pcap` writes a capture Wireshark opens as IEEE 802.15.4 without FCS. On Linux and macOS the script needs Python 3 only; on Windows it needs pyserial. The channel and hopping set from the script are not kept: after a restart the sniffer is back on `ESL_CHANNEL`.

A POLL and its answer look like this:

```
   12.481032           ch25  -61dBm lqi216  37B data seq=7 0000021234567890/4553 -> 0000/4553 POLL fw=1100 bat=2950mV id=1234ABCD model=1 [image,rle,2d] lost=0 | 0101...
   12.482701    +1.7ms ch25  -48dBm lqi255  19B data seq=7 0000/4553 -> 0000021234567890/4553 IDLE next=60s | 02013C00
```

The columns: time in seconds on the sniffer's clock, the time since the frame before it, channel, RSSI, LQI, length without the FCS, then the MAC header (type, sequence number, source and destination as address/PAN) and the openESL message. Addresses are shown as the dashboard shows them. `ackreq` marks a frame that asks for a MAC ACK, which openESL never does; a data frame whose frame control is not openESL's shows it as `fcf=....`, and the payload follows the `|` in hex.

## What it can and cannot tell

- **The time** of a frame is when its last byte arrived, in microseconds, so the gap between a POLL and its answer is the time the answer took plus its own air time (32 µs per byte). An answer that should land in the tag's window has to arrive well inside 20 ms ([tag-radio.md](tag-radio.md#one-round)).
- **A frame with a wrong FCS is never shown.** The C6's radio drops it before the firmware sees it. A sender whose frames never appear, even right next to the sniffer and on the right channel, either sends nothing on that channel or sends frames with a broken FCS (or not as 802.15.4 at 250 kbit/s at all).
- **What the sniffer hears is not what the tag hears.** A frame that reaches the sniffer can still miss the tag, which is further away or has its antenna the other way; put the sniffer next to the tag to see what the tag gets.
- **It drops frames it cannot keep up with.** `dropped` in its status line (`# @SNF-ST ...`, printed at the start and after each command) counts them; at a few hundred tags on 60 s intervals it keeps up easily.

## The line format

Each frame is one line on the USB port, between the firmware's other log lines:

```
@SNF <rx_us> <channel> <rssi> <lqi> <frame in hex, without the FCS>
```

`rx_us` is microseconds since the sniffer started. Commands go the other way, one per line: `ch <11-26>` (stay on that channel, stop hopping), `hop <0 or 20-10000>` (ms per channel, 0 stops), `status`. Each command answers with an `@SNF-ST` line, an unknown one with `@SNF-ERR`. The code is `sniff_line()` and `sniff_command()` in `ap/main/radio.c`.
