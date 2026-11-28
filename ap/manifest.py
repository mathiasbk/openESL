#!/usr/bin/env python3
"""
The AP's capability manifest: what each board can do, and what its build has.

  ap/protocols.json           the tag protocols the firmware can speak: the radio each needs,
                              the tag models it covers, the Kconfig symbol and sdkconfig file
                              that turn it on
  boards/<board>/board.json   "radios": the radios the board has and how it reaches them,
                              "protocols": what its build has, "variants": other builds of
                              the same board (the Bluetooth AP, the sniffer)

CMakeLists.txt and build.py both build from here, so they agree on what a build reads:

  python manifest.py cmake <board> <variant|-> <build dir>
      writes <build dir>/manifest.sdkconfig (the radio driver, and the protocols that are off),
      then prints four lines for CMake: the sdkconfig.defaults files the build reads, in order
      (separated by "|", relative to ap/ or absolute), the protocols as "openesl,gicisky" ("-"
      for none), the app's name, and the Kconfig symbols those protocols turn on ("-" for none),
      which CMakeLists.txt checks against the sdkconfig Kconfig made
  python manifest.py check    checks every board and variant against protocols.json
  python manifest.py table    what each board's builds have, in Markdown

The radios ("radios" in board.json):
  "802.15.4": {"via": "native"}      the AP chip's own (C6, C5; main/radio_ieee802154.c)
              {"via": "uart", "radio_fw": [...]}   a C6 running radio/ on UART (main/radio_uart.c)
              {"via": "net", "radio_fw": [...]}    a C6 running netradio/ over WiFi (main/radio_net.c)
  "ble":      {"via": "native"}      the AP chip's own Bluetooth LE (main/ble.c)
Without an "802.15.4" entry the build has the stub radio mode with no simulated tags (802.15.4
off). The radio boards in "radio_fw" are built with the board (build.py).

A protocol ("protocols.json") needs a radio of its "radio" kind, and every word of its "needs"
in that radio's entry: a "via" value or a flag set to true on it. "conflicts" names radios
the build must not have with it: a kind ("802.15.4"), or a kind reached one way ("802.15.4/native")
(its Kconfig symbol depends on that, main/Kconfig.projbuild).
The sniffer variant answers no tags, so it has no protocols. Kconfig has the last word: a
symbol whose "depends on" is not met is dropped, and CMakeLists.txt then stops the build.

A variant ("variants" in board.json) is a build of the same board with its own "radios",
"protocols" (each falls back to the board's when left out), "sdkconfig" (a file read last) and
"app" (the app's project name, esl_ap by default). build.py --variant <name> builds it into
out/<artifact>-<name>.bin.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BOARDS = os.path.join(HERE, "boards")
PROTOCOLS = os.path.join(HERE, "protocols.json")
sys.path.insert(0, os.path.join(HERE, "..", "shared"))
import models  # noqa: E402  (shared/models.py)

RADIO_KINDS = ("802.15.4", "ble")
VIAS = {"802.15.4": ("native", "uart", "net"), "ble": ("native",)}
# the Kconfig radio driver for each way to the 802.15.4 radio (main/Kconfig.projbuild)
DRIVERS = {"native": "ESL_RADIO_DRIVER_IEEE802154", "uart": "ESL_RADIO_DRIVER_UART", "net": "ESL_RADIO_DRIVER_NET"}
FRAGMENT = "manifest.sdkconfig"


class ManifestError(Exception):
    pass


def load_protocols(path=PROTOCOLS):
    with open(path, encoding="utf-8") as f:
        ps = json.load(f)["protocols"]
    by_id = {}
    for p in ps:
        if p["id"] in by_id:
            raise ManifestError("protocols.json: %s twice" % p["id"])
        if p.get("radio") not in RADIO_KINDS:
            raise ManifestError("protocols.json: %s: radio must be one of %s" % (p["id"], ", ".join(RADIO_KINDS)))
        sdk = p.get("sdkconfig")
        if sdk and not os.path.isfile(os.path.join(HERE, sdk)):
            raise ManifestError("protocols.json: %s: no %s" % (p["id"], sdk))
        by_id[p["id"]] = p
    return by_id


def protocol_models(p, ms=None):
    """The tag models a protocol covers: "protocol" = the models in shared/models.json whose
    protocol it is, or a list of their "build" names"""
    ms = ms or models.load()
    if p.get("models") == "protocol":
        return [m["build"] for m in ms if m.get("protocol") == p["id"]]
    return list(p.get("models") or [])


def load_board(name):
    path = os.path.join(BOARDS, name, "board.json")
    with open(path, encoding="utf-8") as f:
        b = json.load(f)
    if b.get("id") != name:
        raise ManifestError("%s: id must be the folder name (%s)" % (path, name))
    return normalize(b)


def load_boards():
    return [load_board(n) for n in sorted(os.listdir(BOARDS))
            if os.path.isfile(os.path.join(BOARDS, n, "board.json"))]


def normalize(b):
    """Fills in what the older readers of board.json take: "radio" (how the 802.15.4 radio is
    reached, "native" when there is none) and "radio_fw" (its radio boards)"""
    radios = b.get("radios")
    if not isinstance(radios, dict) or not radios:
        raise ManifestError("board %s: no \"radios\"" % b.get("id"))
    r154 = radios.get("802.15.4") or {}
    b["radio"] = r154.get("via", "native")
    fw = r154.get("radio_fw") or []
    b["radio_fw"] = [fw] if isinstance(fw, str) else list(fw)
    b.setdefault("protocols", [])
    b.setdefault("variants", {})
    return b


def resolve(b, variant=None):
    """The build of board b (a variant of it when variant is given): its radios, protocols,
    extra sdkconfig file and app name"""
    if not variant:
        return {"radios": b["radios"], "protocols": list(b["protocols"]), "sdkconfig": None, "app": "esl_ap"}
    v = b["variants"].get(variant)
    if v is None:
        raise ManifestError("board %s has no variant %s (it has: %s)" % (b["id"], variant, ", ".join(b["variants"]) or "none"))
    return {"radios": v.get("radios", b["radios"]), "protocols": list(v.get("protocols", b["protocols"])),
            "sdkconfig": v.get("sdkconfig"), "app": v.get("app", "esl_ap")}


def check_build(b, variant=None, protocols=None):
    """The reasons a board's build (or variant) cannot be built as its manifest says; [] if it can"""
    protocols = protocols or load_protocols()
    r = resolve(b, variant)
    where = "board %s%s" % (b["id"], " variant " + variant if variant else "")
    errors = []
    for kind, radio in r["radios"].items():
        if kind not in RADIO_KINDS:
            errors.append("%s: radio %s is not one of %s" % (where, kind, ", ".join(RADIO_KINDS)))
        elif radio.get("via") not in VIAS[kind]:
            errors.append("%s: radio %s: via must be one of %s" % (where, kind, ", ".join(VIAS[kind])))
        elif radio.get("via") in ("uart", "net") and not radio.get("radio_fw"):
            errors.append("%s: radio %s via %s needs radio_fw, the radio's firmware" % (where, kind, radio["via"]))
    for pid in r["protocols"]:
        p = protocols.get(pid)
        if not p:
            errors.append("%s: protocol %s is not in protocols.json" % (where, pid))
            continue
        radio = r["radios"].get(p["radio"])
        if not radio:
            errors.append("%s: protocol %s needs a %s radio" % (where, pid, p["radio"]))
            continue
        for c in p.get("conflicts", []):
            kind, _, via = c.partition("/")
            other = r["radios"].get(kind)
            if other and (not via or other.get("via") == via):
                errors.append("%s: protocol %s cannot be built with a %s radio" % (where, pid, c))
        have = {radio.get("via")} | {k for k, v in radio.items() if v is True}
        for need in p.get("needs", []):
            if need not in have:
                errors.append("%s: protocol %s needs its %s radio to be %s" % (where, pid, p["radio"], need))
    if variant == "sniffer" and r["protocols"]:
        errors.append("%s: the sniffer answers no tags, so it has no protocols" % where)
    if r["sdkconfig"] and not os.path.isfile(os.path.join(HERE, r["sdkconfig"])):
        errors.append("%s: no %s" % (where, r["sdkconfig"]))
    return errors


def check_all():
    protocols = load_protocols()
    errors = []
    for b in load_boards():
        for v in [None] + list(b["variants"]):
            errors += check_build(b, v, protocols)
    return errors


def fragment(b, variant=None, protocols=None):
    """The sdkconfig the manifest writes for a build: the radio driver (or the stub radio mode
    without 802.15.4), and every protocol the build does not have turned off"""
    protocols = protocols or load_protocols()
    r = resolve(b, variant)
    lines = ["# Written by manifest.py from boards/%s/board.json%s and protocols.json: do not edit"
             % (b["id"], " (variant %s)" % variant if variant else "")]
    r154 = r["radios"].get("802.15.4")
    if r154:
        lines.append("CONFIG_%s=y" % DRIVERS[r154["via"]])
    else:
        lines += ["CONFIG_ESL_RADIO_STUB=y", "CONFIG_ESL_STUB_TAGS=0", "CONFIG_IEEE802154_ENABLED=n"]
    for pid, p in protocols.items():
        if p.get("kconfig") and pid not in r["protocols"]:
            lines.append("# CONFIG_%s is not set" % p["kconfig"])
    return "\n".join(lines) + "\n"


def defaults_files(b, variant=None, bdir="build", protocols=None):
    """The sdkconfig.defaults a build reads, in order, relative to ap/: the common one, the
    "base" board's, the board's own, the manifest's (in bdir), each protocol's, the variant's"""
    protocols = protocols or load_protocols()
    r = resolve(b, variant)
    files = ["sdkconfig.defaults"]
    if b.get("base"):
        files.append("boards/%s/sdkconfig.defaults" % b["base"])
    files.append("boards/%s/sdkconfig.defaults" % b["id"])
    files.append(os.path.join(bdir, FRAGMENT))
    files += [protocols[p]["sdkconfig"] for p in r["protocols"] if protocols[p].get("sdkconfig")]
    if r["sdkconfig"]:
        files.append(r["sdkconfig"])
    return files


def write_fragment(b, variant, bdir):
    """Writes bdir/manifest.sdkconfig (bdir relative to ap/ or absolute) when it changed"""
    path = os.path.join(HERE, bdir, FRAGMENT)
    text = fragment(b, variant)
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return path
    except OSError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


def table():
    protocols = load_protocols()
    lines = ["| Board | Build | 802.15.4 | Bluetooth | Protocols |", "|---|---|---|---|---|"]
    for b in load_boards():
        for v in [None] + list(b["variants"]):
            r = resolve(b, v)
            # a radio the board has but no protocol of this build uses is not in the build
            used = {protocols[p]["radio"] for p in r["protocols"]}
            via = lambda kind: r["radios"][kind]["via"] if kind in used and kind in r["radios"] else "-"
            if v == "sniffer":
                used.add("802.15.4")
            lines.append("| %s | %s | %s | %s | %s |" % (
                b["id"], v or "standard", via("802.15.4"), via("ble"),
                ", ".join(protocols[p]["label"] for p in r["protocols"]) or "-"))
    return "\n".join(lines)


def main(args):
    try:
        if args[:1] == ["cmake"] and len(args) == 4:
            b = load_board(args[1])
            variant = None if args[2] in ("-", "") else args[2]
            errors = check_build(b, variant)
            if errors:
                raise ManifestError("\n".join(errors))
            bdir = os.path.abspath(args[3])
            write_fragment(b, variant, bdir)
            files = defaults_files(b, variant, bdir)
            print("|".join(f.replace("\\", "/") for f in files))
            print(",".join(resolve(b, variant)["protocols"]) or "-")
            print(resolve(b, variant)["app"])
            protocols = load_protocols()
            print(",".join(protocols[p]["kconfig"] for p in resolve(b, variant)["protocols"]
                           if protocols[p].get("kconfig")) or "-")
            return 0
        if args == ["check"]:
            errors = check_all()
            for e in errors:
                print(e, file=sys.stderr)
            if not errors:
                print("manifest: every board and variant fits protocols.json")
            return 1 if errors else 0
        if args == ["table"]:
            print(table())
            return 0
    except ManifestError as e:
        print("manifest: %s" % e, file=sys.stderr)
        return 1
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
