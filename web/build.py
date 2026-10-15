#!/usr/bin/env python3
"""
Builds the web page (web/) into web/dist/, ready for GitHub Pages or any static web server.

  python web/build.py              renderer (WebAssembly) + page into web/dist/
  python web/build.py --native     only the native test build of web_api.c (no Emscripten),
                                   used by web/test/render.test.mjs
  python web/build.py --firmware <tag-29.bin> <tag-154.bin> <ap.bin>
                                   copies firmware into web/dist/firmware/ with the AP's
                                   manifest for esp-web-tools and info.json (used by the
                                   GitHub workflow; any of the three may be "-" for none)

The renderer is web/wasm/web_api.c with the tag's drawing code and the AP's layout
compiler, built with Emscripten (emcc) once per tag model:

  dist/wasm/render-29.js/.wasm     2.9"  (296 x 128)
  dist/wasm/render-154.js/.wasm    1.54" (200 x 200)

Firmware for the flasher is not built here. The GitHub workflow (.github/workflows/web.yml)
builds it and adds it with --firmware; the AP image is the merged image (bootloader, partition
table and app in one file, written at offset 0).
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
AP_SOURCES = ["esl_layout_compile.c", "esl_json.c", "esl_crc32.c", "esl_proto.c", "esl_content.c"]
TAG_SOURCES = tag_build.CORE + tag_build.FONTS + ["test/fake_epd.c", "test/helpers.c"]
MODELS = ["29", "154"]
STATIC = ["index.html", "css", "js"]

EXPORTS = ["web_width", "web_height", "web_model_code", "web_error", "web_fb", "web_max_text",
           "web_layout_bin", "web_layout_size", "web_layout_version", "web_load_layouts",
           "web_clear_layouts", "web_text_bytes", "web_render", "malloc", "free"]


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
    # the example layout files, so the editor can open them
    lay = os.path.join(DIST, "designs", "layouts")
    os.makedirs(lay, exist_ok=True)
    for m in ("2.9", "1.54"):
        shutil.copy2(os.path.join(tag_build.TAG, "designs", "layouts", m + ".json"), lay)
    os.makedirs(os.path.join(DIST, "firmware"), exist_ok=True)


def add_firmware(tag29, tag154, ap):
    out = os.path.join(DIST, "firmware")
    os.makedirs(out, exist_ok=True)
    info = {
        "ref": os.environ.get("GITHUB_REF_NAME", "local"),
        "commit": os.environ.get("GITHUB_SHA", "0000000"),
        "date": datetime.date.today().isoformat(),
        "tag": tag29 != "-" or tag154 != "-",
        "ap": ap != "-",
    }
    for src, name in ((tag29, "tag-29.bin"), (tag154, "tag-154.bin"), (ap, "ap-openesl.bin")):
        if src != "-":
            shutil.copy2(src, os.path.join(out, name))
    if ap != "-":
        manifest = {
            "name": "OpenESL AP",
            "version": info["commit"][:7],
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": "ESP32-C6", "parts": [{"path": "ap-openesl.bin", "offset": 0}]}],
        }
        with open(os.path.join(out, "ap-manifest.json"), "w") as f:
            json.dump(manifest, f, indent=2)
    with open(os.path.join(out, "info.json"), "w") as f:
        json.dump(info, f, indent=2)
    print("Firmware in", out)


def main():
    if len(sys.argv) == 5 and sys.argv[1] == "--firmware":
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
