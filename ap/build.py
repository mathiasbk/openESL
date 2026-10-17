#!/usr/bin/env python3
"""
Builds the AP firmware for one or more boards (boards/<board>/board.json) with ESP-IDF.

  python build.py                    every board
  python build.py c6-supermini ...   only these boards
  python build.py --list             the boards: id, chip, status, file name
  python build.py --release          "<artifact> <release name>" per board (release.yml)

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


def build(b):
    bdir = "build-" + b["id"]
    run(["idf.py", "-B", bdir, "-D", "SDKCONFIG=%s/sdkconfig" % bdir, "-D", "BOARD=" + b["id"], "build"])
    os.makedirs(OUT, exist_ok=True)
    run([sys.executable, "-m", "esptool", "--chip", b["target"], "merge_bin", "-o",
         os.path.join(OUT, b["artifact"] + ".bin"), "@flash_args"], cwd=os.path.join(HERE, bdir))
    shutil.copyfile(os.path.join(HERE, bdir, "esl_ap.bin"), os.path.join(OUT, b["artifact"] + "-ota.bin"))
    print("%s: out/%s.bin and out/%s-ota.bin" % (b["id"], b["artifact"], b["artifact"]))


def main():
    boards = load_boards()
    args = sys.argv[1:]
    if args == ["--list"]:
        for b in boards:
            print("%-20s %-8s %-12s %s" % (b["id"], b["target"], b["status"], b["artifact"]))
        return
    if args == ["--release"]:
        for b in boards:
            print(b["artifact"], b["release"])
        return
    by_id = {b["id"]: b for b in boards}
    unknown = [a for a in args if a not in by_id]
    if unknown:
        sys.exit("unknown board %s; the boards are %s" % (", ".join(unknown), ", ".join(by_id)))
    if not shutil.which("idf.py"):
        sys.exit("idf.py not found: source ESP-IDF v5.5.5's export.sh first")
    for b in [by_id[a] for a in args] or boards:
        build(b)


if __name__ == "__main__":
    main()
