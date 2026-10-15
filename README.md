# OpenESL

Open firmware for the SoluM ST-GM29XX2 price tags (ZBS243) and an access point (AP) on the ESP32-C6. The AP exposes a small HTTP API, so any software can send price and text straight to the tags.

```
your software ──HTTP + token──► ESP32-C6 (AP) ──802.15.4──► tags (ZBS243)
```

The AP is only an API. It does not know about products, shops or databases: it stores text fields per tag MAC and hands them to the tags. Anything that can make an HTTP request (a script, a web shop, a home automation system) can be a client.

**API documentation:** the API tab of the web page, [mathiasbk.github.io/openESL/#api](https://mathiasbk.github.io/openESL/#api): interactive, with test requests against your own AP through `tools/esl-client` (`esl_client.py serve`). The reference is [docs/openapi.yaml](docs/openapi.yaml) (OpenAPI 3.1), checked against the AP code in CI. The same page has the label designer and the flashers. An example client is in [tools/esl-client](tools/esl-client/README.md).

## Status

Early work in progress. The protocol, the AP core with its HTTP API, and the drawing code on the tag are written and tested on a PC. The hardware layer of the tag (clock, GPIO, radio, sleep) is not written yet, so the tag firmware does not run on a real tag yet. See [docs/fremdriftsplan.md](docs/fremdriftsplan.md).

## Folders

| Folder | Contents |
|---|---|
| [web/](web/README.md) | Web page: label designer, and flashing of tags and the AP from the browser |
| [docs/](docs/README.md) | Requirements, architecture, protocol, work plan, risks and hardware notes |
| [tag/](tag/) | Tag firmware for the ZBS243 (SDCC, 8051) |
| [ap/](ap/) | AP firmware for the ESP32-C6 (ESP-IDF) with the HTTP API |
| [shared/](shared/) | `proto.h`, shared packet definitions for tag and AP |
| [sim/](sim/README.md) | Tag firmware and AP core together on the PC, with a simulated radio link: end-to-end tests in CI, and a serial port for `esl-client` with no hardware |

Start with [docs/README.md](docs/README.md), then phase 0 in [docs/fremdriftsplan.md](docs/fremdriftsplan.md).

## Releases

The version is in [VERSION](VERSION). The tag firmware shows it on its start screen and sends it in POLL (1.2.3 as 10203), and the AP reports it as `ap.fw` in `GET /api/tags`.

To make a release, bump `VERSION` in a pull request. When it is merged to `master`, [release.yml](.github/workflows/release.yml) makes the release `v<VERSION>` from that commit: it builds the firmware and publishes a GitHub release with the tag firmware for each model, the merged AP image, `SHA256SUMS.txt`, and notes listing the pull requests since the previous release. The web page is then published again, and its flashers serve that release.

The same workflow can be started by hand (Actions → Release → Run workflow) to release the current `VERSION` if that release does not exist yet, or by pushing a tag that matches `VERSION` (`git tag v0.2.0 && git push origin v0.2.0`). A tag with a suffix (`v0.2.0-rc1`) becomes a pre-release, which the web page does not use.

Every pull request and push to `master` runs [web.yml](.github/workflows/web.yml): host tests, both tag models, the AP and the web page with its tests.

## A clean implementation

OpenESL is written from scratch. It does not contain code from other firmware for the same hardware, and it uses its own radio protocol. Third-party code is only used where its licence allows it (ESP-IDF, the Inter font). Hardware facts the code relies on, and where they come from, are listed in [docs/kildelogg.md](docs/kildelogg.md).

## Credits

This project would be much harder without the people who opened up these tags first:

- **Dmitry Grinberg** ([dmitry.gr](https://dmitry.gr)) for the reverse engineering of the ZBS243 and the SoluM tags.
- **atc1441** for [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher), the flasher used to read and write the tags, and for a lot of public research and videos on these tags.
- The **[OpenEPaperLink](https://github.com/OpenEPaperLink)** project and its contributors for documenting the tags, pinouts and flashing.

## Licence

Not decided yet.
