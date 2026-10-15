# OpenESL

Open firmware for the SoluM ST-GM29XX2 price tags (ZBS243) and an access point (AP) on the ESP32-C6. The AP exposes a small HTTP API, so any software can send price and text straight to the tags.

```
your software ──HTTP + token──► ESP32-C6 (AP) ──802.15.4──► tags (ZBS243)
```

The AP is only an API. It does not know about products, shops or databases: it stores text fields per tag MAC and hands them to the tags. Anything that can make an HTTP request (a script, a web shop, a home automation system) can be a client.

**API documentation:** the API tab of the web page, [mathiasbk.github.io/openESL/#api](https://mathiasbk.github.io/openESL/#api), made from [docs/protokoll.md](docs/protokoll.md) parts 2 and 3. The same page has the label designer and the flashers. An example client is in [tools/esl-client](tools/esl-client/README.md).

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

Start with [docs/README.md](docs/README.md), then phase 0 in [docs/fremdriftsplan.md](docs/fremdriftsplan.md).

## A clean implementation

OpenESL is written from scratch. It does not contain code from other firmware for the same hardware, and it uses its own radio protocol. Third-party code is only used where its licence allows it (ESP-IDF, the Inter font). Hardware facts the code relies on, and where they come from, are listed in [docs/kildelogg.md](docs/kildelogg.md).

## Credits

This project would be much harder without the people who opened up these tags first:

- **Dmitry Grinberg** ([dmitry.gr](https://dmitry.gr)) for the reverse engineering of the ZBS243 and the SoluM tags.
- **atc1441** for [ZBS_Flasher](https://github.com/atc1441/ZBS_Flasher), the flasher used to read and write the tags, and for a lot of public research and videos on these tags.
- The **[OpenEPaperLink](https://github.com/OpenEPaperLink)** project and its contributors for documenting the tags, pinouts and flashing.

## Licence

Not decided yet.
