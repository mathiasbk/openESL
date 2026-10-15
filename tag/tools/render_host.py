#!/usr/bin/env python3
"""
Host checks of drawing and layout for every tag model, without hardware.

  python tools/render_host.py            tests + previews for all models
  python tools/render_host.py 154        only one model (29 or 154)

For each model (src/model.h) it compiles the host tests and the preview tool with a
normal C compiler and -DMODEL=<model>, runs the tests (including test_epd, which rebuilds
the image from a model of the display controller's RAM), and draws:

  build/preview/<model>/<example>.png            the example set in test/preview.c
  build/preview/<model>/layout-<name>.png        every layout in designs/layouts/<model>.json,
                                                 with the field names as text
  build/preview/<model>/design-<id>-<n>.png      every design in designs/*.json, filled with
                                                 the data in designs/examples/<id>.json
  build/preview/<model>/epd_path.png             the image as epd.c leaves it in controller RAM
  build/preview/<model>/all.png                  all of the above on one sheet

Layout files (designs/layouts/2.9.json and 1.54.json) are compiled with the AP's own
compiler (ap/components/esl_core/esl_layout_compile.c, built here as layout_compile) into
build/preview/<model>/layouts.bin, the file a tag of that model downloads. The preview
tool installs it with the tag's own check (layout_file_install) and draws content for
layout 16 + n, exactly as a tag does after the download (docs/protokoll.md part 1).

The images are scaled 2x. Without Pillow the PPM files are kept instead.
Uses the compiler and source lists from tools/build.py, which builds the default model only.
"""
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import build  # noqa: E402
import label_design  # noqa: E402

MODELS = ["29", "154"]
MODEL_NAME = {"29": "2.9", "154": "1.54"}        # as in the API and the layout files
MODEL_CODE = {"29": 1, "154": 2}                 # ESL_MODEL_* in shared/proto.h
LAYOUT_IDS = {"price": 1, "text": 2}
LAYOUT_FILE = 0x10                               # ESL_LAYOUT_FILE: layout n of the file is 0x10 + n
FLAG_BITS = {"price_highlight": 0x01, "frame": 0x02}
AP_CORE = os.path.join(build.ROOT, "ap", "components", "esl_core")
LAYOUT_COMPILER = ["esl_layout_compile.c", "esl_json.c", "esl_crc32.c", "esl_proto.c", "esl_content.c"]


def compile_host(name, model):
    cc = build.find_cc()
    out_dir = os.path.join(build.BUILD, "host-%s" % model)
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, name + (".exe" if os.name == "nt" else ""))
    flags = ["-std=c99", "-Wall", "-Wextra", "-O1", "-g", "-DMODEL=%s" % model] + build.version_defines()
    srcs = [os.path.join(build.TAG, s) for s in build.HOST_TARGETS[name]]
    build.run(cc + flags + ["-I" + i for i in build.INCLUDES] + srcs + ["-o", exe])
    return exe


def compile_layout_tool():
    """The AP's layout compiler as a host program (one for all models)."""
    cc = build.find_cc()
    out_dir = os.path.join(build.BUILD, "host")
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, "layout_compile" + (".exe" if os.name == "nt" else ""))
    srcs = [os.path.join(AP_CORE, s) for s in LAYOUT_COMPILER] + [os.path.join(build.TAG, "test", "layout_compile.c")]
    build.run(cc + ["-std=c11", "-Wall", "-Wextra", "-O1", "-I" + os.path.join(AP_CORE, "include"),
                    "-I" + os.path.join(build.ROOT, "shared")] + srcs + ["-o", exe])
    return exe


def layout_file(model, out):
    """
    Compiles designs/layouts/<model>.json for this model. Returns (parsed file, path of the
    binary) or (None, None) if there is none; raises SystemExit if the AP would reject it.
    """
    path = os.path.join(build.TAG, "designs", "layouts", MODEL_NAME[model] + ".json")
    if not os.path.exists(path):
        return None, None
    binary = os.path.join(out, "layouts.bin")
    build.run([compile_layout_tool(), str(MODEL_CODE[model]), path, binary])
    return label_design.load(path), binary


def preview_cmd(exe, binary, ppm, layout, flags, fields, lfile):
    """The preview command line for one image; file layouts load the downloaded file first."""
    if layout in LAYOUT_IDS:
        return [exe, "-o", ppm, str(LAYOUT_IDS[layout]), str(flags)] + fields
    names = [lay["name"] for lay in lfile["layouts"]]
    return [exe, "-L", binary, "-o", ppm, str(LAYOUT_FILE + names.index(layout)), str(flags)] + fields


def layout_images(exe, out, lfile, binary):
    """Every layout of the file, with each field showing its own name. Returns the errors."""
    errors = 0
    for lay in lfile["layouts"]:
        ppm = os.path.join(out, "layout-%s.ppm" % lay["name"])
        flags = FLAG_BITS["price_highlight"] | FLAG_BITS["frame"]
        r = subprocess.run(preview_cmd(exe, binary, ppm, lay["name"], flags, list(lay["fields"]), lfile))
        errors += r.returncode != 0
    return errors


def design_images(exe, out, lfile, binary):
    """Renders each design with its example data. Returns the number of errors."""
    errors = 0
    for path in sorted(glob.glob(os.path.join(build.TAG, "designs", "*.json"))):
        design = label_design.load(path)
        ex_path = os.path.join(build.TAG, "designs", "examples", design["id"] + ".json")
        if not os.path.exists(ex_path):
            print("no example data for", design["id"], "(designs/examples/%s.json)" % design["id"])
            continue
        if label_design.layout_fields(design.get("layout"), lfile) is None:
            print("%s: layout %r is not in this model's layout file, skipped" % (design["id"], design.get("layout")))
            continue
        for n, data in enumerate(label_design.load(ex_path), 1):
            try:
                c = label_design.render(design, data, lfile)
            except label_design.DesignError as e:
                print("ERROR %s example %d: %s" % (design["id"], n, e))
                errors += 1
                continue
            flags = sum(bit for key, bit in FLAG_BITS.items() if c.get(key))
            ppm = os.path.join(out, "design-%s-%d.ppm" % (design["id"], n))
            r = subprocess.run(preview_cmd(exe, binary, ppm, c["layout"], flags, c["fields"], lfile))
            errors += r.returncode != 0
    return errors


def to_png(out):
    try:
        from PIL import Image
    except ImportError:
        print("Pillow is missing: the PPM files are in", out)
        return
    imgs = []
    for ppm in sorted(glob.glob(os.path.join(out, "*.ppm"))):
        img = Image.open(ppm).convert("RGB")
        img = img.resize((img.width * 2, img.height * 2), Image.NEAREST)
        img.save(ppm[:-4] + ".png")
        os.remove(ppm)
        imgs.append(img)
    if not imgs:
        return
    # Contact sheet: grey gaps between the screens so the edges are visible
    cols = 3 if imgs[0].width < 500 else 2
    gap = 12
    w, h = imgs[0].width, imgs[0].height
    rows = (len(imgs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (w + gap) + gap, rows * (h + gap) + gap), (150, 150, 150))
    for i, img in enumerate(imgs):
        sheet.paste(img, (gap + (i % cols) * (w + gap), gap + (i // cols) * (h + gap)))
    sheet.save(os.path.join(out, "all.png"))


def one_model(model):
    print("==== MODEL=%s ====" % model)
    build.run([compile_host("test_all", model)])
    out = os.path.join(build.BUILD, "preview", model)
    os.makedirs(out, exist_ok=True)
    for old in glob.glob(os.path.join(out, "*.png")) + glob.glob(os.path.join(out, "*.ppm")):
        os.remove(old)
    build.run([compile_host("test_epd", model), os.path.join(out, "epd_path.ppm")])
    exe = compile_host("preview", model)
    build.run([exe, out])
    lfile, binary = layout_file(model, out)
    errors = 0
    if lfile:
        errors += layout_images(exe, out, lfile, binary)
    errors += design_images(exe, out, lfile, binary)
    to_png(out)
    print("Images in", out)
    return errors


def main(argv):
    models = argv[1:] or MODELS
    bad = [m for m in models if m not in MODELS]
    if bad:
        sys.exit(__doc__)
    errors = sum(one_model(m) for m in models)
    if errors:
        sys.exit("%d design example(s) failed" % errors)


if __name__ == "__main__":
    main(sys.argv)
