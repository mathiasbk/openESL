#!/usr/bin/env python3
"""
Host tests and previews for every tag model, without hardware.

  python tools/render_host.py            tests + previews for all models
  python tools/render_host.py 154        only one model (a build from shared/models.json)

For each model (src/model.h) it compiles the host tests with a normal C compiler and
-DMODEL=<model>, runs them (including test_epd, which rebuilds the image from a model of
the display controller's RAM), and draws:

  build/preview/<model>/<example>.png            the built-in layouts with the examples below
  build/preview/<model>/layout-<name>.png        every layout in designs/layouts/<model>.json,
                                                 with the field names as text
  build/preview/<model>/design-<id>-<n>.png      every design in designs/*.json, filled with
                                                 the data in designs/examples/<id>.json
  build/preview/<model>/epd_path.png             the image as epd.c leaves it in controller RAM
  build/preview/<model>/all.png                  all of the above on one sheet

The screens are drawn by the AP's own code (esl_render.c, through web/wasm/web_api.c built
natively by web/build.py), so they are the images the AP sends to a tag. A layout file the
AP would reject stops the run with the AP's message.

The images are scaled 2x. Without Pillow the PPM files are kept instead.
Uses the compiler and source lists from tools/build.py, which builds the default model only.
"""
import glob
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import build  # noqa: E402
import label_design  # noqa: E402

# web/build.py has the same file name as tools/build.py, so load it under another name
_spec = importlib.util.spec_from_file_location("web_build", os.path.join(build.ROOT, "web", "build.py"))
web_build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(web_build)

MODELS = [m["build"] for m in build.models.load()]                       # -DMODEL=<build>
MODEL_NAME = {m["build"]: m["name"] for m in build.models.load()}        # as in the API and the layout files
LAYOUT_IDS = {"price": 1, "text": 2}
LAYOUT_FILE = 0x10                               # ESL_LAYOUT_FILE: layout n of the file is 0x10 + n
FLAG_BITS = {"price_highlight": 0x01, "frame": 0x02}
COLORS = {"0": (255, 255, 255), "1": (0, 0, 0), "2": (200, 0, 0)}

HL, FRAME = FLAG_BITS["price_highlight"], FLAG_BITS["frame"]
EXAMPLES = [   # (file, layout, flags, fields): the built-in layouts
    ("pris_framhevet", "price", HL, ["Lettmelk 1 L", "24.90", "kr 24,90/l"]),
    ("pris_komma", "price", 0, ["Brunost Gudbrandsdalsost", "89,-", "kr/kg 178,00"]),
    ("pris_langt_navn", "price", 0, ["Kjøttdeig av storfe 14 % fett, ekstra stor familiepakning", "129,90", "kr/kg 108,25"]),
    ("pris_for_langt", "price", HL, ["Grovbrød med solsikkekjerner, havre og knekt rug, oppskåret og pakket i papirpose 750 g",
                                     "39,90", ""]),
    ("pris_bred", "price", HL, ["Kaffemaskin", "12 499,00", "Spar kr 2 500"]),
    ("pris_tekst", "price", FRAME, ["Appelsiner", "3 for 30", "Gjelder til søndag"]),
    ("pris_ramme", "price", HL | FRAME, ["Rømme lett 18 % Tine", "19,90", "Ærlig økologisk – kr 66,33/l"]),
    ("tekst", "text", 0, ["Åpningstider", "Man–fre 9–21", "Lørdag 9–18", "Søndag stengt"]),
    ("tekst_lang", "text", FRAME, ["Velkommen!", "Denne linjen er altfor lang til å få plass", "", "ÆØÅ æøå é ü ß ©"]),
]


def compile_host(name, model):
    cc = build.find_cc()
    out_dir = os.path.join(build.BUILD, "host-%s" % model)
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, name + (".exe" if os.name == "nt" else ""))
    flags = ["-std=c99", "-Wall", "-Wextra", "-O1", "-g", "-DMODEL=%s" % model] + build.version_defines(model)
    srcs = [os.path.join(build.TAG, s) for s in build.HOST_TARGETS[name]]
    build.run(cc + flags + ["-I" + i for i in build.INCLUDES] + srcs + ["-o", exe])
    return exe


class Renderer:
    """The AP's drawing for one model (web_render), with the model's layout file if it has one."""

    def __init__(self, model):
        self.exe = web_build.build_native(model)
        self.path = os.path.join(build.TAG, "designs", "layouts", MODEL_NAME[model] + ".json")
        self.lfile = None
        if os.path.exists(self.path):
            self.lfile = label_design.load(self.path)
            ok, msg = self.run("x.ppm", "price", 0, [], check_only=True)
            if not ok:
                sys.exit("%s: the AP rejects this file: %s" % (self.path, msg))

    def run(self, ppm, layout, flags, fields, check_only=False):
        """Draws one screen to ppm. Returns (ok, error message)."""
        if layout in LAYOUT_IDS:
            num = LAYOUT_IDS[layout]
        else:
            num = LAYOUT_FILE + [lay["name"] for lay in self.lfile["layouts"]].index(layout)
        r = subprocess.run([self.exe, self.path if self.lfile else "-", str(num), str(flags)] + list(fields),
                           capture_output=True, text=True, encoding="utf-8")
        lines = r.stdout.splitlines()
        if r.returncode or not lines or not lines[0].startswith("ok "):
            return False, (lines[0][6:] if lines and lines[0].startswith("error ") else r.stderr.strip())
        if check_only:
            return True, ""
        _, w, h = lines[0].split()
        with open(ppm, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (int(w), int(h)))
            f.write(b"".join(bytes(COLORS[c]) for row in lines[1:] for c in row))
        return True, ""


def draw(r, ppm, layout, flags, fields, what):
    ok, msg = r.run(ppm, layout, flags, fields)
    if not ok:
        print("ERROR %s: %s" % (what, msg))
    return not ok


def layout_images(r, out):
    """Every layout of the file, with each field showing its own name. Returns the errors."""
    return sum(draw(r, os.path.join(out, "layout-%s.ppm" % lay["name"]), lay["name"], HL | FRAME,
                    list(lay["fields"]), "layout " + lay["name"])
               for lay in r.lfile["layouts"])


def design_images(r, out):
    """Renders each design with its example data. Returns the number of errors."""
    errors = 0
    for path in sorted(glob.glob(os.path.join(build.TAG, "designs", "*.json"))):
        design = label_design.load(path)
        ex_path = os.path.join(build.TAG, "designs", "examples", design["id"] + ".json")
        if not os.path.exists(ex_path):
            print("no example data for", design["id"], "(designs/examples/%s.json)" % design["id"])
            continue
        if label_design.layout_fields(design.get("layout"), r.lfile) is None:
            print("%s: layout %r is not in this model's layout file, skipped" % (design["id"], design.get("layout")))
            continue
        for n, data in enumerate(label_design.load(ex_path), 1):
            try:
                c = label_design.render(design, data, r.lfile)
            except label_design.DesignError as e:
                print("ERROR %s example %d: %s" % (design["id"], n, e))
                errors += 1
                continue
            flags = sum(bit for key, bit in FLAG_BITS.items() if c.get(key))
            ppm = os.path.join(out, "design-%s-%d.ppm" % (design["id"], n))
            errors += draw(r, ppm, c["layout"], flags, c["fields"], "%s example %d" % (design["id"], n))
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
    r = Renderer(model)
    errors = sum(draw(r, os.path.join(out, name + ".ppm"), layout, flags, fields, name)
                 for name, layout, flags, fields in EXAMPLES)
    if r.lfile:
        errors += layout_images(r, out)
    errors += design_images(r, out)
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
        sys.exit("%d screen(s) failed" % errors)


if __name__ == "__main__":
    main(sys.argv)
