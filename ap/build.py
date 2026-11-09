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
  python build.py --sizes DIR [--base DIR]
                                     the flash budget table for the images in DIR (the
                                     <artifact>.size.json files a build leaves in out/), against
                                     the images in --base (an older out/ or ap-firmware artifact);
                                     added to $GITHUB_STEP_SUMMARY in CI, exit code 1 over budget

A board with "radio": "uart" in board.json (an AP host without 802.15.4, such as the ESP32-S3)
also builds the radio coprocessor firmware for each radio board its "radio_fw" names
(radio/boards/<id>/radio.json: c6, c6-lcd), each once, into out/ and the release as
<artifact>.bin and <artifact>-ota.bin: the AP passes the latter on to its radio over the UART
link (POST /api/ota, docs/ap-radio-link.md). A board with "radio": "ble" (Bluetooth tags, docs/ble-tags.md) has no radio firmware.
A board with "radio": "net" (the radio over WiFi,
docs/ap-netradio.md) builds netradio/ the same way, for each of its "radio_fw"
(netradio/boards/<id>/radio.json: c6), as <artifact>.bin only: nothing updates it over the
air yet.

For each board: build-<board>/ with its own sdkconfig, then in out/
  <artifact>.bin       bootloader, partition table and app in one image, written at 0
                       (the web flasher and esptool)
  <artifact>-ota.bin   the app alone, for POST /api/ota (esl-client ota, the dashboard)
  <artifact>.elf       the symbols of that app, to decode a panic backtrace (not released)
with <artifact> from board.json (openesl-ap for the C6 Super Mini, as before). Releases
name them after "release" in board.json (openesl-ap-esp32c6-<version>.bin, ...).

Flash budget: every build also writes out/<artifact>.size.json (the app's size and its slot
in the partition table) and checks that at least BUDGET_FAIL bytes of the slot are left. The
partition table cannot grow on an AP in the field without a USB flash of every AP, so the
4 MB ESP32-C6 boards (1.5 MB slots) must keep room for what comes next: the build fails
with less than 64 KB free and warns with less than 128 KB (BUDGET_WARN).

Needs idf.py on PATH (ESP-IDF v5.5.5: source export.sh). tools/check.py ap-fw runs this,
in the espressif/idf Docker image when idf.py is not installed.
"""
import glob
import json
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BOARDS = os.path.join(HERE, "boards")
# the radio firmware per "radio" in board.json: its project and its boards
RADIO_PROJECTS = {"uart": "radio", "net": "netradio"}
OUT = os.path.join(HERE, "out")
# The flash budget: the free bytes an app slot must keep (fail below) and should keep (warn below)
BUDGET_FAIL = 64 * 1024
BUDGET_WARN = 128 * 1024


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
        if kind not in ("native", "uart", "net", "ble"):
            sys.exit("board %s: radio must be \"native\", \"uart\", \"net\" or \"ble\"" % b["id"])
        if kind in ("uart", "net") and not radio_fws(b):
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


def app_slot(table):
    """The size of the smallest app partition in a built partition table (partition-table.bin:
    32-byte entries, magic 0x50AA, type 0 = app), which is what an OTA image has to fit"""
    with open(table, "rb") as f:
        data = f.read()
    sizes = []
    for i in range(0, len(data) - 31, 32):
        magic, kind, _sub, _offset, size = struct.unpack_from("<HBBII", data, i)
        if magic != 0x50AA:
            break
        if kind == 0:
            sizes.append(size)
    if not sizes:
        sys.exit("%s: no app partition" % table)
    return min(sizes)


def record_size(name, artifact, bdir, app, cwd=HERE):
    """out/<artifact>.size.json: the app's size and its slot, for the budget (--sizes)"""
    size = os.path.getsize(os.path.join(cwd, bdir, app))
    slot = app_slot(os.path.join(cwd, bdir, "partition_table", "partition-table.bin"))
    info = {"id": name, "artifact": artifact, "app": size, "slot": slot}
    with open(os.path.join(OUT, artifact + ".size.json"), "w", encoding="utf-8") as f:
        json.dump(info, f, indent=1)
        f.write("\n")
    return info


def budget_state(free):
    return "over" if free < BUDGET_FAIL else "low" if free < BUDGET_WARN else "ok"


def kb(n):
    return "%.1f KB" % (n / 1024)


def kb_limit(n):
    return "%d KB" % (n // 1024)


def check_budget(info):
    """Prints the image's room in its slot; False when it is under BUDGET_FAIL"""
    free = info["slot"] - info["app"]
    state = budget_state(free)
    line = "%s: app %s of a %s slot, %s free (%d%%)" % (info["id"], kb(info["app"]), kb(info["slot"]), kb(free),
                                                       100 * free // info["slot"])
    if state == "over":
        print("::error::%s, less than the %s the flash budget keeps free" % (line, kb_limit(BUDGET_FAIL)), flush=True)
    elif state == "low":
        print("::warning::%s, less than %s: make room before adding more" % (line, kb_limit(BUDGET_WARN)), flush=True)
    else:
        print(line, flush=True)
    return state != "over"


def build(b):
    bdir = "build-" + b["id"]
    run(["idf.py", "-B", bdir, "-D", "SDKCONFIG=%s/sdkconfig" % bdir, "-D", "BOARD=" + b["id"], "build"])
    merge(b["target"], bdir, "esl_ap.bin", b["artifact"])
    # the symbols for this image: a panic backtrace (the AP is built with silent assertions,
    # sdkconfig.defaults) is decoded with it, e.g. idf.py monitor --elf or addr2line
    shutil.copyfile(os.path.join(HERE, bdir, "esl_ap.elf"), os.path.join(OUT, b["artifact"] + ".elf"))
    print("%s: out/%s.bin, out/%s-ota.bin and out/%s.elf" % (b["id"], b["artifact"], b["artifact"], b["artifact"]))
    return record_size(b["id"], b["artifact"], bdir, "esl_ap.bin")


def build_radio(r):
    cwd = os.path.join(HERE, r["project"])
    bdir = "build-" + r["id"]
    run(["idf.py", "-B", bdir, "-D", "SDKCONFIG=%s/sdkconfig" % bdir, "-D", "RADIO_BOARD=" + r["id"], "build"], cwd=cwd)
    # The radio on UART takes its -ota.bin through the AP (main/ota.c passes an esl_radio image
    # on); the one on the network has no way to take it yet
    app = "esl_radio.bin" if r["project"] == "radio" else None
    merge(r["target"], bdir, app, r["artifact"], cwd=cwd)
    print("%s: out/%s.bin%s" % (r["id"], r["artifact"], " and out/%s-ota.bin" % r["artifact"] if app else ""))
    return record_size("%s/%s" % (r["project"], r["id"]), r["artifact"], bdir,
                       "esl_radio.bin" if r["project"] == "radio" else "esl_netradio.bin", cwd=cwd)


def base_size(base, artifact):
    """The app size of artifact in an older build: its .size.json, or else its -ota.bin (the app
    alone), as builds from before the budget left them; None when it is not there"""
    if not base:
        return None
    path = os.path.join(base, artifact + ".size.json")
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            return json.load(f)["app"]
    path = os.path.join(base, artifact + "-ota.bin")
    return os.path.getsize(path) if os.path.isfile(path) else None


def size_report(infos, base=None):
    """The budget table in Markdown, and whether every image keeps BUDGET_FAIL free"""
    icon = {"ok": "ok", "low": ":warning: under %s" % kb_limit(BUDGET_WARN), "over": ":x: under %s" % kb_limit(BUDGET_FAIL)}
    lines = ["### AP flash budget", "",
             "Free space in each image's app slot: the build fails under %s and warns under %s (ap/build.py)."
             % (kb_limit(BUDGET_FAIL), kb_limit(BUDGET_WARN)), "",
             "| Image | App | Slot | Free | Change | |", "|---|---:|---:|---:|---:|---|"]
    ok = True
    for i in infos:
        free = i["slot"] - i["app"]
        state = budget_state(free)
        ok = ok and state != "over"
        old = base_size(base, i["artifact"])
        change = "n/a" if old is None else "%+.1f KB" % ((i["app"] - old) / 1024) if old != i["app"] else "0"
        lines.append("| %s | %s | %s | %s (%d%%) | %s | %s |" % (i["id"], kb(i["app"]), kb(i["slot"]), kb(free),
                                                               100 * free // i["slot"], change, icon[state]))
    if base:
        lines += ["", "Change: against %s." % os.environ.get("BUDGET_BASE_NAME", base)]
    return "\n".join(lines) + "\n", ok


def sizes(args):
    """--sizes DIR [--base DIR]: the budget table for the images a build left in DIR"""
    base = None
    if "--base" in args:
        i = args.index("--base")
        base = args[i + 1] if i + 1 < len(args) else sys.exit("--base needs a directory")
        args = args[:i] + args[i + 2:]
        if not os.path.isdir(base):
            print("note: no %s to compare with" % base)
            base = None
    if len(args) != 1:
        sys.exit("usage: build.py --sizes DIR [--base DIR]")
    infos = []
    for path in sorted(glob.glob(os.path.join(args[0], "*.size.json"))):
        with open(path, encoding="utf-8") as f:
            infos.append(json.load(f))
    if not infos:
        sys.exit("no *.size.json in %s: build first" % args[0])
    infos.sort(key=lambda i: ("/" in i["id"], i["id"]))    # the AP boards, then the radios
    for i in infos:
        check_budget(i)
    table, ok = size_report(infos, base)
    print(table)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(table)
    if not ok:
        sys.exit("an image keeps less than %s free in its slot (the table above)" % kb_limit(BUDGET_FAIL))


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
    if args[:1] == ["--sizes"]:
        return sizes(args[1:])
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
    infos = [build(b) for b in chosen]
    if not no_radio:
        chosen_radios += [r for r in load_radios(chosen) if r not in chosen_radios]
    infos += [build_radio(r) for r in chosen_radios]
    # the images are in out/ either way, so CI can still show the table; then the budget
    if not all([check_budget(i) for i in infos]):
        sys.exit("over the flash budget: an image keeps less than %s free in its slot" % kb_limit(BUDGET_FAIL))


if __name__ == "__main__":
    main()
