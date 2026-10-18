# Contributing to OpenESL

Thank you for helping. This page is how a change goes from an idea to tags on a shelf. It is short on purpose; ask in an issue if something is unclear.

## 1. Start with an issue

Open an issue for a bug, a new piece of hardware or a feature, with the matching template. Small fixes (a typo, a broken link) can go straight to a pull request.

A change that is hard to undo needs a short design note before the code, in the issue or in `docs/`. Hard to undo means anything tags or APs already in use depend on:

- flash layout, partition table, bootloader
- the settings stored on the AP or the tag
- the radio protocol and tag OTA
- the HTTP API (breaking changes)

## 2. Branches

- `development` is where all pull requests go.
- `master` is the release branch. Nothing is pushed or merged to it except a release (see [docs/release.md](docs/release.md)).
- Branch from `development` with a short descriptive name, e.g. `ap-flash-keep-settings`.

## 3. Build and test locally

Run what CI runs before you push:

```
python tools/check.py            # every step (Linux or WSL, after tools/dev/setup.sh)
python tools/check.py --docker   # every step in Docker (Windows, macOS)
python tools/check.py tag web    # only some steps; --list shows them
```

CI is the last check, not the place to find out whether the code builds. If you change firmware, also try it on real hardware if you can, and say in the pull request whether you did.

New hardware facts (registers, timings, pinouts) go in [docs/kildelogg.md](docs/kildelogg.md) with where they came from. Do not copy code from other firmware for the same hardware: OpenESL is written from scratch (see "A clean implementation" in the README).

## 4. Pull request

- Target `development`.
- Fill in the template: **Summary** (a small diagram, pseudocode or a trimmed diff), **Evidence** (before/after, what you ran, which hardware), and **Merge danger** (one-way or two-way door, and what breaks if it is wrong).
- Keep it to one change. Docs that describe the change go in the same pull request.
- Title and body in English.

## 5. Review and merge

- Every pull request is reviewed before it is merged, and CI must be green.
- One-way changes (the list in section 1) are tested on real hardware before they are merged to `development`, not only before the release.
- Feature pull requests are squash-merged into `development`, so the history has one commit per pull request.

## 6. Release

Releases are made from `development` by the maintainer. The steps, the hardware checklist, how new firmware is rolled out to the tags and how to roll back are in [docs/release.md](docs/release.md).

## Security

Do not report security problems in a public issue. See [SECURITY.md](SECURITY.md).
