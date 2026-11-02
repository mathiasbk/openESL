# Shared code

`proto.h` holds the packet definitions used by both the tag (SDCC) and the AP (GCC). Fixed-width types, no bit-fields, explicit little-endian. See [docs/protokoll.md](../docs/protokoll.md).

## Tag models: `models.json`

The one list of tag models. Everything else is generated from it or reads it, so a new model
is one entry here, an example layout file (`tag/designs/layouts/<name>.json`, or `same_as` for a
screen a model has already), a panel header
(`tag/src/panels/<panel>.h`, unless it drives the screen like one that exists) and
`python tools/gen_models.py`:

| Field | Meaning |
|---|---|
| `code` | the model byte in POLL and in tag firmware images; 1, 2, 3, ... without gaps (0 is "unknown") |
| `build` | `-DMODEL=<build>` for the tag firmware and the web renderer; `ESL_MODEL_<build>` in C |
| `name` | the name in the API, layout files and release files (`/api/layouts/2.9`, `openesl-tag-2.9-v1.0.0.bin`) |
| `id` | a stable id for the model (`solum-m2-29-bw`), for when two models share a size |
| `vendor` | the maker, for people (`SoluM`); the README's list of supported tags shows it |
| `short` | optional: the model as the web page and tools show it to people (`2.9" red`); left out it is the `name` and a `"` (`2.9"`). A model with status `experimental` is shown with "(experimental)" after it |
| `label`, `product` | what the tag is, for people (`ST-GM29XX2`) |
| `chip`, `radio` | the tag's chip and the radio it has (`zbs243`, `802.15.4`); a chip the firmware is built for is in `CHIPS` in `tag/tools/build.py` (see "Another chip" in `tag/README.md`) |
| `protocol` | what the AP speaks with the tag over that radio: `openesl` is ours (`proto.h`); a tag with its maker's firmware would be another |
| `image` | how the AP sends the screen: `openesl-raw` is `ESL_IMG_FMT_RAW` in `proto.h` (screen coordinates, column by column, black plane then red), coded (`ESL_IMG_FMT_2D` or `ESL_IMG_FMT_RLE`) for a tag that takes it; the tag turns it for its panel |
| `width`, `height` | the screen in the orientation the layouts use |
| `colors` | `black`, `white`, and `red` on a three-colour panel: layout files for the model may then use the colours `red` and `red_invert` (`yellow` is refused until the firmware draws it) |
| `mac_code` | byte 6 of the MAC on a tag of this model (the flashers check it) |
| `panel` | how the tag firmware drives the screen: `tag/src/panels/<panel>.h` (RAM direction, update sequence, init commands; `model.h` lists the macros it must define, `EPD_SLEEP_CTRL1` is optional) |
| `photo` | optional: a picture of the tag under `web/` with its size, for the flasher's model picker |
| `same_as` | optional: the `name` of a model with the same screen (`width`, `height`, `colors`) whose layout file this model uses (#181), e.g. the same panel sold by another maker. The AP draws its tags with that model's file, `PUT /api/layouts/{model}` takes only that one, and the designer edits it as one file for both. The model then has no example layout file of its own (`tag/designs/layouts/` has only the other's). The generator refuses another screen size or colours, a model that is not built, and a chain (the named model may not have `same_as` itself) |
| `status` | `supported`, `experimental` or `planned`. A `planned` model is a tag someone has asked for: it needs only `id`, `vendor`, `label`, `product`, `chip`, `radio`, `protocol`, `width`, `height`, `colors` and `status`, shows in the README's list, and nothing is built or generated for it |

`tools/gen_models.py` checks the file and writes `models_gen.h` (here; `ESL_MODEL_*`,
`ESL_MODEL_COUNT`, `ESL_MODEL_LIST`, included by `proto.h`), `tag/src/model_gen.h`,
`web/js/models.js`, the model block in `tools/esl-client/esl_client.py` and the list of
supported tags in the top `README.md`. The generated
files are checked in; `tools/check.py models` and CI fail when they are out of date. The
Python tools in the repository read the JSON through `models.py`.

A new model also wants a variant of each layout template for its screen (`templates/`, see
[wiki/labels/Label-templates.md](../wiki/labels/Label-templates.md)): `python web/build.py` lists
the templates that have no variant for it yet, as a to-do for the model's pull request.
