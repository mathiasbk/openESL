# Contributing

Help is welcome, and most of all with hardware that has not been tried yet:

- **Other labels:** more SoluM sizes and models, other ZBS243 labels, and labels with other chips. See [Adding hardware](Adding-hardware).
- **Other access points:** other ESP32 boards, or radios other than the ESP32-C6.
- **Other flashers:** boards and jigs other than the D1 mini and the pogo-pin jig.
- **Reports from the experimental boards** ([ESP32-C5](ESP32-C5), the [ESP32-S3 + C6](ESP32-S3-and-C6) with a screen on the C6).

## Easy ways to start

No C or firmware needed for these:

- **Photos and details of a label we do not support yet**, in a [hardware issue](https://github.com/mathiasbk/openESL/issues/new?template=hardware.yml): both sides of the board, and what is written on the chip, the display cable and the back.
- **A report from a board marked experimental**: did it flash, did labels show up?
- **A new [template](Label-templates)**, a JSON file in `templates/` with a layout for each label model.
- **A clearer sentence in this wiki.** The pages are in [wiki/](https://github.com/mathiasbk/openESL/tree/master/wiki); edit one on GitHub and send a pull request.
- **A question in an issue** when something here was hard to follow. That tells us what to explain better.

Bug reports and fixes for everything else are just as welcome. Security problems go to [SECURITY.md](https://github.com/mathiasbk/openESL/blob/master/SECURITY.md), not to an issue.

## How a change is made

1. Open an issue, or go straight to a pull request for small things.
2. Branch from `development`, and send the pull request there. `master` is the release branch.
3. Run `python tools/check.py` before you push ([Building from source](Building-from-source)).
4. Fill in the pull request template: what changed, how you tested it, and anything that cannot be undone.

The whole process: [CONTRIBUTING.md](https://github.com/mathiasbk/openESL/blob/master/CONTRIBUTING.md). How releases are made: [docs/release.md](https://github.com/mathiasbk/openESL/blob/master/docs/release.md).

## Where things are

| Read | For |
|---|---|
| [docs/README.md](https://github.com/mathiasbk/openESL/blob/master/docs/README.md) | Requirements, architecture and the main choices |
| [docs/protokoll.md](https://github.com/mathiasbk/openESL/blob/master/docs/protokoll.md) | The radio protocol, the HTTP API and the layout format |
| [ap/README.md](https://github.com/mathiasbk/openESL/blob/master/ap/README.md) | The AP firmware |
| [tag/README.md](https://github.com/mathiasbk/openESL/blob/master/tag/README.md) | The label firmware |
| [web/README.md](https://github.com/mathiasbk/openESL/blob/master/web/README.md) | The web page: designer, flashers, dashboard |
| [docs/kildelogg.md](https://github.com/mathiasbk/openESL/blob/master/docs/kildelogg.md) | Hardware facts the code relies on, and where each came from |

## Clean implementation

OpenESL is written from scratch and does not contain code from other firmware for the same hardware. Please keep it that way: no copied code or binaries from other projects, unless their licence allows it and it is agreed first.
