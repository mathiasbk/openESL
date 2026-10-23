#!/usr/bin/env python3
"""
Generates the font tables for the tag from a TTF font with an open licence (Inter, OFL 1.1).

Output: tag/fonts/font_<name>.c, one file per font, in the format described by src/font.h:
  - fixed height per font, proportional width per character (= the font's advance, rounded)
  - the bitmap is stored column by column, because the tag draws one screen column
    at a time (see src/draw.c). Each column is ceil(height/8) bytes; the MSB of the
    first byte is the top pixel.
  - character 0x7F (DEL, unused in Latin-1) is used for "…" (U+2026) when truncating.

Glyphs are drawn with FreeType's auto-hinter (light target, vertical snapping only, so the
widths stay the font's own advances) and thresholded from grey. Inter has no hinting of its
own, and FreeType's plain monochrome rendering of it at small sizes gives stems of uneven
thickness, broken curves and dropped details: "small" and "medium" looked ragged on the tag.

Run from tag/:  python tools/gen_font.py [name ...]
With names (e.g. "mid"), only those fonts are regenerated.

Requires freetype-py and Pillow (pip install freetype-py pillow). freetype-py carries its own
FreeType, and size, height and baseline are fixed in FONTS below, so the metrics do not move
between machines; a different FreeType build can still change a few pixels in some glyphs.
The generated files are checked into git and are the reference, so regenerate only the font
you are changing, and look at preview_<name>.png afterwards.
"""
import os
import sys

import freetype
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
TAG = os.path.dirname(HERE)
FONT_SRC = os.path.join(TAG, "fonts", "src")
OUT = os.path.join(TAG, "fonts")

ELLIPSIS_CODE = 0x7F

# Grey level (0-255) from which a pixel is drawn black. Below the middle, so thin parts
# of a glyph (the bowl of "d", the dot on "i") are kept rather than dropped.
THRESHOLD = 110
LOAD_FLAGS = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_FORCE_AUTOHINT | freetype.FT_LOAD_TARGET_LIGHT

LATIN1 = [c for c in range(0x20, 0x7F)] + [ELLIPSIS_CODE] + [c for c in range(0xA0, 0x100)]
PRICE = [ord(c) for c in " ,-.0123456789:"]

# name, source font, pixel size, height, baseline row, character set.
# Size, height and baseline are fixed so the fonts keep the same metrics (and the layouts
# the same fit) whichever FreeType build generates them.
FONTS = [
    ("small", "Inter-SemiBold.ttf", 13, 17, 13, LATIN1),
    ("medium", "Inter-SemiBold.ttf", 19, 24, 19, LATIN1),
    ("large", "Inter-Bold.ttf", 56, 52, 42, PRICE),
    # Price font between medium and large: "129,90" fits the 1.54" (152 px) and long prices the 2.9"
    ("mid", "Inter-Bold.ttf", 39, 36, 29, PRICE),
    # Between small and medium (Latin-1, like small and medium)
    ("text20", "Inter-SemiBold.ttf", 16, 20, 16, LATIN1),
    # Price between mid and large: "129,90" fits the 1.54" (152 px)
    ("price44", "Inter-Bold.ttf", 44, 44, 35, PRICE),
]


def unicode_for(code):
    return "…" if code == ELLIPSIS_CODE else bytes([code]).decode("latin-1")


def ink_rows(face, ch):
    """Rows above and below the baseline that the glyph actually draws (after the threshold)."""
    face.load_char(ch, LOAD_FLAGS)
    g = face.glyph
    bmp = g.bitmap
    buf = bytes(bmp.buffer)
    rows = [r for r in range(bmp.rows)
            if max(buf[r * bmp.pitch:r * bmp.pitch + bmp.width], default=0) >= THRESHOLD]
    if not rows:
        return 0, 0
    return g.bitmap_top - rows[0], rows[-1] - g.bitmap_top + 1


def check_fit(face, codes, baseline, height):
    """Warn about characters that are clipped because they reach outside the cell."""
    for c in codes:
        ch = unicode_for(c)
        if ch.strip() == "":
            continue
        above, below = ink_rows(face, ch)
        if above > baseline or below > height - baseline:
            print("  note: %r is clipped (%d px above, %d below the baseline)" % (ch, above, below))


def render_glyph(face, code, top, height):
    ch = unicode_for(code)
    face.load_char(ch, LOAD_FLAGS)
    g = face.glyph
    adv = int(round(g.linearHoriAdvance / 65536.0))
    if adv <= 0:
        return 0, []
    img = Image.new("1", (adv, height), 0)
    bmp = g.bitmap
    for r in range(bmp.rows):
        y = -top - g.bitmap_top + r
        for c in range(bmp.width):
            x = g.bitmap_left + c
            if 0 <= x < adv and 0 <= y < height and bmp.buffer[r * bmp.pitch + c] >= THRESHOLD:
                img.putpixel((x, y), 1)
    bpc = (height + 7) // 8
    cols = []
    px = img.load()
    for x in range(adv):
        col = [0] * bpc
        for y in range(height):
            if px[x, y]:
                col[y >> 3] |= 0x80 >> (y & 7)
        cols.append(col)
    return adv, cols


def c_array(name, ctype, values, per_line=16):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join(values[i:i + per_line]) + ",")
    return "static const %s __code %s[] = {\n%s\n};\n" % (ctype, name, "\n".join(lines))


def generate(name, src, size, height, baseline, codes):
    path = os.path.join(FONT_SRC, src)
    face = freetype.Face(path)
    face.set_pixel_sizes(0, size)
    check_fit(face, codes, baseline, height)
    top = -baseline
    bpc = (height + 7) // 8
    first, last = min(codes), max(codes)
    widths, offsets, bitmap = [], [], []
    glyphs = {}
    for c in range(first, last + 1):
        if c in codes:
            adv, cols = render_glyph(face, c, top, height)
        else:
            adv, cols = 0, []
        glyphs[c] = (adv, cols)
        widths.append(adv)
        offsets.append(len(bitmap))
        for col in cols:
            bitmap.extend(col)
    if len(bitmap) > 0xFFFF:
        raise SystemExit("font %s is too big for 16-bit offsets" % name)

    out = []
    out.append("/* Generated by tools/gen_font.py. Do not edit by hand. */\n")
    out.append("/* Source: %s, %d px (FreeType size), OFL 1.1, see fonts/OFL.txt */\n" % (src, size))
    out.append("/* Height %d px, baseline at row %d, %d bytes per column, characters 0x%02X-0x%02X, "
               "%d bytes of bitmap */\n\n" % (height, -top, bpc, first, last, len(bitmap)))
    out.append('#include "font.h"\n\n')
    out.append(c_array("w_%s" % name, "uint8_t", [str(w) for w in widths]))
    out.append("\n")
    out.append(c_array("o_%s" % name, "uint16_t", [str(o) for o in offsets], 12))
    out.append("\n")
    out.append(c_array("b_%s" % name, "uint8_t", ["0x%02X" % b for b in bitmap] or ["0"]))
    out.append("\n")
    out.append("const font_t __code font_%s = {\n    %d, %d, %d, 0x%02X, 0x%02X,\n    w_%s, o_%s, b_%s\n};\n"
               % (name, height, -top, bpc, first, last, name, name, name))
    with open(os.path.join(OUT, "font_%s.c" % name), "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(out))

    # Preview for people (not checked into the firmware)
    sample = [c for c in codes if glyphs[c][0] > 0]
    rows = []
    row, w = [], 0
    for c in sample:
        if w + glyphs[c][0] > 600:
            rows.append(row)
            row, w = [], 0
        row.append(c)
        w += glyphs[c][0]
    rows.append(row)
    img = Image.new("1", (600, height * len(rows)), 1)
    px = img.load()
    for ri, r in enumerate(rows):
        x = 0
        for c in r:
            adv, cols = glyphs[c]
            for cx, col in enumerate(cols):
                for y in range(height):
                    if col[y >> 3] & (0x80 >> (y & 7)):
                        px[x + cx, ri * height + y] = 0
            x += adv
    img.save(os.path.join(OUT, "preview_%s.png" % name))
    print("font_%s: height %d, size %d, %d bytes of bitmap, %d characters"
          % (name, height, size, len(bitmap), len(sample)))
    return height


def main():
    names = sys.argv[1:]
    unknown = set(names) - set(spec[0] for spec in FONTS)
    if unknown:
        raise SystemExit("unknown font: %s" % ", ".join(sorted(unknown)))
    for spec in FONTS:
        if not names or spec[0] in names:
            generate(*spec)


if __name__ == "__main__":
    sys.exit(main())
