#!/usr/bin/env python3
"""
Builds the AP firmware for one or more boards (boards/<board>/board.json) with ESP-IDF.

  python build.py                    every board
  python build.py c6-supermini ...   only these boards
  python build.py --list             the boards: id, chip, status, file name
  python build.py --release          "<artifact> <release name>" per board (release.yml)
  python build.py --jobs             the CI build jobs as JSON, one per board and one per radio
                                     (firmware.yml builds them side by side)
  python build.py --no-radio <board>...  only the boards, not their radio firmware
  python build.py radio/c6 ...       only this radio firmware (<project>/<id>, as --jobs names it)

A board with "radio": "uart" in board.json (an AP host without 802.15.4, such as the ESP32-S3)
also builds the radio coprocessor firmware for each radio board its "radio_fw" names
(radio/boards/<id>/radio.json: c6, c6-lcd), each once, into out/ and the release as
<artifact>.bin only: nothing updates the radio over the air yet. A board with "radio": "net" (the
radio over WiFi, docs/ap-netradio.md) builds netradio/ the same way, for each of its
"radio_fw" (netradio/boards/<id>/radio.json: c6).

For each board: build-<board>/ with its own sdkconfig, then in out/
  <artifact>.bin       bootloader, partition table and app in one image, written at 0
                       (the web flasher and esptool)
  <artifact>-ota.bin   the app alone, for POST /api/ota (esl-client ota, the dashboard)
with <artifact> from board.json (openesl-ap for the C6 Super Mini, as before). Releases
name them after "release" in board.json (openesl-ap-esp32c6-<version>.bin, ...).

Needs idf.py on PATH (ESP-IDF v5.5.5: source export.sh). tools/check.py ap-fw runs this,
in the espressif/idf Docker image when idf.py is not installed.
"""
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BOARDS = os.path.join(HERE, "boards")
# the radio firmware per "radio" in board.json: its project and its boards
RADIO_PROJECTS = {"uart": "radio", "net": "netradio"}
OUT = os.path.join(HERE, "out")


def load_boards():
    boards = []
    for name in sorted(os.listdir(BOARDS)):
        path = os.path.join(BOARDS, name, "board.json")
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as f:
                b = json.load(f)
            if b.get("id") != name:
                sys.exit("%s: id must be the folder name (%s)" % (path, name))
            boards.append(b)
    return boards


def run(cmd, cwd=HERE):
    print("+ " + " ".join(cmd), flush=True)
    # the board decides the chip (CMakeLists.txt); an IDF_TARGET from the environment,
    # such as the one esp-idf-ci-action sets, would only clash with it
    env = {k: v for k, v in os.environ.items() if k != "IDF_TARGET"}
    if subprocess.run(cmd, cwd=cwd, env=env).returncode:
        sys.exit("failed: " + " ".join(cmd))


def radio_fws(b):
    """The radio boards (radio/boards/<id>/ or netradio/boards/<id>/) a board with "radio":
    "uart" or "net" works with: "radio_fw" is one id or a list of them"""
    fw = b.get("radio_fw") or []
    return [fw] if isinstance(fw, str) else list(fw)


def load_radios(boards):
    """The radio coprocessor firmware the boards use, each once, in board order."""
    radios = []
    for b in boards:
        kind = b.get("radio", "native")
        if kind not in ("native", "uart", "net"):
            sys.exit("board %s: radio must be \"native\", \"uart\" or \"net\"" % b["id"])
        if kind != "native" and not radio_fws(b):
            sys.exit("board %s: radio \"%s\" needs radio_fw, the radio's firmware" % (b["id"], kind))
        for name in radio_fws(b):
            project = RADIO_PROJECTS[kind]
            if any(r["id"] == name and r["project"] == project for r in radios):
                continue
            path = os.path.join(HERE, project, "boards", name, "radio.json")
            if not os.path.isfile(path):
                sys.exit("board %s: radio_fw %s has no %s" % (b["id"], name, path))
            with open(path, encoding="utf-8") as f:
                r = json.load(f)
            if r.get("id") != name:
                sys.exit("%s: id must be the folder name (%s)" % (path, name))
            r["project"] = project
            radios.append(r)
    return radios


def merge(target, bdir, app, artifact, cwd=HERE):
    """out/<artifact>.bin (everything, written at 0) and, given app, out/<artifact>-ota.bin (the app)."""
    os.makedirs(OUT, exist_ok=True)
    run([sys.executable, "-m", "esptool", "--chip", target, "merge_bin", "-o",
         os.path.join(OUT, artifact + ".bin"), "@flash_args"], cwd=os.path.join(cwd, bdir))
    if app:
        shutil.copyfile(os.path.join(cwd, bdir, app), os.path.join(OUT, artifact + "-ota.bin"))


def build(b):
    bdir = "build-" + b["id"]
    run(["idf.py", "-B", bdir, "-D", "SDKCONFIG=%s/sdkconfig" % bdir, "-D", "BOARD=" + b["id"], "build"])
    merge(b["target"], bdir, "esl_ap.bin", b["artifact"])
    print("%s: out/%s.bin and out/%s-ota.bin" % (b["id"], b["artifact"], b["artifact"]))


def build_radio(r):
    cwd = os.path.join(HERE, r["project"])
    bdir = "build-" + r["id"]
    run(["idf.py", "-B", bdir, "-D", "SDKCONFIG=%s/sdkconfig" % bdir, "-D", "RADIO_BOARD=" + r["id"], "build"], cwd=cwd)
    # No -ota.bin: nothing updates the radio over the air yet, and its app image would
    # otherwise sit next to the AP's as something to send to an AP (main/ota.c refuses it)
    merge(r["target"], bdir, None, r["artifact"], cwd=cwd)
    print("%s: out/%s.bin" % (r["id"], r["artifact"]))


def main():
    boards = load_boards()
    args = sys.argv[1:]
    if args == ["--list"]:
        for b in boards + load_radios(boards):
            print("%-20s %-8s %-12s %s" % (b["id"], b["target"], b["status"], b["artifact"]))
        return
    if args == ["--release"]:
        for b in boards + load_radios(boards):
            print(b["artifact"], b["release"])
        return
    if args == ["--jobs"]:
        jobs = [{"name": b["id"], "args": "--no-radio " + b["id"]} for b in boards]
        jobs += [{"name": "%s-%s" % (r["project"], r["id"]), "args": "%s/%s" % (r["project"], r["id"])}
                 for r in load_radios(boards)]
        print(json.dumps(jobs))
        return
    no_radio = "--no-radio" in args
    args = [a for a in args if a != "--no-radio"]
    by_id = {b["id"]: b for b in boards}
    radios = {"%s/%s" % (r["project"], r["id"]): r for r in load_radios(boards)}
    unknown = [a for a in args if a not in by_id and a not in radios]
    if unknown:
        sys.exit("unknown board %s; the boards are %s" % (", ".join(unknown), ", ".join(list(by_id) + list(radios))))
    if not shutil.which("idf.py"):
        sys.exit("idf.py not found: source ESP-IDF v5.5.5's export.sh first")
    chosen = [by_id[a] for a in args if a in by_id]
    chosen_radios = [radios[a] for a in args if a in radios]
    if not args:
        chosen = boards
    for b in chosen:
        build(b)
    if not no_radio:
        chosen_radios += [r for r in load_radios(chosen) if r not in chosen_radios]
    for r in chosen_radios:
        build_radio(r)


if __name__ == "__main__":
    main()
