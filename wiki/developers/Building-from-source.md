# Building from source

`python tools/check.py` builds and tests the same things as CI, on your own machine, and puts the firmware in `out/` with the names CI gives it.

```sh
python tools/check.py --list        # the steps
python tools/check.py tag web       # only some of them
```

A step whose tools are missing is skipped and says what it needs.

## Setting up

- **Linux or WSL:** run `tools/dev/setup.sh` once. It installs SDCC 4.5.0, Emscripten 3.1.74 (and Node 22 if needed) and the Python packages. The AP is built with ESP-IDF v5.5.5 if it is active in the shell, otherwise in the `espressif/idf:v5.5.5` Docker image.
- **Windows or macOS:** with Docker Desktop running, `python tools/check.py --docker` runs every step in one image with all the tools.

## The parts one by one

| Part | Build | Tool versions |
|---|---|---|
| Label firmware | `python tag/tools/build.py fw` (2.9"), `MODEL=154 python tag/tools/build.py fw` (1.54") | SDCC **4.5.0** exactly |
| AP firmware | `cd ap && python build.py <board>`; for a board with the Design tab (S3, C5) run `python web/build.py --wasm` first, and again after a change to the drawing code (the build stops and says so) | ESP-IDF v5.5.5, Emscripten 3.1.74 for the Design tab |
| Web page | `python web/build.py`, then `node --test web/test/*.test.mjs` | Emscripten 3.1.74 |
| Simulator | see [sim/README.md](https://github.com/mathiasbk/openESL/blob/master/sim/README.md) | a C compiler |

## Flashing what you built

- **AP:** `idf.py -B build-<board> -p /dev/ttyACM0 flash monitor` keeps the AP's data, like the web page. See [Flashing the AP](Flashing-the-access-point).
- **Label:** with the jig and flash-tui, or over the air with `esl_client.py tag-ota tag/build/fw/openesl-ota.bin`.

## Building with your own settings

`idf.py menuconfig` → "OpenESL AP" has the radio channel, PAN ID, TX power, the most labels to keep, and which features are on. See [ap/README.md](https://github.com/mathiasbk/openESL/blob/master/ap/README.md#configuration).
