#!/usr/bin/env python3
"""
Writes png_fixtures.h for test_png.c: PNG files of every colour type and bit depth,
with every row filter, transparency and the deflate block types zlib makes (stored,
fixed, dynamic, a small window), split over several IDAT chunks, plus broken files.
Each good file comes with the CRC-32 of the rows esl_png should give (R, G, B per
pixel, transparency laid over white), worked out here from the pixel values written.

  python3 make_png_fixtures.py     (rewrites png_fixtures.h next to this script)
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def filter_rows(rows, bpp):
    out = b""
    prev = bytes(len(rows[0]))
    for y, row in enumerate(rows):
        f = y % 5
        enc = bytearray()
        for i, x in enumerate(row):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            pred = [0, a, b, (a + b) // 2, paeth(a, b, c)][f]
            enc.append((x - pred) & 0xFF)
        out += bytes([f]) + bytes(enc)
        prev = row
    return out


def pack(samples, depth):
    """One row of samples into bytes"""
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    if depth == 8:
        return bytes(samples)
    out = bytearray()
    acc, n = 0, 0
    for s in samples:
        acc = (acc << depth) | s
        n += depth
        if n == 8:
            out.append(acc)
            acc, n = 0, 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def to8(v, depth):
    if depth == 16:
        return v >> 8
    if depth == 8:
        return v
    return v * 255 // ((1 << depth) - 1)


def over_white(c, a):
    return (c * a + 255 * (255 - a) + 127) // 255


CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def make(w, h, ctype, depth, compress, idat_split, trns=False, extra=True):
    """A PNG file and the CRC of the rows it should decode to"""
    maxv = (1 << depth) - 1
    ch = CHANNELS[ctype]
    palette = []
    if ctype == 3:
        n = min(1 << depth, 200)
        palette = [((i * 37) % 256, (i * 91 + 13) % 256, (255 - i * 5) % 256) for i in range(n)]
    trns_pal = [(i * 53) % 256 for i in range(len(palette) // 2)] if trns and ctype == 3 else []
    rows, rgb = [], b""
    key = None
    for y in range(h):
        samples = []
        for x in range(w):
            v = (x * 7 + y * 13 + (x * y) % 11) & 0xFFFF
            if ctype == 3:
                s = [v % len(palette)]
            else:
                s = [((v * (c + 3) + c * 29) * 2654435761 >> 7) % (maxv + 1) for c in range(ch)]
            samples += s
        rows.append(pack(samples, depth))
        # what the decoder should give for this row
        if trns and ctype in (0, 2) and key is None:
            key = samples[:ch] if ctype == 2 else samples[:1]
        for x in range(w):
            s = samples[x * ch:(x + 1) * ch]
            a = 255
            if ctype == 0:
                r = g = b = to8(s[0], depth)
                if key is not None and s == key:
                    a = 0
            elif ctype == 2:
                r, g, b = (to8(c, depth) for c in s)
                if key is not None and s == key:
                    a = 0
            elif ctype == 3:
                r, g, b = palette[s[0]]
                if s[0] < len(trns_pal):
                    a = trns_pal[s[0]]
            elif ctype == 4:
                r = g = b = to8(s[0], depth)
                a = to8(s[1], depth)
            else:
                r, g, b = (to8(c, depth) for c in s[:3])
                a = to8(s[3], depth)
            rgb += bytes([over_white(r, a), over_white(g, a), over_white(b, a)])
    bpx = ch * depth
    data = compress(filter_rows(rows, max(1, bpx // 8)))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, 0))
    if extra:
        png += chunk(b"tEXt", b"Comment\0esl_png test")
    if ctype == 3:
        png += chunk(b"PLTE", b"".join(bytes(p) for p in palette))
    if trns and ctype == 3:
        png += chunk(b"tRNS", bytes(trns_pal))
    if trns and ctype in (0, 2):
        png += chunk(b"tRNS", b"".join(struct.pack(">H", k) for k in key))
    for i in range(0, len(data), idat_split):
        png += chunk(b"IDAT", data[i:i + idat_split])
    if extra:
        png += chunk(b"zzZz", b"unknown ancillary chunk")
    png += chunk(b"IEND", b"")
    return png, zlib.crc32(rgb) & 0xFFFFFFFF


def z(level=6, strategy=zlib.Z_DEFAULT_STRATEGY, wbits=15):
    def f(data):
        c = zlib.compressobj(level, zlib.DEFLATED, wbits, 9, strategy)
        return c.compress(data) + c.flush()
    return f


def main():
    good, bad = [], []
    cases = [
        ("grey1", 37, 13, 0, 1, z(), 1000, False),
        ("grey2", 37, 13, 0, 2, z(9), 50, False),
        ("grey4_trns", 37, 13, 0, 4, z(0), 1000, True),
        ("grey8", 37, 13, 0, 8, z(strategy=zlib.Z_FIXED), 7, False),
        ("grey16_trns", 21, 9, 0, 16, z(1), 64, True),
        ("rgb8", 37, 13, 2, 8, z(9), 100, False),
        ("rgb8_trns", 37, 13, 2, 8, z(6, wbits=9), 33, True),
        ("rgb16", 19, 11, 2, 16, z(strategy=zlib.Z_HUFFMAN_ONLY), 1000, False),
        ("pal1", 37, 13, 3, 1, z(), 1000, False),
        ("pal2_trns", 37, 13, 3, 2, z(), 1000, True),
        ("pal4", 37, 13, 3, 4, z(strategy=zlib.Z_RLE), 1000, False),
        ("pal8_trns", 37, 13, 3, 8, z(9), 5, True),
        ("ga8", 37, 13, 4, 8, z(), 1000, False),
        ("ga16", 17, 9, 4, 16, z(0), 100, False),
        ("rgba8", 37, 13, 6, 8, z(9), 1000, False),
        ("rgba16", 17, 9, 6, 16, z(strategy=zlib.Z_FIXED), 1000, False),
        ("screen29", 296, 128, 0, 1, z(9), 2000, False),
    ]
    for name, w, h, ctype, depth, comp, split, trns in cases:
        png, crc = make(w, h, ctype, depth, comp, split, trns)
        good.append((name, png, w, h, crc))

    base, _ = make(16, 8, 2, 8, z(), 1000, extra=False)
    ihdr_end = 8 + 25
    flipped_crc = bytearray(base)
    flipped_crc[ihdr_end - 1] ^= 1
    interlaced = bytearray(base)
    interlaced[8 + 8 + 12] = 1
    interlaced[ihdr_end - 4:ihdr_end] = struct.pack(">I", zlib.crc32(bytes(interlaced[12:ihdr_end - 4])) & 0xFFFFFFFF)
    # a wrong Adler-32: the last byte of the zlib stream, with the IDAT's CRC put right
    idat = base.index(b"IDAT")
    ilen = struct.unpack(">I", base[idat - 4:idat])[0]
    bad_adler = bytearray(base)
    bad_adler[idat + 4 + ilen - 1] ^= 0x55
    bad_adler[idat + 4 + ilen:idat + 8 + ilen] = struct.pack(
        ">I", zlib.crc32(bytes(bad_adler[idat:idat + 4 + ilen])) & 0xFFFFFFFF)
    # more rows than IHDR says (the image is 9 rows, IHDR 8): bytes past the last row
    tall, _ = make(16, 9, 2, 8, z(), 1000, extra=False)
    trailing = bytearray(tall)
    trailing[8 + 8 + 7] = 8
    trailing[ihdr_end - 4:ihdr_end] = struct.pack(">I", zlib.crc32(bytes(trailing[12:ihdr_end - 4])) & 0xFFFFFFFF)
    bad += [
        ("trailing", bytes(trailing), "ESL_PNG_ERR_BAD"),
        ("not_png", b"GIF89a" + base[6:], "ESL_PNG_ERR_NOT_PNG"),
        ("bad_crc", bytes(flipped_crc), "ESL_PNG_ERR_BAD"),
        ("interlaced", bytes(interlaced), "ESL_PNG_ERR_UNSUPPORTED"),
        ("bad_adler", bytes(bad_adler), "ESL_PNG_ERR_BAD"),
        ("cut", base[:len(base) - 30], "ESL_PNG_ERR_READ"),
        ("no_iend", base[:len(base) - 12], "ESL_PNG_ERR_READ"),
    ]

    def arr(data):
        lines = []
        for i in range(0, len(data), 24):
            lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 24]) + ",")
        return "\n".join(lines)

    out = ["/* Written by make_png_fixtures.py: do not edit. */", "#pragma once", "#include <stdint.h>", ""]
    for name, png, *_ in good + bad:
        out += ["static const uint8_t png_%s[] = {" % name, arr(png), "};"]
    out += ["", "static const struct {", "    const char *name;", "    const uint8_t *data;", "    size_t len;",
            "    uint32_t w, h, crc;", "} png_good[] = {"]
    for name, png, w, h, crc in good:
        out.append('    {"%s", png_%s, sizeof(png_%s), %d, %d, 0x%08xu},' % (name, name, name, w, h, crc))
    out += ["};", "", "static const struct {", "    const char *name;", "    const uint8_t *data;", "    size_t len;",
            "    esl_png_err_t err;", "} png_bad[] = {"]
    for name, png, err in bad:
        out.append('    {"%s", png_%s, sizeof(png_%s), %s},' % (name, name, name, err))
    out += ["};", ""]
    with open(os.path.join(HERE, "png_fixtures.h"), "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
