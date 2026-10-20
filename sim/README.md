# esl-sim: tag and AP on the PC

The tag firmware and the AP run together on the PC, with no hardware:

- **Tag:** `tag/src/app.c` with all of its logic (POLL, CONTENT, CONFIG, layout download,
  drawing, the flash copy from requirement T12), on a HAL (`sim_tag.c`) that sends its frames
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
controller, deep sleep and power, and more than one tag at a time (the tag code is written
for one tag per program).

## Running it

Needs a C compiler and make (Linux or macOS; `serve` needs a pseudo-terminal).

```
make -C sim            # both tag models: build/154/esl-sim and build/29/esl-sim, then the scenarios
make -C sim SAN=1      # the same with AddressSanitizer and UBSan (as CI runs it)
make -C sim serial     # esl-client over the serial framing against "esl-sim serve"
SIM_VERBOSE=1 sim/build/154/esl-sim test /tmp/screens   # every radio exchange
```

The screens are written to `sim/build/<model>/screens/*.png` (twice the real size). CI keeps
them as the `sim-screens` artifact of each run.

## The scenarios (`esl-sim test`, `e2e.c`)

One tag goes through all of them in order, about 4 simulated hours, driven through the API:

| # | Case | Checked |
|---|---|---|
| 0 | The AP draws what the tag draws (`esl_render.c`) | Every layout of `tag/designs/layouts/<model>.json` and `ap/test/layouts/<model>.json`, and the built-in `price` and `text`, with seven sets of fields and every flag combination (224 + 112 = 336 screens per model): the image the AP draws is byte for byte the one the tag's own code draws. Every screen saved below is compared the same way |
| 1 | New tag, unknown to the AP | Start screen; the AP registers it at its first POLL (model, no content) |
| 2 | New content (`PUT /api/tags/{mac}`) | Drawn at the next poll (G1, 2 min); the API confirms one poll later |
| 3 | Same content again | `changed: false`, IDLE, no redraw for 5 min |
| 4a | CONTENT reply lost once | The retry in the same wake-up gets it, no miss |
| 4b | CONTENT lost in both tries | A miss, no backoff; the next wake-up gets it; `pending` until then |
| 4c | The POLL that confirms it is lost | The API stays `pending` until the next POLL |
| 4d | The AP dies while sending CONTENT, back after 3 min | Content survives the restart (NVS); the tag gets it when the AP is back |
| 5 | Layout file (`PUT /api/layouts/{model}` with `tag/designs/layouts/`) and content with the `shelf` layout | Download and drawing in one wake-up; `layout_pending: false` |
| 6 | CONFIG: `poll_s` 120, then back to the default | The tag sleeps 120 s, then 60 s; `config_pending: false` |
| 7 | Tag restart with the AP off | The content comes back from flash (T12); no CONTENT or LAYOUT when the AP returns |
| 8 | AP gone for an hour with new content waiting | The backoff reaches 600 s (T11); the tag finds the AP within 600 s and gets the content |
| 9 | AP gone for more than 2 hours | "No AP found" (T9); content again when the AP is back |
| 10 | Tag deleted on the AP | Registered again with no content, keeps its screen; a new PUT reaches it |

The simulated tag always reports 2950 mV and 20 °C. The scenarios use the `shelf` layout, so barcodes, fixed texts and five or six fields are covered by the tag and AP host tests, not end to end here.

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
