#include "esl_qr.h"
#include <string.h>

/* QR codes, written from ISO/IEC 18004: byte mode, versions 1..ESL_QR_VERSION_MAX.
 * The tables are the standard's (error correction per version and level, alignment
 * pattern positions); the rest is worked out. Not reentrant: one code at a time (the AP
 * draws under the layouts lock). */

/* Error correction codewords per block, and blocks, per level (L M Q H) and version 1..10 */
static const uint8_t EC_PER_BLOCK[4][ESL_QR_VERSION_MAX] = {
    {7, 10, 15, 20, 26, 18, 20, 24, 30, 18},
    {10, 16, 26, 18, 24, 16, 18, 22, 22, 26},
    {13, 22, 18, 26, 18, 24, 18, 22, 20, 24},
    {17, 28, 22, 16, 22, 28, 26, 26, 24, 28},
};
static const uint8_t BLOCKS[4][ESL_QR_VERSION_MAX] = {
    {1, 1, 1, 1, 1, 2, 2, 2, 2, 4},
    {1, 1, 1, 2, 2, 4, 4, 4, 5, 5},
    {1, 1, 2, 2, 4, 4, 6, 6, 8, 8},
    {1, 1, 2, 4, 4, 4, 5, 6, 8, 8},
};
/* Centre rows/columns of the alignment patterns, versions 2..10 (0 ends a list) */
static const uint8_t ALIGN[ESL_QR_VERSION_MAX - 1][3] = {
    {6, 18, 0}, {6, 22, 0}, {6, 26, 0}, {6, 30, 0}, {6, 34, 0},
    {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50},
};
/* The level as the format information writes it: L 01, M 00, Q 11, H 10 */
static const uint8_t ECC_FORMAT[4] = {1, 0, 3, 2};

/* Codewords in a version: the modules left for data, in bytes */
static unsigned total_codewords(unsigned v)
{
    unsigned n = (16 * v + 128) * v + 64; /* everything but the finders, timing and format */
    if (v >= 2) {
        unsigned a = v / 7 + 2;           /* alignment patterns per row */
        n -= (25 * a - 10) * a - 55;
    }
    if (v >= 7)
        n -= 36;                          /* version information */
    return n / 8;
}

static unsigned data_codewords(unsigned v, uint8_t ecc)
{
    return total_codewords(v) - (unsigned)EC_PER_BLOCK[ecc][v - 1] * BLOCKS[ecc][v - 1];
}

size_t esl_qr_capacity(uint8_t v, uint8_t ecc)
{
    if (v < 1 || v > ESL_QR_VERSION_MAX || ecc > ESL_QR_ECC_H)
        return 0;
    unsigned head = 4 + (v < 10 ? 8 : 16); /* mode, character count */
    return (data_codewords(v, ecc) * 8 - head) / 8;
}

uint8_t esl_qr_version(size_t len, uint8_t ecc, uint8_t max)
{
    if (max > ESL_QR_VERSION_MAX)
        max = ESL_QR_VERSION_MAX;
    for (uint8_t v = 1; v <= max; v++)
        if (len <= esl_qr_capacity(v, ecc))
            return v;
    return 0;
}

/* ---- Reed-Solomon over GF(256), x^8 + x^4 + x^3 + x^2 + 1 ---- */

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    unsigned r = 0;
    for (int i = 7; i >= 0; i--) {
        r = (r << 1) ^ ((r >> 7) * 0x11D);
        if ((b >> i) & 1)
            r ^= a;
    }
    return (uint8_t)r;
}

/* The n error correction codewords of data (len bytes) into ec */
static void rs_encode(const uint8_t *data, unsigned len, unsigned n, uint8_t *ec)
{
    uint8_t gen[31], root = 1; /* g(x) = (x - a^0)(x - a^1)..(x - a^(n-1)), leading 1 left out */
    unsigned i, j;

    memset(gen, 0, sizeof(gen));
    gen[n - 1] = 1; /* gen[0..n-1] are the coefficients of x^(n-1)..x^0, the polynomial 1 */
    for (i = 0; i < n; i++) {
        /* multiply by (x + root) */
        for (j = 0; j < n; j++) {
            gen[j] = gf_mul(gen[j], root);
            if (j + 1 < n)
                gen[j] ^= gen[j + 1];
        }
        root = gf_mul(root, 2);
    }
    memset(ec, 0, n);
    for (i = 0; i < len; i++) {
        uint8_t f = data[i] ^ ec[0];
        memmove(ec, ec + 1, n - 1);
        ec[n - 1] = 0;
        for (j = 0; j < n; j++)
            ec[j] ^= gf_mul(gen[j], f);
    }
}

/* ---- the matrix ---- */

static uint8_t mod[ESL_QR_SIZE_MAX][ESL_QR_SIZE_MAX]; /* bit0 dark, bit1 function pattern */
static uint8_t qsize;

static void set_fn(int x, int y, bool dark)
{
    if (x >= 0 && y >= 0 && x < qsize && y < qsize)
        mod[y][x] = (uint8_t)(2 | (dark ? 1 : 0));
}

static void finder(int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy, m = d > e ? d : e;
            set_fn(cx + dx, cy + dy, m != 2 && m != 4); /* the separator is the light ring at 4 */
        }
}

static void alignment(int cx, int cy)
{
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int d = dx < 0 ? -dx : dx, e = dy < 0 ? -dy : dy;
            set_fn(cx + dx, cy + dy, (d > e ? d : e) != 1);
        }
}

static void format_bits(uint8_t ecc, uint8_t mask)
{
    unsigned data = (unsigned)ECC_FORMAT[ecc] << 3 | mask, rem = data;
    int i, n = qsize;
    for (i = 0; i < 10; i++)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    unsigned bits = ((data << 10) | rem) ^ 0x5412;
#define BIT(i) (((bits >> (i)) & 1) != 0)
    /* around the top-left finder */
    for (i = 0; i <= 5; i++)
        set_fn(8, i, BIT(i));
    set_fn(8, 7, BIT(6));
    set_fn(8, 8, BIT(7));
    set_fn(7, 8, BIT(8));
    for (i = 9; i < 15; i++)
        set_fn(14 - i, 8, BIT(i));
    /* the copy by the other two finders */
    for (i = 0; i < 8; i++)
        set_fn(n - 1 - i, 8, BIT(i));
    for (i = 8; i < 15; i++)
        set_fn(8, n - 15 + i, BIT(i));
    set_fn(8, n - 8, true); /* always dark */
#undef BIT
}

static void version_bits(uint8_t v)
{
    unsigned rem = v;
    for (int i = 0; i < 12; i++)
        rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    unsigned bits = (unsigned)v << 12 | rem;
    for (int i = 0; i < 18; i++) {
        bool b = ((bits >> i) & 1) != 0;
        int a = qsize - 11 + i % 3, c = i / 3;
        set_fn(a, c, b);
        set_fn(c, a, b);
    }
}

static void function_patterns(uint8_t v, uint8_t ecc)
{
    int n = qsize, i, j;
    for (i = 0; i < n; i++) {
        set_fn(6, i, i % 2 == 0);
        set_fn(i, 6, i % 2 == 0);
    }
    finder(3, 3);
    finder(n - 4, 3);
    finder(3, n - 4);
    if (v >= 2) {
        const uint8_t *p = ALIGN[v - 2];
        for (i = 0; i < 3 && p[i]; i++)
            for (j = 0; j < 3 && p[j]; j++) {
                /* none on top of a finder: the first position with the first or the last */
                bool last_i = i == 2 || !p[i + 1], last_j = j == 2 || !p[j + 1];
                if (!((i == 0 && j == 0) || (i == 0 && last_j) || (last_i && j == 0)))
                    alignment(p[i], p[j]);
            }
    }
    format_bits(ecc, 0); /* reserves the area; written again with the mask */
    if (v >= 7)
        version_bits(v);
}

static void place_data(const uint8_t *cw, unsigned n)
{
    unsigned i = 0, total = n * 8;
    for (int right = qsize - 1; right >= 1; right -= 2) {
        if (right == 6)
            right = 5; /* the vertical timing pattern */
        bool up = ((right + 1) & 2) == 0;
        for (int vert = 0; vert < qsize; vert++)
            for (int k = 0; k < 2; k++) {
                int x = right - k, y = up ? qsize - 1 - vert : vert;
                if (mod[y][x] & 2)
                    continue;
                if (i < total && ((cw[i >> 3] >> (7 - (i & 7))) & 1))
                    mod[y][x] = 1;
                i++;
            }
    }
}

static bool mask_bit(uint8_t m, int x, int y)
{
    switch (m) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

static void apply_mask(uint8_t m)
{
    for (int y = 0; y < qsize; y++)
        for (int x = 0; x < qsize; x++)
            if (!(mod[y][x] & 2) && mask_bit(m, x, y))
                mod[y][x] ^= 1;
}

static bool dark(int x, int y)
{
    return x >= 0 && y >= 0 && x < qsize && y < qsize && (mod[y][x] & 1);
}

/* The standard's penalty for the code as it is now: runs of 5 or more, 2x2 blocks, finder
 * look-alikes, and the share of dark modules away from half */
static unsigned penalty(void)
{
    unsigned p = 0, n_dark = 0;
    int n = qsize;
    for (int pass = 0; pass < 2; pass++)
        for (int a = 0; a < n; a++) {
            int run = 0;
            bool last = false;
            for (int b = 0; b < n; b++) {
                bool d = pass ? dark(a, b) : dark(b, a);
                if (b && d == last) {
                    run++;
                } else {
                    if (run >= 5)
                        p += 3 + (unsigned)(run - 5);
                    run = 1;
                    last = d;
                }
                /* 1:1:3:1:1 with four light modules on one side */
                if (b + 7 <= n) {
                    static const bool F[7] = {1, 0, 1, 1, 1, 0, 1};
                    bool match = true, before = true, after = true;
                    for (int k = 0; k < 7 && match; k++)
                        match = (pass ? dark(a, b + k) : dark(b + k, a)) == F[k];
                    for (int k = 1; k <= 4 && match; k++) {
                        before = before && !(pass ? dark(a, b - k) : dark(b - k, a));
                        after = after && !(pass ? dark(a, b + 6 + k) : dark(b + 6 + k, a));
                    }
                    if (match && (before || after))
                        p += 40;
                }
            }
            if (run >= 5)
                p += 3 + (unsigned)(run - 5);
        }
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            n_dark += dark(x, y);
            if (x + 1 < n && y + 1 < n && dark(x, y) == dark(x + 1, y) && dark(x, y) == dark(x, y + 1) &&
                dark(x, y) == dark(x + 1, y + 1))
                p += 3;
        }
    unsigned total = (unsigned)(n * n);
    unsigned dev = n_dark * 20 > total * 10 ? n_dark * 20 - total * 10 : total * 10 - n_dark * 20;
    return p + 10 * (dev / total);
}

uint8_t esl_qr_encode(const uint8_t *text, size_t len, uint8_t ecc, uint8_t max, uint8_t bits[ESL_QR_BYTES])
{
    static uint8_t data[400], out[400], ec[31];
    uint8_t v = ecc <= ESL_QR_ECC_H ? esl_qr_version(len, ecc, max) : 0;
    unsigned i, pos = 0, bit = 0;

    if (!v)
        return 0;
    unsigned nd = data_codewords(v, ecc), nt = total_codewords(v);
    unsigned cc = v < 10 ? 8 : 16;

    /* the bit stream: mode 0100 (byte), count, the bytes, terminator, padding */
    memset(data, 0, sizeof(data));
#define PUT(val, nbits)                                                       \
    for (int b_ = (nbits) - 1; b_ >= 0; b_--, bit++)                          \
        if (((val) >> b_) & 1)                                                \
            data[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
    PUT(4u, 4);
    PUT((unsigned)len, (int)cc);
    for (i = 0; i < len; i++)
        PUT(text[i], 8);
#undef PUT
    bit += 4;                 /* terminator (at most what is left) */
    pos = (bit + 7) / 8;
    if (pos > nd)
        pos = nd;
    for (i = 0; pos < nd; pos++, i++)
        data[pos] = (i & 1) ? 0x11 : 0xEC;

    /* split into blocks, error correction per block, interleave */
    unsigned nb = BLOCKS[ecc][v - 1], ne = EC_PER_BLOCK[ecc][v - 1];
    unsigned short_len = nt / nb, nlong = nt % nb, nshort = nb - nlong;
    unsigned dshort = short_len - ne;
    unsigned o = 0;
    for (i = 0; i <= dshort; i++) {
        unsigned start = 0;
        for (unsigned b = 0; b < nb; b++) {
            unsigned dl = dshort + (b >= nshort);
            if (i < dl)
                out[o++] = data[start + i];
            start += dl;
        }
    }
    for (unsigned b = 0, start = 0; b < nb; b++) {
        unsigned dl = dshort + (b >= nshort);
        rs_encode(data + start, dl, ne, ec);
        for (i = 0; i < ne; i++)
            out[nd + i * nb + b] = ec[i];
        start += dl;
    }
    o = nt;

    /* the matrix, with the mask the standard's penalty likes best */
    qsize = (uint8_t)ESL_QR_SIZE(v);
    memset(mod, 0, sizeof(mod));
    function_patterns(v, ecc);
    place_data(out, o);
    uint8_t best = 0;
    unsigned best_p = ~0u;
    for (uint8_t m = 0; m < 8; m++) {
        apply_mask(m);
        format_bits(ecc, m);
        unsigned p = penalty();
        if (p < best_p) {
            best_p = p;
            best = m;
        }
        apply_mask(m); /* undo */
    }
    apply_mask(best);
    format_bits(ecc, best);

    memset(bits, 0, ESL_QR_BYTES);
    for (unsigned y = 0; y < qsize; y++)
        for (unsigned x = 0; x < qsize; x++)
            if (mod[y][x] & 1) {
                unsigned k = y * qsize + x;
                bits[k >> 3] |= (uint8_t)(0x80 >> (k & 7));
            }
    return qsize;
}
