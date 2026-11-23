#!/usr/bin/env python3
"""Draws the openESL demo card for a Gicisky 2.9" BWRY Bluetooth tag.

  python3 tools/gallery/render_bwry.py --name NEMR92852188 --address FF:FF:92:85:21:88 \
      --out docs/images/gallery/openesl-bwry-1.png

The 802.15.4 pictures in the gallery come from the simulator (render_gallery.py). A Bluetooth
tag keeps the maker's firmware, so there is no simulator for it: this card is drawn directly in
the tag's four colours (black, white, red and yellow) and is the image the tag is sent. Each
tag's card carries its own BLE name and address. The PNG is the screen at twice its size with
the same thin grey frame as the other gallery pictures.

Needs Pillow and the Noto Sans fonts (tools/dev/requirements.txt has Pillow; on Debian/Ubuntu
the fonts are in fonts-noto-core).
"""
import argparse
import os
import sys
from collections import Counter

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow is missing: pip install -r tools/dev/requirements.txt")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

W, H = 296, 128
BLACK, WHITE, RED, YELLOW = (0, 0, 0), (255, 255, 255), (220, 30, 30), (255, 210, 0)
PALETTE = [BLACK, WHITE, RED, YELLOW]
FONTS = ["/usr/share/fonts/google-noto", "/usr/share/fonts/truetype/noto",
         "/Library/Fonts", "/System/Library/Fonts"]


def font(name, size):
    for d in FONTS:
        path = os.path.join(d, name)
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    sys.exit(f"{name} not found; install the Noto Sans fonts (fonts-noto-core)")


def nearest(px):
    """The closest of the tag's four colours, weighing green most and blue least."""
    best, best_d = None, None
    for c in PALETTE:
        dr, dg, db = px[0] - c[0], px[1] - c[1], px[2] - c[2]
        d = 3 * dr * dr + 4 * dg * dg + 2 * db * db
        if best_d is None or d < best_d:
            best, best_d = c, d
    return best


def card(name, address):
    im = Image.new("RGB", (W, H), WHITE)
    d = ImageDraw.Draw(im)
    f_brand = font("NotoSans-Bold.ttf", 22)
    f_badge = font("NotoSans-Bold.ttf", 13)
    f_title = font("NotoSans-Bold.ttf", 16)
    f_text = font("NotoSans-Regular.ttf", 13)
    f_small = font("NotoSans-Regular.ttf", 11)
    f_four = font("NotoSans-Bold.ttf", 34)
    f_four_label = font("NotoSans-Bold.ttf", 11)
    f_swatch = font("NotoSans-Bold.ttf", 9)

    # The red band with the openESL name and a yellow demo badge.
    d.rectangle([0, 0, W - 1, 32], fill=RED)
    d.text((8, -2), "openESL", font=f_brand, fill=WHITE)
    d.rounded_rectangle([232, 4, 288, 27], radius=7, fill=YELLOW)
    d.text((260, 15), "demo", font=f_badge, fill=BLACK, anchor="mm")

    # What the tag is, and which one this is.
    d.text((8, 37), 'Gicisky / PICKSMART 2.9"', font=f_title, fill=BLACK)
    d.text((8, 58), f"BWRY \u00b7 {name}", font=f_text, fill=BLACK)
    d.text((8, 73), address, font=f_text, fill=BLACK)
    d.text((8, 87), "Bluetooth LE \u00b7 4 colours", font=f_small, fill=BLACK)

    # A yellow block, so the fourth colour is large and plain to see.
    d.rounded_rectangle([208, 38, 288, 96], radius=8, fill=YELLOW)
    d.text((248, 42), "4", font=f_four, fill=BLACK, anchor="ma")
    d.text((248, 82), "colours", font=f_four_label, fill=BLACK, anchor="ma")

    # The four colours along the bottom, each with its name.
    swatches = [("BLACK", BLACK, WHITE), ("WHITE", WHITE, BLACK), ("RED", RED, WHITE), ("YELLOW", YELLOW, BLACK)]
    bw = W // 4
    for i, (name, fill, ink) in enumerate(swatches):
        x = i * bw
        d.rectangle([x, 104, x + bw - 1, H - 1], fill=fill)
        if fill == WHITE:
            d.rectangle([x, 104, x + bw - 1, H - 1], outline=BLACK)
        d.text((x + bw // 2, 115), name, font=f_swatch, fill=ink, anchor="mm")

    # The tag has exactly these four colours; snap the anti-aliased text to them.
    px = im.load()
    for y in range(H):
        for x in range(W):
            px[x, y] = nearest(px[x, y])
    return im


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", required=True, help="the tag's BLE name, such as NEMR92852188")
    ap.add_argument("--address", required=True, help="the tag's BLE address, such as FF:FF:92:85:21:88")
    ap.add_argument("--out", help="default docs/images/gallery/openesl-bwry.png")
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(ROOT, "docs", "images", "gallery", "openesl-bwry.png"))
    im = card(a.name, a.address.upper())
    big = im.resize((W * 2, H * 2), Image.NEAREST)
    pad, edge = 2, (176, 176, 176)
    framed = Image.new("RGB", (W * 2 + 2 * pad, H * 2 + 2 * pad), edge)
    framed.paste(big, (pad, pad))
    parent = os.path.dirname(out)
    if parent:
        os.makedirs(parent, exist_ok=True)
    framed.save(out)
    print(f"{out}  ({framed.size[0]}x{framed.size[1]}, {dict(Counter(im.getcolors(W * H)))})")


if __name__ == "__main__":
    main()
