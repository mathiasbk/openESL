#include "esl_png.h"
#include "esl_crc32.h"
#include <stdlib.h>
#include <string.h>

/* Written from the specifications (esl_png.h): the PNG chunk layout, the row filters
 * and colour types; the zlib wrapper; deflate's stored, fixed and dynamic blocks with
 * canonical Huffman codes. The deflate data is pulled out of the IDAT chunks a byte at
 * a time, inflated into the window and handed to the rows as it comes. */

const uint8_t esl_png_signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

#define IN_BUF   512
#define MAX_BITS 15

typedef struct {
    uint16_t count[MAX_BITS + 1]; /* codes of each length */
    uint16_t sym[288];            /* symbols by code, shortest codes first */
} huff_t;

typedef struct {
    const esl_png_io_t *io;
    esl_png_err_t err;

    /* the chunk being read */
    uint32_t crc;
    uint32_t left;       /* IDAT bytes not read yet */
    uint8_t in[IN_BUF];
    size_t in_pos, in_len;

    /* header */
    uint32_t w, h;
    uint8_t depth, type, channels;
    uint8_t plte[256][3];
    uint16_t plte_n;
    uint8_t trns[256];   /* alpha per palette entry */
    uint16_t trns_n;
    bool has_key;        /* tRNS for grey or RGB: the one transparent value */
    uint16_t key[3];

    /* inflate */
    uint32_t bits;
    unsigned nbits;
    uint8_t *win;
    uint32_t win_mask;
    size_t out;          /* bytes inflated so far */
    uint32_t adler_a, adler_b;
    huff_t lit, dist, lens; /* lens: the code for the code lengths of a dynamic block */
    bool fixed_built;    /* lit and dist hold the fixed codes */

    /* rows */
    size_t rowbytes;     /* one row without its filter byte */
    unsigned bpp;        /* bytes per pixel for the filters, at least 1 */
    uint8_t *mem;        /* the rows below, in one block */
    uint8_t *cur, *prev, *rgb;
    size_t col;          /* bytes of the current row so far, its filter byte included */
    uint8_t filter;
    uint32_t y;
} dec_t;

static void fail(dec_t *d, esl_png_err_t err)
{
    if (!d->err)
        d->err = err;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool get(dec_t *d, uint8_t *buf, size_t n)
{
    if (d->err)
        return false;
    if (!d->io->read(d->io->ctx, buf, n)) {
        fail(d, ESL_PNG_ERR_READ);
        return false;
    }
    return true;
}

/* ---- chunks ------------------------------------------------------------------- */

static bool chunk_head(dec_t *d, uint32_t *len, uint8_t type[4])
{
    uint8_t b[8];
    if (!get(d, b, 8))
        return false;
    *len = be32(b);
    memcpy(type, b + 4, 4);
    if (*len > 0x7FFFFFFFu) {
        fail(d, ESL_PNG_ERR_BAD);
        return false;
    }
    d->crc = esl_crc32_update(0, type, 4);
    return true;
}

static bool chunk_data(dec_t *d, uint8_t *buf, size_t n)
{
    if (!get(d, buf, n))
        return false;
    d->crc = esl_crc32_update(d->crc, buf, n);
    return true;
}

static bool chunk_end(dec_t *d)
{
    uint8_t b[4];
    if (!get(d, b, 4))
        return false;
    if (be32(b) != d->crc) {
        fail(d, ESL_PNG_ERR_BAD);
        return false;
    }
    return true;
}

static bool chunk_skip(dec_t *d, uint32_t len)
{
    while (len) {
        size_t n = len < IN_BUF ? len : IN_BUF;
        if (!chunk_data(d, d->in, n))
            return false;
        len -= (uint32_t)n;
    }
    return chunk_end(d);
}

/* The next byte of the zlib stream, across IDAT chunks; 0 after an error. */
static uint8_t idat_byte(dec_t *d)
{
    while (d->in_pos == d->in_len) {
        if (d->err)
            return 0;
        if (!d->left) {
            uint32_t len;
            uint8_t type[4];
            if (!chunk_end(d) || !chunk_head(d, &len, type))
                return 0;
            if (memcmp(type, "IDAT", 4) != 0) {
                fail(d, ESL_PNG_ERR_BAD); /* the image data ends early */
                return 0;
            }
            d->left = len;
            continue;
        }
        size_t n = d->left < IN_BUF ? d->left : IN_BUF;
        if (!chunk_data(d, d->in, n))
            return 0;
        d->left -= (uint32_t)n;
        d->in_pos = 0;
        d->in_len = n;
    }
    return d->in[d->in_pos++];
}

/* ---- rows --------------------------------------------------------------------- */

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = (int)a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

/* Sample i of the row (0..channels * w), as it is in the file */
static unsigned sample(const dec_t *d, size_t i)
{
    if (d->depth == 16)
        return (unsigned)d->cur[2 * i] << 8 | d->cur[2 * i + 1];
    if (d->depth == 8)
        return d->cur[i];
    size_t bit = i * d->depth;
    return (d->cur[bit / 8] >> (8 - d->depth - bit % 8)) & ((1u << d->depth) - 1);
}

/* A sample scaled to 8 bits */
static uint8_t to8(const dec_t *d, unsigned v)
{
    if (d->depth == 16)
        return (uint8_t)(v >> 8);
    if (d->depth == 8)
        return (uint8_t)v;
    return (uint8_t)(v * 255 / ((1u << d->depth) - 1));
}

static uint8_t over_white(uint8_t c, uint8_t a)
{
    return (uint8_t)(((unsigned)c * a + 255u * (255u - a) + 127) / 255);
}

static void row_done(dec_t *d)
{
    for (uint32_t x = 0; x < d->w && !d->err; x++) {
        size_t s = (size_t)x * d->channels;
        unsigned v[4] = {0, 0, 0, 0};
        for (unsigned c = 0; c < d->channels; c++)
            v[c] = sample(d, s + c);
        uint8_t r, g, b, a = 255;
        switch (d->type) {
        case 0: /* grey */
            r = g = b = to8(d, v[0]);
            if (d->has_key && v[0] == d->key[0])
                a = 0;
            break;
        case 2: /* RGB */
            r = to8(d, v[0]);
            g = to8(d, v[1]);
            b = to8(d, v[2]);
            if (d->has_key && v[0] == d->key[0] && v[1] == d->key[1] && v[2] == d->key[2])
                a = 0;
            break;
        case 3: /* palette */
            if (v[0] >= d->plte_n) {
                fail(d, ESL_PNG_ERR_BAD);
                return;
            }
            r = d->plte[v[0]][0];
            g = d->plte[v[0]][1];
            b = d->plte[v[0]][2];
            if (v[0] < d->trns_n)
                a = d->trns[v[0]];
            break;
        case 4: /* grey and alpha */
            r = g = b = to8(d, v[0]);
            a = to8(d, v[1]);
            break;
        default: /* 6: RGB and alpha */
            r = to8(d, v[0]);
            g = to8(d, v[1]);
            b = to8(d, v[2]);
            a = to8(d, v[3]);
            break;
        }
        uint8_t *o = d->rgb + 3 * x;
        o[0] = over_white(r, a);
        o[1] = over_white(g, a);
        o[2] = over_white(b, a);
    }
    if (d->err)
        return;
    if (!d->io->row(d->io->ctx, d->y, d->rgb)) {
        fail(d, ESL_PNG_ERR_STOPPED);
        return;
    }
    uint8_t *t = d->prev;
    d->prev = d->cur;
    d->cur = t;
    d->col = 0;
    d->y++;
}

/* One inflated byte: the filter type of a row, or the next byte of it, unfiltered as
   it comes (a filter needs only the bytes before it in the row and the row above).
   Bytes after the last row are an error (a stream that inflates far past the
   picture is a zip bomb, not a picture). */
static void row_byte(dec_t *d, uint8_t v)
{
    if (d->y >= d->h) {
        fail(d, ESL_PNG_ERR_BAD);
        return;
    }
    if (d->col == 0) {
        if (v > 4)
            fail(d, ESL_PNG_ERR_BAD);
        d->filter = v;
        d->col = 1;
        return;
    }
    size_t i = d->col - 1;
    uint8_t a = i >= d->bpp ? d->cur[i - d->bpp] : 0, b = d->prev[i], c = i >= d->bpp ? d->prev[i - d->bpp] : 0;
    switch (d->filter) {
    case 1: v = (uint8_t)(v + a); break;
    case 2: v = (uint8_t)(v + b); break;
    case 3: v = (uint8_t)(v + ((unsigned)a + b) / 2); break;
    case 4: v = (uint8_t)(v + paeth(a, b, c)); break;
    default: break;
    }
    d->cur[i] = v;
    if (++d->col == d->rowbytes + 1)
        row_done(d);
}

/* ---- inflate ------------------------------------------------------------------ */

static unsigned bits(dec_t *d, unsigned n)
{
    while (d->nbits < n) {
        d->bits |= (uint32_t)idat_byte(d) << d->nbits;
        d->nbits += 8;
        if (d->err)
            return 0;
    }
    unsigned v = d->bits & ((1u << n) - 1);
    d->bits >>= n;
    d->nbits -= n;
    return v;
}

static void put(dec_t *d, uint8_t v)
{
    d->win[d->out & d->win_mask] = v;
    d->out++;
    d->adler_a += v;
    if (d->adler_a >= 65521)
        d->adler_a -= 65521;
    d->adler_b += d->adler_a;
    if (d->adler_b >= 65521)
        d->adler_b -= 65521;
    row_byte(d, v);
}

/* The canonical code for the code lengths len[0..n): codes of one length are
   consecutive and in symbol order, shorter codes before longer ones (RFC 1951 3.2.2).
   An incomplete code is allowed (a single distance code), too many codes are not. */
static bool huff_build(huff_t *h, const uint8_t *len, unsigned n)
{
    uint16_t next[MAX_BITS + 2];
    memset(h->count, 0, sizeof(h->count));
    for (unsigned i = 0; i < n; i++)
        h->count[len[i]]++;
    h->count[0] = 0;
    int room = 1;
    for (unsigned l = 1; l <= MAX_BITS; l++) {
        room = 2 * room - h->count[l];
        if (room < 0)
            return false;
    }
    next[1] = 0;
    for (unsigned l = 1; l <= MAX_BITS; l++)
        next[l + 1] = (uint16_t)(next[l] + h->count[l]);
    for (unsigned i = 0; i < n; i++)
        if (len[i])
            h->sym[next[len[i]]++] = (uint16_t)i;
    return true;
}

/* The next symbol: the code is read a bit at a time, most significant bit first, and
   compared with the first code of each length. -1 after an error. */
static int huff_sym(dec_t *d, const huff_t *h)
{
    unsigned code = 0, first = 0, index = 0;
    for (unsigned l = 1; l <= MAX_BITS; l++) {
        code |= bits(d, 1);
        if (d->err)
            return -1;
        unsigned n = h->count[l];
        if (code - first < n)
            return h->sym[index + code - first];
        index += n;
        first = (first + n) << 1;
        code <<= 1;
    }
    fail(d, ESL_PNG_ERR_BAD); /* a code the table does not have */
    return -1;
}

static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* The symbols of one block, up to its end code */
static void codes(dec_t *d)
{
    for (;;) {
        int sym = huff_sym(d, &d->lit);
        if (sym < 0)
            return;
        if (sym < 256) {
            put(d, (uint8_t)sym);
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) {
                fail(d, ESL_PNG_ERR_BAD);
                return;
            }
            unsigned len = len_base[sym] + bits(d, len_extra[sym]);
            int ds = huff_sym(d, &d->dist);
            if (ds < 0)
                return;
            if (ds >= 30) {
                fail(d, ESL_PNG_ERR_BAD);
                return;
            }
            size_t dist = dist_base[ds] + bits(d, dist_extra[ds]);
            if (d->err)
                return;
            if (dist > d->out || dist > d->win_mask + 1u) {
                fail(d, ESL_PNG_ERR_BAD);
                return;
            }
            while (len-- && !d->err)
                put(d, d->win[(d->out - dist) & d->win_mask]);
        }
        if (d->err)
            return;
    }
}

static void stored(dec_t *d)
{
    d->bits = 0; /* to the next byte: fewer than 8 bits are left over */
    d->nbits = 0;
    uint8_t b[4];
    for (int i = 0; i < 4; i++)
        b[i] = idat_byte(d);
    unsigned len = b[0] | (unsigned)b[1] << 8, nlen = b[2] | (unsigned)b[3] << 8;
    if (d->err)
        return;
    if (len != (~nlen & 0xFFFFu)) {
        fail(d, ESL_PNG_ERR_BAD);
        return;
    }
    while (len-- && !d->err)
        put(d, idat_byte(d));
}

static void fixed(dec_t *d)
{
    if (d->fixed_built)
        return;
    d->fixed_built = true;
    uint8_t len[288];
    unsigned i = 0;
    for (; i < 144; i++)
        len[i] = 8;
    for (; i < 256; i++)
        len[i] = 9;
    for (; i < 280; i++)
        len[i] = 7;
    for (; i < 288; i++)
        len[i] = 8;
    huff_build(&d->lit, len, 288);
    memset(len, 5, 30);
    huff_build(&d->dist, len, 30);
}

static bool dynamic(dec_t *d)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    unsigned nlit = bits(d, 5) + 257, ndist = bits(d, 5) + 1, ncode = bits(d, 4) + 4;
    uint8_t len[286 + 30];
    if (d->err)
        return false;
    if (nlit > 286 || ndist > 30) {
        fail(d, ESL_PNG_ERR_BAD);
        return false;
    }
    memset(len, 0, sizeof(len));
    for (unsigned i = 0; i < ncode; i++)
        len[order[i]] = (uint8_t)bits(d, 3);
    if (d->err)
        return false;
    if (!huff_build(&d->lens, len, 19)) {
        fail(d, ESL_PNG_ERR_BAD);
        return false;
    }
    memset(len, 0, sizeof(len));
    for (unsigned i = 0; i < nlit + ndist;) {
        int sym = huff_sym(d, &d->lens);
        if (sym < 0)
            return false;
        if (sym < 16) {
            len[i++] = (uint8_t)sym;
            continue;
        }
        uint8_t v = 0;
        unsigned rep;
        if (sym == 16) {
            if (i == 0) {
                fail(d, ESL_PNG_ERR_BAD);
                return false;
            }
            v = len[i - 1];
            rep = 3 + bits(d, 2);
        } else if (sym == 17) {
            rep = 3 + bits(d, 3);
        } else {
            rep = 11 + bits(d, 7);
        }
        if (d->err)
            return false;
        if (i + rep > nlit + ndist) {
            fail(d, ESL_PNG_ERR_BAD);
            return false;
        }
        while (rep--)
            len[i++] = v;
    }
    if (!len[256] || !huff_build(&d->lit, len, nlit) || !huff_build(&d->dist, len + nlit, ndist)) {
        fail(d, ESL_PNG_ERR_BAD);
        return false;
    }
    return true;
}

/* The zlib stream in the IDAT chunks, the first of which has just begun */
static void inflate_idat(dec_t *d)
{
    uint8_t cmf = idat_byte(d), flg = idat_byte(d);
    if (d->err)
        return;
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((unsigned)cmf << 8 | flg) % 31 || (flg & 0x20)) {
        fail(d, ESL_PNG_ERR_BAD);
        return;
    }
    /* the window the encoder used, never more than 32 KB */
    size_t win = (size_t)1 << ((cmf >> 4) + 8);
    d->win = malloc(win);
    if (!d->win) {
        fail(d, ESL_PNG_ERR_NOMEM);
        return;
    }
    d->win_mask = (uint32_t)(win - 1);
    d->adler_a = 1;
    d->adler_b = 0;
    bool last;
    do {
        last = bits(d, 1);
        unsigned type = bits(d, 2);
        if (d->err)
            return;
        if (type == 0)
            stored(d);
        else if (type == 1) {
            fixed(d);
            codes(d);
        } else if (type == 2) {
            d->fixed_built = false;
            if (dynamic(d))
                codes(d);
        } else
            fail(d, ESL_PNG_ERR_BAD);
    } while (!last && !d->err);
    d->bits = 0;
    d->nbits = 0;
    uint8_t b[4];
    for (int i = 0; i < 4; i++)
        b[i] = idat_byte(d);
    if (!d->err && be32(b) != (d->adler_b << 16 | d->adler_a))
        fail(d, ESL_PNG_ERR_BAD);
}

/* ---- the file ------------------------------------------------------------------ */

static bool ihdr(dec_t *d, uint32_t len)
{
    uint8_t b[13];
    if (len != 13) {
        fail(d, ESL_PNG_ERR_BAD);
        return false;
    }
    if (!chunk_data(d, b, 13) || !chunk_end(d))
        return false;
    d->w = be32(b);
    d->h = be32(b + 4);
    d->depth = b[8];
    d->type = b[9];
    if (!d->w || !d->h || d->w > 0x7FFFFFFFu || d->h > 0x7FFFFFFFu || b[10] || b[11] || b[12] > 1) {
        fail(d, ESL_PNG_ERR_BAD);
        return false;
    }
    unsigned dp = d->depth;
    bool ok;
    switch (d->type) {
    case 0: ok = dp == 1 || dp == 2 || dp == 4 || dp == 8 || dp == 16; d->channels = 1; break;
    case 3: ok = dp == 1 || dp == 2 || dp == 4 || dp == 8; d->channels = 1; break;
    case 2: ok = dp == 8 || dp == 16; d->channels = 3; break;
    case 4: ok = dp == 8 || dp == 16; d->channels = 2; break;
    case 6: ok = dp == 8 || dp == 16; d->channels = 4; break;
    default: ok = false; break;
    }
    if (!ok || b[12]) {
        fail(d, ESL_PNG_ERR_UNSUPPORTED);
        return false;
    }
    if (d->w > ESL_PNG_MAX_WIDTH || !d->io->header(d->io->ctx, d->w, d->h)) {
        fail(d, ESL_PNG_ERR_SIZE);
        return false;
    }
    unsigned bpx = (unsigned)d->channels * d->depth;
    d->rowbytes = ((size_t)d->w * bpx + 7) / 8;
    d->bpp = bpx >= 8 ? bpx / 8 : 1;
    d->mem = calloc(1, 2 * d->rowbytes + 3 * (size_t)d->w);
    if (!d->mem) {
        fail(d, ESL_PNG_ERR_NOMEM);
        return false;
    }
    d->cur = d->mem;
    d->prev = d->cur + d->rowbytes;
    d->rgb = d->prev + d->rowbytes;
    return true;
}

static bool plte(dec_t *d, uint32_t len)
{
    if (len % 3 || len == 0 || len > 768 || d->plte_n) {
        fail(d, ESL_PNG_ERR_BAD);
        return false;
    }
    d->plte_n = (uint16_t)(len / 3);
    return chunk_data(d, &d->plte[0][0], len) && chunk_end(d);
}

static bool trns(dec_t *d, uint32_t len)
{
    uint8_t b[6];
    if (d->type == 3) {
        if (len > d->plte_n) {
            fail(d, ESL_PNG_ERR_BAD);
            return false;
        }
        d->trns_n = (uint16_t)len;
        return chunk_data(d, d->trns, len) && chunk_end(d);
    }
    unsigned n = d->type == 0 ? 1 : d->type == 2 ? 3 : 0;
    if (!n) /* not allowed with an alpha channel: ignored */
        return chunk_skip(d, len);
    if (len != 2 * n) {
        fail(d, ESL_PNG_ERR_BAD);
        return false;
    }
    if (!chunk_data(d, b, len) || !chunk_end(d))
        return false;
    for (unsigned i = 0; i < n; i++)
        d->key[i] = (uint16_t)(b[2 * i] << 8 | b[2 * i + 1]);
    d->has_key = true;
    return true;
}

esl_png_err_t esl_png_decode(const esl_png_io_t *io)
{
    dec_t *d = calloc(1, sizeof(dec_t));
    if (!d)
        return ESL_PNG_ERR_NOMEM;
    d->io = io;
    uint8_t sig[8];
    if (get(d, sig, 8) && memcmp(sig, esl_png_signature, 8) != 0)
        fail(d, ESL_PNG_ERR_NOT_PNG);
    bool seen_ihdr = false, seen_idat = false, done = false;
    while (!d->err && !done) {
        uint32_t len;
        uint8_t type[4];
        if (!chunk_head(d, &len, type))
            break;
        bool is_ihdr = memcmp(type, "IHDR", 4) == 0;
        if (is_ihdr != !seen_ihdr) {
            fail(d, ESL_PNG_ERR_BAD); /* IHDR first, and only once */
            break;
        }
        if (is_ihdr) {
            seen_ihdr = ihdr(d, len);
        } else if (memcmp(type, "IEND", 4) == 0) {
            done = true;
            if (!seen_idat || d->y < d->h)
                fail(d, ESL_PNG_ERR_BAD);
            else
                chunk_skip(d, len);
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (seen_idat) { /* after the zlib stream: nothing more to read from it */
                chunk_skip(d, len);
                continue;
            }
            if (d->type == 3 && !d->plte_n) {
                fail(d, ESL_PNG_ERR_BAD);
                break;
            }
            seen_idat = true;
            d->left = len;
            inflate_idat(d);
            if (d->err)
                break;
            if (d->y < d->h) {
                fail(d, ESL_PNG_ERR_BAD); /* fewer rows than the header said */
                break;
            }
            /* the rest of this chunk, after the stream */
            d->in_pos = d->in_len = 0;
            chunk_skip(d, d->left);
        } else if (memcmp(type, "PLTE", 4) == 0 && !seen_idat) {
            plte(d, len);
        } else if (memcmp(type, "tRNS", 4) == 0 && !seen_idat) {
            trns(d, len);
        } else if (!(type[0] & 0x20)) {
            fail(d, ESL_PNG_ERR_UNSUPPORTED); /* a critical chunk it does not know */
        } else {
            chunk_skip(d, len);
        }
    }
    esl_png_err_t err = d->err;
    free(d->win);
    free(d->mem);
    free(d);
    return err;
}

const char *esl_png_err_text(esl_png_err_t err)
{
    switch (err) {
    case ESL_PNG_OK:              return "ok";
    case ESL_PNG_ERR_READ:        return "the body ended early";
    case ESL_PNG_ERR_NOT_PNG:     return "not a PNG file";
    case ESL_PNG_ERR_BAD:         return "a broken PNG file";
    case ESL_PNG_ERR_UNSUPPORTED: return "an interlaced PNG, or one with a colour type or bit depth PNG does not have";
    case ESL_PNG_ERR_SIZE:        return "the picture is not the size of the tag's screen";
    case ESL_PNG_ERR_NOMEM:       return "out of memory";
    default:                      return "stopped";
    }
}
