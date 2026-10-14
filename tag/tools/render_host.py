#!/usr/bin/env python3
"""
Host checks of drawing and layout for every tag model, without hardware.

  python tools/render_host.py            tests + previews for all models
  python tools/render_host.py 154        only one model (29 or 154)

For each model (src/model.h) it compiles the host tests and the preview tool with a
normal C compiler and -DMODEL=<model>, runs the tests (including test_epd, which rebuilds
the image from a model of the display controller's RAM), and draws:

  build/preview/<model>/<example>.png            the example set in test/preview.c
  build/preview/<model>/design-<id>-<n>.png      every design in designs/*.json, filled with
                                                 the data in designs/examples/<id>.json
  build/preview/<model>/epd_path.png             the image as epd.c leaves it in controller RAM
  build/preview/<model>/all.png                  all of the above on one sheet

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
LAYOUT_IDS = {"price": 1, "text": 2}
FLAG_BITS = {"price_red": 0x01, "frame_red": 0x02}


def compile_host(name, model):
    cc = build.find_cc()
    out_dir = os.path.join(build.BUILD, "host-%s" % model)
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, name + (".exe" if os.name == "nt" else ""))
    flags = ["-std=c99", "-Wall", "-Wextra", "-O1", "-g", "-DMODEL=%s" % model]
    srcs = [os.path.join(build.TAG, s) for s in build.HOST_TARGETS[name]]
    build.run(cc + flags + ["-I" + i for i in build.INCLUDES] + srcs + ["-o", exe])
    return exe


def design_images(exe, out):
    """Renders each design with its example data. Returns the number of errors."""
    errors = 0
    for path in sorted(glob.glob(os.path.join(build.TAG, "designs", "*.json"))):
        design = label_design.load(path)
        ex_path = os.path.join(build.TAG, "designs", "examples", design["id"] + ".json")
        if not os.path.exists(ex_path):
            print("no example data for", design["id"], "(designs/examples/%s.json)" % design["id"])
            continue
        for n, data in enumerate(label_design.load(ex_path), 1):
            try:
                c = label_design.render(design, data)
            except label_design.DesignError as e:
                print("ERROR %s example %d: %s" % (design["id"], n, e))
                errors += 1
                continue
            flags = sum(bit for key, bit in FLAG_BITS.items() if c.get(key))
            ppm = os.path.join(out, "design-%s-%d.ppm" % (design["id"], n))
            r = subprocess.run([exe, "-o", ppm, str(LAYOUT_IDS[c["layout"]]), str(flags)] + c["fields"])
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
    errors = design_images(exe, out)
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
