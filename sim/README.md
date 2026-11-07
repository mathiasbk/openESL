# esl-sim: tag and AP on the PC

The tag firmware and the AP run together on the PC, with no hardware:

- **Tag:** `tag/src/app.c` with all of its logic (POLL, CONFIG, the image download and
  drawing, the status screens, the image kept for requirement T12), on a HAL (`sim_tag.c`) that sends its frames
  to the AP instead of a radio. The display is `tag/test/fake_epd.c`, which draws into an
  image that is saved as PNG.
- **AP:** `ap/components/esl_core` as the C6 runs it: `esl_radio_handle()` answers every frame,
  `esl_api_handle()` every API request, and "NVS" is kept in RAM in the same blob formats as
  `ap/main/store_nvs.c`, so the AP can be switched off and restarted (`sim_ap.c`).
- **Radio link:** a frame from the tag goes straight to the AP and the reply straight back.
  Frames and replies can be lost on purpose, and the AP can be off or die in the middle of an
  exchange.
- **Time** is simulated: it moves only when the tag sleeps, waits or listens, so hours
  without the AP take milliseconds.

What the simulation does not cover: the radio drivers and their timing, the display
controller, deep sleep and power, and radio collisions. The tag code is written for one tag
per program, so the scenarios have one tag; `serve --tags N` runs each further tag in a
program of its own (below).

The quickest way to see it all is `python3 tools/demo.py` at the top of the repository: it
builds the simulator, starts `serve` with six tags, the esl-client bridge and the web page,
gives the tags some content and opens the dashboard connected to it (`--docker` on Windows).

## Running it

Needs a C compiler and make (Linux or macOS; `serve` needs a pseudo-terminal).

```
make -C sim            # both tag models: build/154/esl-sim and build/29/esl-sim, then the scenarios
make -C sim SAN=1      # the same with AddressSanitizer and UBSan (as CI runs it)
make -C sim serial     # esl-client over the serial framing against "esl-sim serve"
make -C sim upgrade    # tag firmware over the air from each release to today's, and back
SIM_VERBOSE=1 sim/build/154/esl-sim test /tmp/screens   # every radio exchange
```

The screens are written to `sim/build/<model>/screens/*.png` (twice the real size). CI keeps
them as the `sim-screens` artifact of each run.

## The scenarios (`esl-sim test`, `e2e.c`)

One tag goes through all of them in order, about 4 simulated hours, driven through the API:

| # | Case | Checked |
|---|---|---|
| 0 | The AP draws every layout as the reference says (`esl_render.c`) | Every layout of `tag/designs/layouts/<model>.json`, `ap/test/layouts/<model>.json` and `sim/layouts/<model>.json`, and the built-in `price` and `text`, with nine sets of fields and every flag combination (828 screens per model): each is drawn, and its CRC-32 is the one in `sim/reference/<model>.crc`. Every screen saved below is what the tag shows, compared with what the AP draws for it |
| 1 | New tag, unknown to the AP | Start screen; the AP registers it at its first POLL (model, no content) |
| 2 | New content (`PUT /api/tags/{mac}`) | Drawn at the next poll (G1, 2 min); the API confirms one poll later |
| 3 | Same content again | `changed: false`, IDLE, no redraw for 5 min |
| 4a | The first IMG block lost once | The retry in the same wake-up gets it, no miss |
| 4b | The first IMG block lost in both tries | A miss, no backoff; the next wake-up gets it; `pending` until then |
| 4c | The POLL that confirms it is lost | The API stays `pending` until the next POLL |
| 4d | The AP dies while sending the image, back after 3 min | Content survives the restart (NVS); the tag gets the image when the AP is back |
| 5 | Layout file (`PUT /api/layouts/{model}` with `tag/designs/layouts/`) and content with the `shelf` layout | The AP draws the named layout and the tag shows the image at the next poll; the file stays on the AP |
| 6 | CONFIG: `poll_s` 120, then back to the default | The tag sleeps 120 s, then 60 s; `config_pending: false` |
| 7 | Tag restart with the AP off | The content comes back from flash (T12); no new image when the AP returns |
| 8 | AP gone for an hour with new content waiting | The backoff reaches 600 s (T11); the tag finds the AP within 600 s and gets the content |
| 9 | AP gone for more than 2 hours | The content stays with the "no AP" mark in its corner (T9, #98), no more refreshes while the AP is away; the content without the mark when the AP is back, from the tag's flash |
| 10 | Tag deleted on the AP | Registered again with no content, keeps its screen; a new PUT reaches it |
| 11 | Content with `at`, poll interval 300 s | Shown just after it is due (the AP shortens the tag's sleep), not at the next 300 s poll |
| 12 | Tag firmware over the air (`PUT /api/tag-firmware`, a 4 KB test image in two parts) | Downloaded, copied into the chip by the bootloader, restarted with the same screen, confirmed |

The simulated tag reports 2950 mV and 20 °C. The scenarios use the `shelf` layout, so barcodes, fixed texts and five or six fields are covered by the AP host tests, not end to end here.

`sim/layouts/` holds layouts made only for this comparison: a short last word that has to
wrap, `invert` and `highlight` text, and barcodes centred or right-aligned with an odd number of
pixels to spare. `sim/reference/<model>.crc` was made while the tag still drew content with its
own code, and each screen in it was byte for byte what the tag drew: one line per screen (`file
layout set flags bytes crc`). A change to the drawing that is
meant to change screens rewrites it with `SIM_REF_UPDATE=1 make -C sim`, and its diff shows
which screens changed. So does a change to a layout in `tag/designs/layouts/`,
`ap/test/layouts/` or `sim/layouts/`: those screens are part of the reference too.

## Serial port (`esl-sim serve`)

```
sim/build/154/esl-sim serve --speed 60 --png /tmp/screen.png
PTY /dev/pts/5
```

opens a pseudo-terminal that speaks the AP's USB serial framing (`ap/main/serial_api.c`), with
log lines in between as on the C6. One simulated tag (`00000123456789A8`) polls it,
`--speed` times faster than real time, and its screen is written to `--png` after every
drawing. Anything that talks to the AP over USB works against it:

```
tools/esl-client/esl_client.py --serial /dev/pts/5 status
tools/esl-client/esl_client.py --serial /dev/pts/5 price 00000123456789A8 "Milk 1 L" 24.90 "NOK 24.90/L" --wait
tools/esl-client/esl_client.py --serial /dev/pts/5 serve    # "Test request" in the API tab of the web page
```

`test_serial.py` does this in CI.

### More tags (`--tags N`)

```
sim/build/154/esl-sim serve --speed 10 --tags 6 --sims sim/build/29/esl-sim,sim/build/293/esl-sim,sim/build/154/esl-sim
```

starts five more tags (at most 16 in all), `00000123456789A9` and up, each as `esl-sim tag`
in a program of its own, so each can be another model: the programs in `--sims` are taken in
turn (default: the same one). Each has its own battery, temperature and signal (the RSSI the
AP measures for its frames), one with a low battery and one with a weak signal, so the
dashboard has something to show. Their frames go to the AP in `serve` over a socket; they stop
when it stops. `--png` is the first tag's screen only; the dashboard shows the others as the
AP draws them.

## Upgrade from a release (`make upgrade`)

A tag in the field runs the firmware of some earlier release, and gets today's over the air
from an AP that runs today's. `upgrade.py` tests exactly that, for every release with a
golden file in `ap/test/compat/` (the VERSION PR adds one) and every tag model that release
and today's both have (#307):

```
make -C sim upgrade            # or: python3 sim/upgrade.py v0.10.0
make -C sim upgrade SAN=1      # with the sanitizers, as CI runs it
```

It needs the release tags (`git fetch --tags`). For each release it takes that release's
`tag/` and `shared/` from git (nothing in the working tree changes) and builds `esl-sim-tag`
from it and from today's tag firmware, both with today's `sim_tag.c`, `tagstep.c` and `png.c`.
`esl-sim-tag` is one tag in a program of its own; `esl-sim upgrade` (`upgrade.c`) runs today's
AP core and drives it step by step over a socket (`tagstep.h`), with simulated time, so a run
gives the same result every time:

1. A new tag on the release's firmware registers with today's AP, gets content and its own
   poll interval (120 s), and draws what the AP draws.
2. The AP offers today's firmware (`PUT /api/tag-firmware`, a made-up 4 KB application with
   today's version). The release's firmware downloads it and its bootloader copies it into the
   chip; the program then saves what a restart keeps (the settings record, the SPI flash, the
   chip's code flash and the e-paper image, not RAM) and stops.
3. Today's firmware starts from that in a second program, and must: show the same screen, find
   the AP at once, keep its settings (no CONFIG when both keep them in the same record format,
   `STORE_FORMAT` in `tag/src/app.c`; at most the one CONFIG `app.c` allows when not), confirm the
   update (no rollback; the AP counts the tag as updated, not failed), not be sent its image
   again, and draw new content as the AP draws it.

Then the same the other way round, from today's firmware back to the release. When a model's
firmware has not changed since a release, both report the same version and the tag would not
take the update, so today's is tested as the next patch version. The screens are saved in
`sim/build/upgrade/<release>-<model>/up/` and `.../back/` (`1-before.png`, `2-after.png`,
`3-new-content.png`) and kept by CI with the other screens.

What it does not cover: the bootloader is never updated over the air, and the one that installs
the update is the first program's, as on a real tag; a further restart in the second program
would run that program's own. The update is not today's real image built with SDCC
(`tools/check.py tag-fw` builds that), so its size and the code on the chip are not part of it.
