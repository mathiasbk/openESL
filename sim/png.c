/* png.c - writes an 8-bit greyscale PNG with no libraries: zlib "stored" blocks (no
 * compression), which is fine for a few screens of a 200 x 200 or 296 x 128 tag. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim.h"

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    uint8_t b[4];
    put32(b, (uint32_t)n);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n)
        fwrite(data, 1, n, f);
    put32(b, crc(crc(0, (const uint8_t *)type, 4), data, n));
    fwrite(b, 1, 4, f);
}

bool png_write_gray(const char *path, const uint8_t *pix, int w, int h)
{
    size_t raw_len = (size_t)(w + 1) * (size_t)h;          /* a filter byte (0) before each row */
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + 5 * blocks + 4;
    uint8_t *raw = malloc(raw_len), *z = malloc(z_len), *q;
    uint8_t ihdr[13];
    uint32_t a = 1, b = 0;
    FILE *f;

    if (!raw || !z) {
        free(raw);
        free(z);
        return false;
    }
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (size_t)(w + 1)] = 0;
        memcpy(raw + (size_t)y * (size_t)(w + 1) + 1, pix + (size_t)y * (size_t)w, (size_t)w);
    }
    q = z;
    *q++ = 0x78;
    *q++ = 0x01;
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        *q++ = off + n == raw_len;   /* BFINAL, BTYPE 00 */
        *q++ = (uint8_t)n;
        *q++ = (uint8_t)(n >> 8);
        *q++ = (uint8_t)~n;
        *q++ = (uint8_t)(~n >> 8);
        memcpy(q, raw + off, n);
        q += n;
    }
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    put32(q, (b << 16) | a);
    q += 4;

    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 0;    /* greyscale */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    f = fopen(path, "wb");
    if (f) {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        chunk(f, "IHDR", ihdr, sizeof(ihdr));
        chunk(f, "IDAT", z, (size_t)(q - z));
        chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw);
    free(z);
    return f != NULL;
}
