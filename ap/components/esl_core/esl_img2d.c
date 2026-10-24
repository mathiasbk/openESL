#include "esl_img2d.h"
#include <stdbool.h>
#include <string.h>

#define COL_MAX (ESL_IMG_2D_MAX_H / 8)

typedef struct {
    uint8_t *out;
    size_t max;
    size_t o;          /* whole bytes written */
    size_t limit;      /* ... at most this many until the next column is read (in place) */
    uint8_t acc;       /* bits not yet written, from bit 7 down */
    uint8_t nacc;
    bool full;
} bits_t;

static void put(bits_t *b, uint32_t v, unsigned n)
{
    while (n--) {
        b->acc |= (uint8_t)(((v >> n) & 1u) << (7 - b->nacc));
        if (++b->nacc == 8) {
            if (b->o >= b->max || b->o >= b->limit)
                b->full = true;
            else if (b->out)
                b->out[b->o++] = b->acc;
            else
                b->o++;
            b->acc = 0;
            b->nacc = 0;
        }
    }
}

/* A run length as an Exp-Golomb code of order 2: v + 4 in binary (b bits), after b - 3 zeros */
static void put_run(bits_t *b, unsigned v)
{
    unsigned bits = 0;
    v += 4;
    for (unsigned t = v; t; t >>= 1)
        bits++;
    put(b, 0, bits - 3);
    put(b, v, bits);
}

static int px(const uint8_t *c, int y)
{
    return y < 0 ? 0 : (c[y >> 3] >> (7 - (y & 7))) & 1;
}

/* The first row after a with colour col whose row above has the other colour, h if none */
static int change_to(const uint8_t *c, int a, int col, int h)
{
    for (int y = a + 1; y < h; y++)
        if (px(c, y) == col && px(c, y - 1) != col)
            return y;
    return h;
}

/* ahead: how far ahead of the output the input starts (coding in place) */
static size_t code(const uint8_t *in, size_t n, uint16_t h, uint8_t *out, size_t ahead, size_t max)
{
    uint8_t ref[COL_MAX], cur[COL_MAX];
    size_t cb = ((size_t)h + 7) / 8;
    bits_t b = {out, max, 0, 0, 0, 0, false};

    if (!h || h > ESL_IMG_2D_MAX_H || n % cb)
        return 0;
    memset(ref, 0, sizeof(ref));
    for (size_t x = 0; x < n / cb; x++) {
        memcpy(cur, in + x * cb, cb);
        b.limit = ahead + (x + 1) * cb;
        int a0 = -1, col = 0;
        while (a0 < h && !b.full) {
            int a1 = change_to(cur, a0, !col, h);
            int b1 = change_to(ref, a0, !col, h);
            int b2 = change_to(ref, b1, col, h);
            if (b2 < a1) {
                put(&b, 0x1, 4);                        /* pass: 0001 */
                a0 = b2;
                continue;
            }
            int d = a1 - b1;
            if (d >= -3 && d <= 3) {
                /* vertical: 1, 011 / 010, 000011 / 000010, 0000011 / 0000010 */
                static const uint8_t code[7] = {0x2, 0x2, 0x2, 0x1, 0x3, 0x3, 0x3};
                static const uint8_t len[7] = {7, 6, 3, 1, 3, 6, 7};
                put(&b, code[d + 3], len[d + 3]);
                a0 = a1;
                col = !col;
                continue;
            }
            int a2 = change_to(cur, a1, col, h);        /* horizontal: 001, two runs */
            put(&b, 0x1, 3);
            put_run(&b, (unsigned)(a1 - (a0 < 0 ? 0 : a0)));
            put_run(&b, (unsigned)(a2 - a1));
            a0 = a2;
        }
        if (b.full)
            return 0;
        memcpy(ref, cur, cb);
    }
    b.limit = SIZE_MAX;
    if (b.nacc)
        put(&b, 0, 8 - b.nacc);
    return b.full ? 0 : b.o;
}

size_t esl_img_2d_encode(const uint8_t *in, size_t n, uint16_t h, uint8_t *out, size_t max)
{
    size_t ahead = out && (uintptr_t)out <= (uintptr_t)in ? (size_t)((uintptr_t)in - (uintptr_t)out) : SIZE_MAX / 2;
    return code(in, n, h, out, ahead, max);
}

size_t esl_img_2d_count(const uint8_t *in, size_t n, uint16_t h, size_t margin, size_t max)
{
    return code(in, n, h, NULL, margin, max);
}
