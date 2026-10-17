#!/usr/bin/env python3
"""
Builds the web page (web/) into web/dist/, ready for GitHub Pages or any static web server.

  python web/build.py              renderer (WebAssembly) + page into web/dist/
  python web/build.py --native     only the native test build of web_api.c (no Emscripten),
                                   used by web/test/render.test.mjs
  python web/build.py --firmware <tag-29.bin> <tag-154.bin> <ap.bin> [<ap-lcd.bin>]
                                   copies firmware into web/dist/firmware/ with the AP's
                                   manifests for esp-web-tools and info.json (used by the
                                   GitHub workflow; any of them may be "-" for none;
                                   ap-lcd.bin is the AP with the status screen, offered
                                   by a checkbox on the page; FIRMWARE_RELEASE=v1.2.3
                                   marks them as that release). A <file>-ota.bin next to
                                   each one is copied too, for the dashboard's updates
                                   through the API

The renderer is web/wasm/web_api.c with the tag's drawing code and the AP's layout
compiler, built with Emscripten (emcc) once per tag model:

  dist/wasm/render-29.js/.wasm     2.9"  (296 x 128)
  dist/wasm/render-154.js/.wasm    1.54" (200 x 200)

Firmware for the flasher is not built here. The GitHub workflow (.github/workflows/web.yml)
adds it with --firmware: the files of the latest GitHub release, or, before the first release
and on pull requests, the firmware built from the same commit. The AP image is the merged
image (bootloader, partition table and app in one file, written at offset 0).
"""
import datetime
import importlib.util
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# tag/tools/build.py has the same file name as this script, so load it under another name
_spec = importlib.util.spec_from_file_location("tag_build", os.path.join(ROOT, "tag", "tools", "build.py"))
tag_build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tag_build)

DIST = os.path.join(HERE, "dist")
AP_CORE = os.path.join(ROOT, "ap", "components", "esl_core")
AP_SOURCES = ["esl_layout_compile.c", "esl_json.c", "esl_crc32.c", "esl_proto.c", "esl_content.c", "esl_text.c"]
TAG_SOURCES = tag_build.CORE + tag_build.FONTS + ["test/fake_epd.c", "test/helpers.c"]
MODELS = ["29", "154"]
STATIC = ["index.html", "api.html", "css", "js"]

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
    for m in ("2.9", "1.54"):
        shutil.copy2(os.path.join(tag_build.TAG, "designs", "layouts", m + ".json"), lay)
    os.makedirs(os.path.join(DIST, "firmware"), exist_ok=True)


def ap_manifest(out, name, version, path):
    manifest = {
        "name": name,
        "version": version,
        "new_install_prompt_erase": True,
        # the version in the address keeps a browser from flashing an image cached from an older release
        "builds": [{"chipFamily": "ESP32-C6", "parts": [{"path": path + "?v=" + version, "offset": 0}]}],
    }
    with open(os.path.join(out, path.replace("ap-openesl", "ap").replace(".bin", "-manifest.json")), "w") as f:
        json.dump(manifest, f, indent=2)


def add_firmware(tag29, tag154, ap, ap_lcd="-"):
    out = os.path.join(DIST, "firmware")
    os.makedirs(out, exist_ok=True)
    # FIRMWARE_RELEASE is the release tag (v1.2.3) when the files come from a GitHub release;
    # otherwise they were built from this checkout and get the VERSION file's version + "-dev"
    release = os.environ.get("FIRMWARE_RELEASE", "")
    info = {
        "version": release or tag_build.read_version()[0] + "-dev",
        "release": bool(release),
        "ref": os.environ.get("GITHUB_REF_NAME", "local"),
        "commit": os.environ.get("GITHUB_SHA", "0000000"),
        "date": datetime.date.today().isoformat(),
        "tag": tag29 != "-" or tag154 != "-",
        "ap": ap != "-",
        "ap_lcd": ap_lcd != "-" and os.path.exists(ap_lcd),
    }
    info["ota"] = {}
    for src, name in ((tag29, "tag-29.bin"), (tag154, "tag-154.bin"), (ap, "ap-openesl.bin"),
                      (ap_lcd if info["ap_lcd"] else "-", "ap-openesl-lcd.bin")):
        if src != "-":
            shutil.copy2(src, os.path.join(out, name))
            # the file for an update through the API lies next to it as <name>-ota.bin
            # (release and CI names alike); the dashboard sends it
            ota = src[:-4] + "-ota.bin"
            if os.path.exists(ota):
                shutil.copy2(ota, os.path.join(out, name[:-4] + "-ota.bin"))
                info["ota"][name[:-4]] = True
    # ap-manifest.json and ap-lcd-manifest.json
    if ap != "-":
        ap_manifest(out, "OpenESL AP", info["version"], "ap-openesl.bin")
    if info["ap_lcd"]:
        ap_manifest(out, "OpenESL AP with status screen", info["version"], "ap-openesl-lcd.bin")
    with open(os.path.join(out, "info.json"), "w") as f:
        json.dump(info, f, indent=2)
    print("Firmware in", out)


def main():
    if len(sys.argv) in (5, 6) and sys.argv[1] == "--firmware":
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
