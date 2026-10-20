# Shared code

`proto.h` holds the packet definitions used by both the tag (SDCC) and the AP (GCC). Fixed-width types, no bit-fields, explicit little-endian. See [docs/protokoll.md](../docs/protokoll.md).

## Tag models: `models.json`

The one list of tag models. Everything else is generated from it or reads it, so a new model
is one entry here, an example layout file (`tag/designs/layouts/<name>.json`), a panel header
(`tag/src/panels/<panel>.h`, unless it drives the screen like one that exists) and
`python tools/gen_models.py`:

| Field | Meaning |
|---|---|
| `code` | the model byte in POLL and in tag firmware images; 1, 2, 3, ... without gaps (0 is "unknown") |
| `build` | `-DMODEL=<build>` for the tag firmware and the web renderer; `ESL_MODEL_<build>` in C |
| `name` | the name in the API, layout files and release files (`/api/layouts/2.9`, `openesl-tag-2.9-v1.0.0.bin`) |
| `id` | a stable id for the model (`solum-m2-29-bw`), for when two models share a size |
| `vendor` | the maker, for people (`SoluM`); the README's list of supported tags shows it |
| `label`, `product` | what the tag is, for people (`ST-GM29XX2`) |
| `chip`, `radio` | the tag's chip and radio; only `zbs243` and `802.15.4` so far |
| `image` | how the AP sends the screen to the tag: `planes-1bpp` is one bit per pixel in layout orientation, the black plane and then the red one (`IMG` in `proto.h`); the tag turns it for its panel |
| `width`, `height` | the screen in the orientation the layouts use |
| `colors` | `black`, `white`, and `red` on a three-colour panel: layout files for the model may then use the colours `red` and `red_invert` (`yellow` is refused until the firmware draws it) |
| `mac_code` | byte 6 of the MAC on a tag of this model (the flashers check it) |
| `panel` | how the tag firmware drives the screen: `tag/src/panels/<panel>.h` (RAM direction, update sequence, init commands; `model.h` lists the macros it must define, `EPD_SLEEP_CTRL1` is optional) |
| `photo` | optional: a picture of the tag under `web/` with its size, for the flasher's model picker |
| `status` | `supported`, `experimental` or `planned` |

`tools/gen_models.py` checks the file and writes `models_gen.h` (here; `ESL_MODEL_*`,
`ESL_MODEL_COUNT`, `ESL_MODEL_LIST`, included by `proto.h`), `tag/src/model_gen.h`,
`web/js/models.js`, the model block in `tools/esl-client/esl_client.py` and the list of
supported tags in the top `README.md`. The generated
files are checked in; `tools/check.py models` and CI fail when they are out of date. The
Python tools in the repository read the JSON through `models.py`.
