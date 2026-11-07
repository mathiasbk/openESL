#!/usr/bin/env python3
"""Renders the "What you can make" gallery (README.md, the Overview tab of the web page) with
the simulator: every picture is the screen of a simulated tag, drawn by the AP code and shown
by the tag firmware, as on real hardware.

  python3 tools/gallery/render_gallery.py [OUTDIR]     OUTDIR: default docs/images/gallery

Needs a C compiler and make (Linux or macOS), like the simulator. For each tag model it starts
"esl-sim serve" with one tag, uploads a layout file over the AP's serial API (esl-client) and
sends the content, waits until the tag reports it shown, and saves the tag's screen. The
layouts are the template catalog's (templates/), plus one drawn here (FREEFORM) to show what a
layout of your own can hold. The picture is tools/gallery/photo-154.png (docs/images/ap-c6-case.jpg
cropped to the 1.54" screen in greyscale), sent with the AP's dithering. All the text is made up.

The PNGs are the screen at twice its size (as the simulator saves it) with a thin grey frame
around it, so a white screen stands out on a white page.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "esl-client"))
import esl_client  # noqa: E402

MAC = "00000123456789A8"
SIMS = {"2.9": "29", "1.54": "154", "2.9-bwr": "293"}   # model -> sim/build/<dir>

# A layout of its own: rounded boxes, an icon picked by a field, big digits, inverted and white
# text and lines, with the weather template's icons
FREEFORM = {
    "name": "home",
    "fields": ["symbol", "temp", "desc", "day", "when1", "what1", "when2", "what2"],
    "elements": [
        {"type": "rect", "x": 4, "y": 4, "w": 104, "h": 120, "radius": 12, "border": 2},
        {"type": "icon", "field": "symbol", "x": 34, "y": 10, "w": 44, "h": 40},
        {"type": "text", "field": "temp", "x": 10, "y": 50, "w": 74, "h": 44, "fonts": ["price44", "mid"],
         "align": "right", "valign": "middle", "lines": 1},
        {"type": "text", "text": "°", "x": 86, "y": 54, "w": 18, "h": 24, "fonts": ["medium"], "lines": 1},
        {"type": "text", "field": "desc", "x": 8, "y": 96, "w": 96, "h": 22, "fonts": ["small"], "align": "center", "lines": 1},
        {"type": "text", "field": "day", "x": 114, "y": 4, "w": 178, "h": 28, "fonts": ["medium", "text20"],
         "align": "center", "valign": "middle", "lines": 1, "color": "invert"},
        {"type": "rect", "x": 114, "y": 40, "w": 62, "h": 26, "radius": 8},
        {"type": "text", "field": "when1", "x": 114, "y": 40, "w": 62, "h": 26, "fonts": ["text20", "small"],
         "align": "center", "valign": "middle", "lines": 1, "color": "white"},
        {"type": "text", "field": "what1", "x": 182, "y": 40, "w": 110, "h": 26, "fonts": ["text20", "small"],
         "valign": "middle", "lines": 1},
        {"type": "line", "x": 114, "y": 72, "w": 178, "h": 1, "width": 1},
        {"type": "rect", "x": 114, "y": 78, "w": 62, "h": 26, "radius": 8, "border": 2},
        {"type": "text", "field": "when2", "x": 114, "y": 78, "w": 62, "h": 26, "fonts": ["text20", "small"],
         "align": "center", "valign": "middle", "lines": 1},
        {"type": "text", "field": "what2", "x": 182, "y": 78, "w": 110, "h": 26, "fonts": ["text20", "small"],
         "valign": "middle", "lines": 1},
        {"type": "line", "x": 114, "y": 110, "w": 14, "h": 14, "width": 2, "direction": "up"},
        {"type": "line", "x": 124, "y": 110, "w": 14, "h": 14, "width": 2, "direction": "up"},
        {"type": "line", "x": 134, "y": 110, "w": 14, "h": 14, "width": 2, "direction": "up"},
        {"type": "text", "text": "Have a nice day", "x": 152, "y": 106, "w": 140, "h": 20, "fonts": ["small"],
         "align": "right", "valign": "bottom", "lines": 1},
    ],
}

# (file name, model, template id or None, fields, caption); template None: FREEFORM, "photo": the picture
GALLERY = [
    ("weather", "2.9", "weather", ["Lakeside", "7", "partsun", "Partly cloudy", "3° / 9°", "4 m/s  0,6 mm"],
     "Weather with a symbol that follows the forecast"),
    ("freeform", "2.9", None, ["rain", "12", "Light rain", "Sat 14 November", "09:30", "Swimming", "18:00", "Pizza night"],
     "A layout of your own: boxes, round corners, lines and icons"),
    ("photo", "1.54", "photo", None, "A photo, dithered by the access point"),
    ("guest-wifi", "1.54", "guest-wifi", ["HomeGuest", "sunny-day-42", "WIFI:T:WPA;S:HomeGuest;P:sunny-day-42;;"],
     "Guest WiFi with a QR code to join"),
    ("calendar", "2.9", "calendar", ["14", "Sat November", "Book club", "18:00-20:00  Library", "Bike service", "Sun 10:00  Workshop"],
     "Your next events from a calendar"),
    ("shelf-red", "2.9-bwr", "shelf", ["Rye bread", "34,90", "kr 46,53/kg", "Baked today, 750 g"],
     "A shelf label with red, on the 2.9\" black/white/red"),
    ("3d-printer", "1.54", "3d-printer", ["Printer", "benchy.gcode", "70", "Left 1 h 12 min", "Done at 14:35", "              "],
     "A 3D print's progress, from Home Assistant"),
    ("product-ean", "2.9", "vare-ean", ["Fjordbakst", "Oat crackers 250 g", "7038010009457", "27,50"],
     "A price with an EAN-13 barcode"),
    ("meeting-room", "2.9", "meeting-room", ["Room Aurora", "BUSY", "Now: Team meeting until 11:00", "Next: 13:00 Planning"],
     "A meeting room sign"),
    ("opening-hours", "1.54", "opening-hours", ["Opening hours", "Mon-Fri  9-21", "Saturday  9-18", "Sunday  closed", "Holidays: see our website"],
     "Opening hours on the door"),
]


def templates():
    out = {}
    for sub in sorted(os.listdir(os.path.join(ROOT, "templates"))):
        d = os.path.join(ROOT, "templates", sub)
        if os.path.isdir(d):
            for f in sorted(os.listdir(d)):
                if f.endswith(".json"):
                    with open(os.path.join(d, f), encoding="utf-8") as fh:
                        t = json.load(fh)
                    out[t["id"]] = t
    return out


def variant(t, model):
    v = t["variants"][model]
    return t["variants"][v["same_as"]] if "same_as" in v else v


def layout_file(model, tpl):
    """A layout file with one layout, as the designer's "From template" gives it (soloFile in web/js/catalog.js)."""
    if tpl is None:
        layout, icons = FREEFORM, variant(TEMPLATES["weather"], "2.9" if model.startswith("2.9") else model)["icons"]
    else:
        v = variant(TEMPLATES[tpl], model)
        layout = {k: val for k, val in v["layout"].items() if not k.startswith("_")}
        icons = v.get("icons") or {}
    doc = {"format": "esl-layouts", "version": 1, "model": model}
    if icons:
        doc["icons"] = icons
    doc["layouts"] = [layout]
    return doc


def read_png(path):
    """The simulator's PNG (sim/png.c: 8-bit greyscale or palette, filter 0, no interlace) as rows of RGB."""
    data = open(path, "rb").read()
    pos, idat, pal = 8, b"", None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype in (0, 3), "unexpected PNG from the simulator"
        elif kind == b"PLTE":
            pal = [tuple(body[i:i + 3]) for i in range(0, n, 3)]
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    rows = []
    for y in range(h):
        line = raw[y * (w + 1) + 1:(y + 1) * (w + 1)]
        assert raw[y * (w + 1)] == 0
        rows.append([pal[p] if pal else (p, p, p) for p in line])
    return rows


def write_png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def framed(rows, pad=2, edge=(176, 176, 176)):
    w = len(rows[0]) + 2 * pad
    out = [[edge] * w for _ in range(pad)]
    out += [[edge] * pad + r + [edge] * pad for r in rows]
    return out + [[edge] * w for _ in range(pad)]


def wait(what, cond, timeout_s=30):
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        v = cond()
        if v:
            return v
        time.sleep(0.1)
    sys.exit("timed out waiting for " + what)


def render_model(model, items, out, tmp):
    sim = os.path.join(ROOT, "sim", "build", SIMS[model], "esl-sim")
    png = os.path.join(tmp, f"screen-{SIMS[model]}.png")
    p = subprocess.Popen([sim, "serve", "--speed", "120", "--png", png], stdout=subprocess.PIPE, text=True)
    try:
        first = p.stdout.readline().split()
        assert first[:1] == ["PTY"], first
        cli = esl_client.SerialClient(first[1], timeout=5)
        wait("the tag's first poll", lambda: [t for t in cli.tags()["tags"] if t["mac"] == MAC])
        for name, _, tpl, fields, _ in items:
            before = os.path.getmtime(png) if os.path.exists(png) else 0
            if tpl == "photo":
                with open(os.path.join(HERE, f"photo-{SIMS[model]}.png"), "rb") as f:
                    r = cli.set_image(MAC, f.read(), dither=True)
            else:
                doc = layout_file(model, tpl)
                cli.put_layout(model, json.dumps(doc, ensure_ascii=False).encode("utf-8"))
                r = cli.set_tag(MAC, esl_client.content(doc["layouts"][0]["name"], fields))
            if not cli.wait_shown(MAC, r["content_id"], timeout_s=30, poll_s=0.1):
                sys.exit(f"{name}: the tag did not show it")
            wait(f"{name}: the screen", lambda: os.path.exists(png) and os.path.getmtime(png) > before)
            write_png(os.path.join(out, name + ".png"), framed(read_png(png)))
            print(f"{name}.png  ({model})")
    finally:
        p.kill()
        p.wait()
        p.stdout.close()


TEMPLATES = templates()


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "images", "gallery"))
    os.makedirs(out, exist_ok=True)
    subprocess.run(["make", "-C", os.path.join(ROOT, "sim"), "-s"] + [f"build/{d}/esl-sim" for d in SIMS.values()], check=True)
    with tempfile.TemporaryDirectory() as tmp:
        for model in SIMS:
            items = [g for g in GALLERY if g[1] == model]
            if items:
                render_model(model, items, out, tmp)
    with open(os.path.join(out, "gallery.json"), "w", encoding="utf-8") as f:
        json.dump([{"file": g[0] + ".png", "model": g[1], "caption": g[4]} for g in GALLERY], f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
