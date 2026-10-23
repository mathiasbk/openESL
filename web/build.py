#!/usr/bin/env python3
"""
Builds the web page (web/) into web/dist/, ready for GitHub Pages or any static web server.

  python web/build.py              renderer (WebAssembly) + page into web/dist/
  python web/build.py --native     only the native test build of web_api.c (no Emscripten),
                                   used by web/test/render.test.mjs
  python web/build.py --firmware <file>...
                                   copies firmware into web/dist/firmware/ with the AP's
                                   manifests for esp-web-tools and info.json (used by the
                                   GitHub workflow). Each file is told apart by its header:
                                   a tag image (tag-<build>.bin for the flasher, named after
                                   the model in the OTA header of its -ota.bin) or an AP image
                                   (the board in ap/boards/ is the one whose "artifact" or
                                   "release" name the file starts with; the page offers a choice
                                   of board). "-" means no file.
                                   FIRMWARE_RELEASE=v1.2.3 marks them as that release. A
                                   <file>-ota.bin next to each one is copied too, for the
                                   dashboard's updates through the API

The renderer is web/wasm/web_api.c with the AP's layout compiler and drawing (esl_render.c,
the image the AP sends to a tag), built with Emscripten (emcc) once per tag model in
shared/models.json:

  dist/wasm/render-29.js/.wasm     2.9"  (296 x 128)
  dist/wasm/render-154.js/.wasm    1.54" (200 x 200)
  dist/wasm/render-<build>.js/.wasm for any other model

Firmware for the flasher is not built here. The GitHub workflow (.github/workflows/web.yml)
adds it with --firmware: the files of the latest GitHub release, or, before the first release
and on pull requests, the firmware built from the same commit. The AP image is the merged
image (bootloader, partition table and app in one file); the manifest flashes its parts one by
one so the AP's stored settings survive (split_ap_image).
"""
import datetime
import glob
import importlib.util
import json
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# tag/tools/build.py has the same file name as this script, so load it under another name
_spec = importlib.util.spec_from_file_location("tag_build", os.path.join(ROOT, "tag", "tools", "build.py"))
tag_build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tag_build)

DIST = os.path.join(HERE, "dist")
AP_CORE = os.path.join(ROOT, "ap", "components", "esl_core")
AP_SOURCES = ["esl_layout_compile.c", "esl_json.c", "esl_crc32.c", "esl_proto.c", "esl_content.c", "esl_text.c",
              "esl_render.c", "esl_qr.c", "esl_barcode.c"]
TAG_SOURCES = ["src/font.c", "src/barcode.c"] + tag_build.FONTS     # what esl_render.c draws with
MODELS = [m["build"] for m in tag_build.models.load()]     # -DMODEL=<build>
STATIC = ["index.html", "api.html", "css", "js", "img"]

EXPORTS = ["web_width", "web_height", "web_model_code", "web_error", "web_fb", "web_max_text",
           "web_layout_bin", "web_layout_size", "web_layout_version", "web_load_layouts",
           "web_clear_layouts", "web_text_bytes", "web_render", "web_limit", "malloc", "free"]


def sources():
    return ([os.path.join(tag_build.TAG, s) for s in TAG_SOURCES] +
            [os.path.join(AP_CORE, s) for s in AP_SOURCES] +
            [os.path.join(HERE, "wasm", "web_api.c")])


def includes(native=False):
    dirs = tag_build.INCLUDES + [os.path.join(AP_CORE, "include")]
    if native:
        dirs.append(os.path.join(HERE, "wasm", "native"))     # stands in for emscripten.h
    return ["-I" + i for i in dirs]


def build_wasm(model):
    emcc = os.environ.get("EMCC", "emcc")
    if not shutil.which(emcc):
        sys.exit("emcc not found. Install Emscripten (https://emscripten.org) and source emsdk_env.sh.")
    out = os.path.join(DIST, "wasm")
    os.makedirs(out, exist_ok=True)
    js = os.path.join(out, "render-%s.js" % model)
    tag_build.run([emcc, "-std=c11", "-Wall", "-O2", "-DMODEL=%s" % model] + includes() + sources() + [
        "-o", js,
        "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sENVIRONMENT=web,node",
        "-sEXPORT_NAME=createRenderer", "-sALLOW_MEMORY_GROWTH=1", "-sFILESYSTEM=0",
        "-sEXPORTED_FUNCTIONS=" + ",".join("_" + e for e in EXPORTS),
        "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8",
    ])


def build_native(model):
    """web_api.c as a native program with a small main (web/wasm/native/), for the tests."""
    cc = tag_build.find_cc()
    out = os.path.join(tag_build.BUILD, "web-%s" % model)
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, "web_render" + (".exe" if os.name == "nt" else ""))
    tag_build.run(cc + ["-std=c11", "-Wall", "-Wextra", "-O1", "-DMODEL=%s" % model] + includes(True) +
                  sources() + [os.path.join(HERE, "wasm", "native", "main.c"), "-o", exe])
    return exe


def copy_static():
    for name in STATIC:
        src = os.path.join(HERE, name)
        dst = os.path.join(DIST, name)
        if os.path.isdir(src):
            shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)
    # the API reference (api.html) reads the spec next to it
    shutil.copy2(os.path.join(ROOT, "docs", "openapi.yaml"), DIST)
    # the example layout files, so the editor can open them
    lay = os.path.join(DIST, "designs", "layouts")
    os.makedirs(lay, exist_ok=True)
    for m in (m["name"] for m in tag_build.models.load()):
        shutil.copy2(os.path.join(tag_build.TAG, "designs", "layouts", m + ".json"), lay)
    os.makedirs(os.path.join(DIST, "firmware"), exist_ok=True)


# The merged AP image is written at offset 0 and is 0xFF everywhere in between, so writing it
# whole also wipes the NVS partition (WiFi, API token, tags, layout files). The manifest instead
# lists the parts of the image that hold something: bootloader, partition table, otadata (blank,
# so the board starts the new app in ota_0) and the app. NVS, phy_init, ota_1 and tagfw are left
# alone, so flashing without "Erase device" in esp-web-tools keeps everything the AP has stored.
# This relies on the board already having the same partition table; an AP with an older layout
# must be flashed with "Erase device" (the page says so).
PT_OFFSET = 0x8000
PT_SIZE = 0x1000


def partitions(img):
    """(label, type, subtype, offset, size) of each entry in the image's partition table."""
    table = img[PT_OFFSET:PT_OFFSET + PT_SIZE]
    out = []
    for i in range(0, len(table), 32):
        e = table[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        ptype, sub, off, size = struct.unpack("<BBII", e[2:12])
        out.append((e[12:28].rstrip(b"\0").decode(), ptype, sub, off, size))
    return out


def split_ap_image(src, out, name):
    """Writes the parts of the merged image src that are flashed: [(file name, offset)]."""
    img = open(src, "rb").read()
    parts = [(0, img[:PT_OFFSET]), (PT_OFFSET, img[PT_OFFSET:PT_OFFSET + PT_SIZE])]
    table = partitions(img)
    if not table:
        sys.exit(src + ": no partition table at 0x8000")
    for label, ptype, sub, off, size in table:
        if ptype == 1 and sub == 0:            # otadata: written blank, whole
            parts.append((off, b"\xff" * size))
        elif ptype == 0 and sub in (0x00, 0x10):  # factory or ota_0: the app of the merged image
            parts.append((off, img[off:off + size]))
    if not any(off >= 0x10000 and data for off, data in parts):
        sys.exit(src + ": no app in the merged image")
    files = []
    for off, data in parts:
        data += b"\xff" * (-len(data) % 4)
        fname = "%s-0x%x.bin" % (name[:-4], off)
        with open(os.path.join(out, fname), "wb") as f:
            f.write(data)
        files.append((fname, off))
    return files


def ap_manifest(out, name, version, path, parts, chip_family):
    manifest = {
        "name": name,
        "version": version,
        # esp-web-tools asks "Erase device?" before installing (not ticked by default); without
        # it only the parts below are written and the AP keeps its settings and tags
        "new_install_prompt_erase": True,
        # the version in the address keeps a browser from flashing an image cached from an older release
        "builds": [{"chipFamily": chip_family,
                    "parts": [{"path": f + "?v=" + version, "offset": off} for f, off in parts]}],
    }
    with open(os.path.join(out, path.replace("ap-openesl", "ap").replace(".bin", "-manifest.json")), "w") as f:
        json.dump(manifest, f, indent=2)


def image_version(path):
    """The version inside an update image: the OTA header of a tag image (u16, as the tag sends
    it in POLL) or esp_app_desc_t of an AP app image (the version it was built with). None when
    the file is not there or not one of those."""
    try:
        with open(path, "rb") as f:
            data = f.read(80)
    except OSError:
        return None
    if data[:4] == b"OSFW" and len(data) >= 16:
        v = struct.unpack_from("<H", data, 6)[0]
        return "%d.%d.%d" % (v // 10000, v // 100 % 100, v % 100)
    if len(data) >= 80 and data[0] == 0xE9 and struct.unpack_from("<I", data, 32)[0] == 0xABCD5432:
        return data[48:80].split(b"\0", 1)[0].decode() or None
    return None


def tag_image_model(path):
    """The model (from shared/models.json) a tag firmware image is for, read from the model code in
    the OTA header of the file or of the <file>-ota.bin next to it (the flasher's image has the
    bootloader first and no header), or None when neither is a tag image."""
    for p in (path, path[:-4] + "-ota.bin"):
        try:
            with open(p, "rb") as f:
                data = f.read(16)
        except OSError:
            continue
        if data[:4] != b"OSFW" or len(data) < 16:
            continue
        for m in tag_build.models.load():
            if m["code"] == data[5]:
                return m
        sys.exit("%s is a tag firmware image for an unknown model code %d" % (p, data[5]))
    return None


def ap_image(path):
    """The chip id (esp_image_header_t chip_id: 13 for the ESP32-C6, 23 for the C5) when the
    file, or the <file>-ota.bin next to it, is an ESP32 app image, else None (the merged flasher
    image starts with the bootloader, so the -ota.bin is the one with the app header)"""
    for p in (path, path[:-4] + "-ota.bin"):
        try:
            with open(p, "rb") as f:
                data = f.read(80)
        except OSError:
            continue
        if len(data) >= 80 and data[0] == 0xE9 and struct.unpack_from("<I", data, 32)[0] == 0xABCD5432:
            return struct.unpack_from("<H", data, 12)[0]
    return None


CHIP_IDS = {"esp32c6": 13, "esp32c5": 23, "esp32s3": 9}


def ap_boards():
    """The AP boards in ap/boards/ (board.json, with "display" from its sdkconfig.defaults or its
    base's), supported ones first"""
    bdir = os.path.join(ROOT, "ap", "boards")
    boards = {}
    for name in sorted(os.listdir(bdir)):
        path = os.path.join(bdir, name, "board.json")
        if os.path.exists(path):
            with open(path) as f:
                boards[name] = json.load(f)

    def sdkconfig(b):
        path = os.path.join(bdir, b["id"], "sdkconfig.defaults")
        text = open(path).read() if os.path.exists(path) else ""
        return (sdkconfig(boards[b["base"]]) if b.get("base") in boards else "") + text

    for b in boards.values():
        b["display"] = "\nCONFIG_ESL_DISPLAY=y" in "\n" + sdkconfig(b)
    return sorted(boards.values(), key=lambda b: (b["status"] != "supported", b.get("base") is not None, b["id"]))


def ap_radio_names():
    """artifact and release names of the radio coprocessor firmware (ap/radio/boards/*/radio.json,
    for boards with "radio": "uart"): the flasher does not offer it yet (docs/ap-radio-link.md)"""
    names = []
    for path in glob.glob(os.path.join(ROOT, "ap", "radio", "boards", "*", "radio.json")):
        with open(path) as f:
            r = json.load(f)
        names += [r["artifact"], r["release"]]
    return names


def ap_board_for(path, boards):
    """The board an AP image is for: the one whose artifact (CI: openesl-ap-lcd.bin) or release
    name (openesl-ap-esp32c6-lcd-v1.2.3.bin) the file name starts with, the longest match, and
    otherwise the first board for the chip in the image. Stops when the chip does not fit."""
    stem = os.path.basename(path)[:-4].lower()
    chip = ap_image(path)
    best, n = None, 0
    for b in boards:
        for name in (b["artifact"], b["release"]):
            if (stem == name or stem.startswith(name + "-")) and len(name) > n:
                best, n = b, len(name)
    if best is None:
        fits = [b for b in boards if CHIP_IDS.get(b["target"]) == chip]
        best = next((b for b in fits if b["display"] == ("lcd" in stem)), fits[0] if fits else None)
        if best is None:
            sys.exit("%s: an AP image for chip id %s, which no board in ap/boards/ has" % (path, chip))
    if CHIP_IDS.get(best["target"]) not in (None, chip):
        sys.exit("%s: the name says board %s (%s), the image is for chip id %s" % (path, best["id"], best["target"], chip))
    return best


def ap_file_name(board):
    """The name of a board's image in web/dist/firmware/: ap-openesl.bin for the C6 Super Mini,
    ap-openesl-lcd.bin, ap-openesl-c5-n16r8.bin (from "artifact" in board.json)"""
    return "ap-openesl" + board["artifact"][len("openesl-ap"):] + ".bin"


def add_firmware(*files):
    out = os.path.join(DIST, "firmware")
    os.makedirs(out, exist_ok=True)
    # FIRMWARE_RELEASE is the release tag (v1.2.3) when the files come from a GitHub release;
    # otherwise they were built from this checkout and get the VERSION file's version + "-dev"
    release = os.environ.get("FIRMWARE_RELEASE", "")
    # Sorted by what they are, whatever their order: the tag images by their model, the AP
    # images by their app header and name. A file that is neither stops the build, so a wrong
    # call in CI is not published; a release may lack a model that was added after it
    tags = {}      # build -> path
    boards = ap_boards()
    aps = {}       # board id -> path
    for f in files:
        if f == "-":
            continue
        if f.endswith("-ota.bin"):
            sys.exit("%s: give the flasher image; its -ota.bin is picked up next to it" % f)
        if not os.path.exists(f):
            if release:
                print("%s: not in release %s, skipped" % (f, release))
                continue
            sys.exit("%s: no such file" % f)
        stem = os.path.basename(f)[:-4].lower()
        if any(stem == n or stem.startswith(n + "-") for n in ap_radio_names()):
            print("%s: radio coprocessor firmware, not offered by the flasher yet" % f)
            continue
        m = tag_image_model(f)
        if m:
            tags[m["build"]] = f
        elif ap_image(f) is not None:
            aps[ap_board_for(f, boards)["id"]] = f
        else:
            sys.exit("%s: neither a tag firmware image (OTA header in it or in its -ota.bin) nor an AP image "
                     "(app header in its -ota.bin)" % f)
    info = {
        "version": release or open(os.path.join(ROOT, "VERSION")).read().strip() + "-dev",
        "release": bool(release),
        "ref": os.environ.get("GITHUB_REF_NAME", "local"),
        "commit": os.environ.get("GITHUB_SHA", "0000000"),
        "date": datetime.date.today().isoformat(),
        "tag": bool(tags),
        # ap and ap_lcd for pages and AP dashboards from before ap_boards
        "ap": "c6-supermini" in aps,
        "ap_lcd": "c6-supermini-lcd" in aps,
        # The AP and the tags each keep the version of the release they last changed in
        # (tools/fw_version.py), read from the -ota.bin images: the dashboard compares these
        # with what the AP and the tags run, so a release without changes for them offers
        # them nothing. tag_version is the first model's; tag_versions has every model's
        # (the page names them when they differ)
        "ap_version": next((image_version(f[:-4] + "-ota.bin") for f in aps.values()), None),
        "tag_version": None,
        "tag_versions": {},
    }
    for build, f in tags.items():
        v = image_version(f[:-4] + "-ota.bin")
        info["tag_versions"][build] = v
        info["tag_version"] = info["tag_version"] or v
    info["ota"] = {}
    for src, name in ([(f, "tag-%s.bin" % b) for b, f in tags.items()] +
                      [(aps[b["id"]], ap_file_name(b)) for b in boards if b["id"] in aps]):
        shutil.copy2(src, os.path.join(out, name))
        # the file for an update through the API lies next to it as <name>-ota.bin
        # (release and CI names alike); the dashboard sends it
        ota = src[:-4] + "-ota.bin"
        if os.path.exists(ota):
            shutil.copy2(ota, os.path.join(out, name[:-4] + "-ota.bin"))
            info["ota"][name[:-4]] = True
    # One manifest per board (ap-manifest.json, ap-lcd-manifest.json, ap-c5-n16r8-manifest.json),
    # and the boards in the order the page offers them
    info["ap_boards"] = []
    for b in boards:
        if b["id"] not in aps:
            continue
        name = ap_file_name(b)
        manifest = name.replace("ap-openesl", "ap").replace(".bin", "-manifest.json")
        title = "OpenESL AP, %s%s" % (b["chip_family"], " with status screen" if b["display"] else "")
        ap_manifest(out, title, info["version"], name, split_ap_image(aps[b["id"]], out, name),
                    b["chip_family"])
        # name is what the page's list shows; label (board.json) goes under it
        short = b["chip_family"] + (" with status screen" if b["display"] else "") + (" with PSRAM" if b.get("psram_mb") else "") + (" + C6 radio" if b.get("radio") == "uart" else "")
        info["ap_boards"].append({"id": b["id"], "name": short, "label": b["label"], "status": b["status"], "target": b["target"],
                                  "display": b["display"], "file": name[:-4], "manifest": manifest})
    with open(os.path.join(out, "info.json"), "w") as f:
        json.dump(info, f, indent=2)
    print("Firmware in", out)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--firmware":
        add_firmware(*sys.argv[2:])
        return
    if sys.argv[1:] == ["--native"]:
        for m in MODELS:
            print(build_native(m))
        return
    if sys.argv[1:]:
        sys.exit(__doc__)
    os.makedirs(DIST, exist_ok=True)
    for m in MODELS:
        build_wasm(m)
    copy_static()
    print("Page in", DIST)


if __name__ == "__main__":
    main()
