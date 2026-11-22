#include "tinytest.h"
#include "esl_content.h"
#include "esl_layout.h"
#include "esl_render.h"
#include "esl_solum_stock.h"
#include "esl_store.h"
#include <stdlib.h>
#include <string.h>

/* The SoluM stock-firmware core (esl_solum_stock.c): the vendor's frames, the vendor's
 * 1-bit BMP behind a page block, and the zlib stream in front of it. The deflate output
 * is checked with a second, independent inflate (below), so a bug would have to be in
 * both to pass. */

/* ---- a small reference inflate (stored and fixed Huffman) -------------------- */

typedef struct {
    const uint8_t *in;
    size_t len, pos;
    uint32_t bits;
    unsigned nbits;
} br_t;

static unsigned br_bits(br_t *r, unsigned n)
{
    while (r->nbits < n) {
        r->bits |= (uint32_t)r->in[r->pos++] << r->nbits;
        r->nbits += 8;
    }
    unsigned v = r->bits & ((1u << n) - 1);
    r->bits >>= n;
    r->nbits -= n;
    return v;
}

typedef struct {
    uint16_t count[16];
    uint16_t sym[288];
} rhuff_t;

static void rhuff_build(rhuff_t *h, const uint8_t *len, unsigned n)
{
    memset(h->count, 0, sizeof h->count);
    for (unsigned i = 0; i < n; i++)
        h->count[len[i]]++;
    h->count[0] = 0;
    uint16_t off[16];
    off[1] = 0;
    for (unsigned l = 1; l < 15; l++)
        off[l + 1] = (uint16_t)(off[l] + h->count[l]);
    for (unsigned i = 0; i < n; i++)
        if (len[i])
            h->sym[off[len[i]]++] = (uint16_t)i;
}

static int rhuff_sym(br_t *r, const rhuff_t *h)
{
    unsigned code = 0, first = 0, index = 0;
    for (unsigned l = 1; l <= 15; l++) {
        code |= br_bits(r, 1);
        unsigned cnt = h->count[l];
        if (code - first < cnt)
            return h->sym[index + code - first];
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t r_len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t r_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t r_dist_base[30] = {1,   2,   3,   4,   5,    7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t r_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* Inflates a zlib stream (stored and fixed blocks) into out; its length, -1 on error. */
static long rinflate(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    br_t r = {in, len, 0, 0, 0};
    unsigned cmf = br_bits(&r, 8), flg = br_bits(&r, 8);
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((cmf << 8 | flg) % 31) || (flg & 0x20))
        return -1;
    size_t o = 0;
    bool last = false;
    while (!last) {
        last = br_bits(&r, 1) != 0;
        unsigned type = br_bits(&r, 2);
        if (type == 0) {
            r.bits = 0;
            r.nbits = 0;
            unsigned l = br_bits(&r, 16), nl = br_bits(&r, 16);
            if ((l ^ 0xFFFFu) != nl)
                return -1;
            while (l--)
                out[o++] = (uint8_t)br_bits(&r, 8);
        } else if (type == 1) {
            uint8_t lenv[288];
            unsigned i = 0;
            for (; i < 144; i++) lenv[i] = 8;
            for (; i < 256; i++) lenv[i] = 9;
            for (; i < 280; i++) lenv[i] = 7;
            for (; i < 288; i++) lenv[i] = 8;
            rhuff_t lit, dist;
            rhuff_build(&lit, lenv, 288);
            uint8_t dl[30];
            memset(dl, 5, sizeof dl);
            rhuff_build(&dist, dl, 30);
            for (;;) {
                int s = rhuff_sym(&r, &lit);
                if (s < 0)
                    return -1;
                if (s < 256) {
                    out[o++] = (uint8_t)s;
                } else if (s == 256) {
                    break;
                } else {
                    s -= 257;
                    unsigned l2 = r_len_base[s] + br_bits(&r, r_len_extra[s]);
                    int ds = rhuff_sym(&r, &dist);
                    if (ds < 0)
                        return -1;
                    unsigned d = r_dist_base[ds] + br_bits(&r, r_dist_extra[ds]);
                    if (d > o)
                        return -1;
                    while (l2--)
                        out[o] = out[o - d], o++;
                }
                if (o > cap)
                    return -1;
            }
        } else {
            return -1; /* the encoder never emits dynamic blocks */
        }
    }
    return (long)o;
}

/* ---- helpers ---- */

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static const uint8_t GW[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
static const uint64_t TAG = 0x1122334455667788ull;

static void test_solum_stock_models(void)
{
    CHECK_INT(esl_solum_stock_model_to_esl(ESL_SOLUM_STOCK_MODEL_154), ESL_MODEL_154);
    CHECK_INT(esl_solum_stock_model_to_esl(ESL_SOLUM_STOCK_MODEL_29), ESL_MODEL_29);
    CHECK_INT(esl_solum_stock_model_to_esl(0x00), ESL_MODEL_UNKNOWN);
    CHECK_INT(esl_solum_stock_model_from_esl(ESL_MODEL_154), ESL_SOLUM_STOCK_MODEL_154);
    CHECK_INT(esl_solum_stock_model_from_esl(ESL_MODEL_29), ESL_SOLUM_STOCK_MODEL_29);
    CHECK_INT(esl_solum_stock_model_from_esl(ESL_MODEL_UNKNOWN), 0);
}

static void test_solum_stock_deflate(void)
{
    void *scratch = malloc(esl_solum_stock_mem_size());
    uint8_t in[4096], out[8192], back[4096];
    CHECK(scratch != NULL);

    /* runs, a ramp, and pseudo-random bytes: something for LZ77 and something not */
    for (size_t i = 0; i < sizeof in; i++)
        in[i] = (uint8_t)(i < 2048 ? (i % 7 ? 0xFF : 0x00) : (i * 2654435761u >> 13));
    size_t z = esl_solum_stock_deflate(scratch, in, sizeof in, out, sizeof out);
    CHECK(z > 0);
    CHECK(out[0] == 0x28 && out[1] == 0x15); /* the zlib header for a 1 KB window */
    long n = rinflate(out, z, back, sizeof back);
    CHECK_INT(n, (long)sizeof in);
    CHECK(memcmp(in, back, sizeof in) == 0);
    CHECK(z < sizeof in); /* the runs really compressed */

    /* a buffer that is over the BMP size is refused */
    CHECK_INT(esl_solum_stock_deflate(scratch, in, ESL_SOLUM_STOCK_BMP_MAX + 1, out, sizeof out), 0);
    free(scratch);
}

/* Rebuilds the BMP the way the tag sees it, from the render image, as a second
 * implementation: 1.54" mirrored, 2.9" transposed, 1 = white. */
static void expect_bmp(uint8_t *bmp, uint8_t model, const uint8_t *render)
{
    uint8_t *px = bmp + 62; /* past the 62-byte header */
    if (model == ESL_MODEL_154) {
        memset(px, 0xFF, 200 * 28);
        for (int y = 0; y < 200; y++)
            for (int x = 0; x < 200; x++)
                if (render[x * 25 + y / 8] & (0x80 >> (y % 8))) {
                    int c = 199 - x;
                    px[y * 28 + c / 8] &= (uint8_t)~(0x80 >> (c % 8));
                }
    } else {
        for (int x = 0; x < 296; x++)
            for (int b = 0; b < 16; b++)
                px[x * 16 + b] = (uint8_t)~render[x * 16 + b];
    }
}

static void test_solum_stock_job(void)
{
    void *scratch = malloc(esl_solum_stock_mem_size());
    uint8_t render[ESL_SOLUM_STOCK_RENDER_MAX];
    uint8_t job[ESL_SOLUM_STOCK_JOB_MAX], back[ESL_SOLUM_STOCK_BMP_MAX], want[ESL_SOLUM_STOCK_BMP_MAX];
    CHECK(scratch != NULL);

    const uint8_t models[2] = {ESL_MODEL_154, ESL_MODEL_29};
    for (int m = 0; m < 2; m++) {
        uint8_t model = models[m];
        size_t rsize = esl_render_size(model);
        CHECK(rsize > 0 && rsize <= sizeof render);
        /* a pattern with runs and isolated ink */
        for (size_t i = 0; i < rsize; i++)
            render[i] = (uint8_t)(i * 37 >> 3 ^ (i % 5 == 0 ? 0xF0 : 0x00));
        size_t jl = esl_solum_stock_job(scratch, model, render, job, sizeof job);
        CHECK(jl > 17);
        CHECK_INT(job[0], 0x01);
        CHECK_INT(job[1], 0x02);
        CHECK_INT(job[6], 0xFF);
        uint16_t comp = (uint16_t)(job[2] << 8 | job[3]);
        uint16_t decomp = (uint16_t)(job[4] << 8 | job[5]);
        CHECK_INT(comp, jl - 17);
        CHECK_INT(decomp, (long)(model == ESL_MODEL_154 ? 5662 : 4798));
        uint8_t x = 0;
        for (size_t i = 0; i < comp; i++)
            x ^= job[17 + i];
        CHECK_INT(job[7], (long)(0xFF ^ x));
        uint16_t w = (uint16_t)(job[12] << 8 | job[13]);
        uint16_t h = (uint16_t)(job[14] << 8 | job[15]);
        CHECK_INT(w, model == ESL_MODEL_154 ? 152 : 128);
        CHECK_INT(h, model == ESL_MODEL_154 ? 152 : 296);
        CHECK_INT(job[16], 0x01);

        /* the stream the tag gets: head, then the job data */
        long bn = rinflate(job + 17, comp, back, sizeof back);
        CHECK_INT(bn, decomp);
        expect_bmp(want, model, render);
        CHECK(memcmp(back + 62, want + 62, decomp - 62) == 0);
        /* the BMP header SoluM's firmware expects */
        CHECK(back[0] == 'B' && back[1] == 'M');
        CHECK_INT(back[2] | back[3] << 8 | back[4] << 16 | back[5] << 24, decomp);
        CHECK_INT(back[10], 62);
        CHECK_INT(back[14], 40);
        CHECK_INT(back[18] | back[19] << 8, model == ESL_MODEL_154 ? 200 : 128);
        CHECK_INT(back[22] | back[23] << 8, model == ESL_MODEL_154 ? 200 : 296);
        CHECK_INT(back[28], 1);
    }

    /* an unknown model gives nothing */
    CHECK_INT(esl_solum_stock_job(scratch, ESL_MODEL_UNKNOWN, render, job, sizeof job), 0);
    free(scratch);
}

static void test_solum_stock_stream(void)
{
    void *scratch = malloc(esl_solum_stock_mem_size());
    uint8_t render[ESL_SOLUM_STOCK_RENDER_MAX];
    uint8_t out[ESL_SOLUM_STOCK_STREAM_MAX];
    CHECK(scratch != NULL);
    memset(render, 0, sizeof render); /* all white */
    size_t n = esl_solum_stock_stream(scratch, ESL_MODEL_29, render, 0x00000042, 20, out, sizeof out);
    CHECK(n > ESL_SOLUM_STOCK_STREAM_HEAD);
    CHECK_INT(out[0], 0x0B);
    CHECK_INT(out[1], 0xB8);
    CHECK_INT(out[2], 0x96);
    CHECK_INT(out[3], 0x19); /* the channel byte checked on channel 20 */
    CHECK_INT(out[4], 0x00);
    CHECK_INT(out[5], 0x00);
    CHECK_INT(out[6], 0x00);
    CHECK_INT(out[7], 0x42);
    CHECK_INT(out[8], 1);
    CHECK_INT(out[9], 1);
    CHECK_INT(out[10], 1);
    free(scratch);
}

static void test_solum_stock_frames(void)
{
    uint8_t tx[ESL_PSDU_MAX];
    uint8_t rx[ESL_PSDU_MAX];
    esl_solum_stock_frame_t f;

    /* SCAN_R: short dst = the tag's short address, long src = the AP */
    size_t n = esl_solum_stock_build_scan_reply(tx, 0x1234, 0x10AF, GW);
    CHECK_INT(n, 22);
    CHECK_INT(tx[0], 0x41);
    CHECK_INT(tx[1], 0xC8);
    CHECK_INT(tx[2], 0x00);
    CHECK_INT(tx[3], 0x34);
    CHECK_INT(tx[4], 0x12);
    CHECK_INT(tx[5], 0xAF);
    CHECK_INT(tx[6], 0x10);
    CHECK(memcmp(tx + 7, GW, 8) == 0);
    CHECK_INT(tx[15], 0xE1);

    /* JOIN_R: long dst = the tag, long src = the AP */
    n = esl_solum_stock_build_join_reply(tx, 0x1234, TAG, GW, 7);
    CHECK_INT(n, 32);
    CHECK_INT(tx[0], 0x61);
    CHECK_INT(tx[1], 0xCC);
    CHECK_INT(tx[2], 7);
    CHECK_INT(tx[3], 0x34);
    CHECK_INT(tx[4], 0x12);
    CHECK_INT(tx[5], 0x88); /* TAG little-endian on air */
    CHECK_INT(tx[12], 0x11);
    CHECK(memcmp(tx + 13, GW, 8) == 0);
    CHECK_INT(tx[21], 0xD8);

    /* NONE: DE, the job, then the fixed tail */
    n = esl_solum_stock_build_none(tx, 0x1234, TAG, GW, 0x00000003, 9);
    CHECK_INT(n, 37);
    CHECK_INT(tx[21], 0xDE);
    CHECK_INT(tx[22], 0x00);
    CHECK_INT(tx[25], 0x03);
    CHECK_INT(tx[26], 0x00);
    CHECK_INT(tx[27], 0x19);
    CHECK_INT(tx[36], 0x7C);

    /* IMG: the fragment header, then the data */
    uint8_t data[ESL_SOLUM_STOCK_FRAG_DATA];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (uint8_t)i;
    n = esl_solum_stock_build_img(tx, 0x1234, TAG, GW, 2, 3, 1, 0x088E, data, sizeof data);
    CHECK_INT(n, 21 + 7 + (int)sizeof data);
    CHECK_INT(tx[21], 0xAD);
    CHECK_INT(tx[22], 0x00);
    CHECK_INT(tx[23], 0x03); /* total */
    CHECK_INT(tx[24], 0x00);
    CHECK_INT(tx[25], 0x01); /* idx */
    CHECK_INT(tx[26], 0x08);
    CHECK_INT(tx[27], 0x8E); /* ts */
    CHECK(memcmp(tx + 28, data, sizeof data) == 0);
    CHECK_INT(esl_solum_stock_build_img(tx, 0x1234, TAG, GW, 2, 3, 1, 0, data, ESL_SOLUM_STOCK_FRAG_DATA + 1), 0);

    /* a broadcast SCAN from a short address */
    uint8_t scan[11] = {0x41, 0x88, 0x1C, 0x34, 0x12, 0xFF, 0xFF, 0xAF, 0x10, 0xE0, ESL_SOLUM_STOCK_MODEL_154};
    CHECK(esl_solum_stock_parse(scan, sizeof scan, &f));
    CHECK_INT(f.msg, ESL_SOLUM_STOCK_MSG_SCAN);
    CHECK_INT(f.seq, 0x1C);
    CHECK_INT(f.pan, 0x1234);
    CHECK_INT(f.short_addr, 0x10AF);
    CHECK_INT(f.model, ESL_SOLUM_STOCK_MODEL_154);
    CHECK_INT(f.mac, 0);

    /* a long POLL addressed to the AP */
    memcpy(rx, (uint8_t[]){0x61, 0xCC, 0x05, 0x34, 0x12}, 5);
    memcpy(rx + 5, GW, 8);
    put_le64(rx + 13, TAG);
    uint8_t *pl = rx + 21;
    pl[0] = 0xAA;
    pl[1] = ESL_SOLUM_STOCK_MODEL_29;
    pl[2] = 0x1F;
    pl[3] = 0x00;
    pl[4] = 0x50;
    pl[5] = 0x11;
    pl[6] = 0x22;
    pl[7] = 0x33;
    pl[8] = 0x44;
    memset(pl + 9, 0, 23);
    CHECK(esl_solum_stock_parse(rx, 21 + 32, &f));
    CHECK_INT(f.msg, ESL_SOLUM_STOCK_MSG_POLL);
    CHECK_INT(f.model, ESL_SOLUM_STOCK_MODEL_29);
    CHECK_INT(f.mac, TAG);
    CHECK_INT(f.job, 0x11223344);
    CHECK(memcmp(f.dst, GW, 8) == 0);

    /* a STATUS: the job, the message and the code */
    pl[0] = 0xF4;
    pl[1] = ESL_SOLUM_STOCK_MODEL_29;
    pl[2] = 0x00;
    pl[3] = 0x00;
    pl[4] = 0x00;
    pl[5] = 0x42;
    pl[6] = 0xAD;
    pl[7] = 0x00;
    CHECK(esl_solum_stock_parse(rx, 21 + 32, &f));
    CHECK_INT(f.msg, ESL_SOLUM_STOCK_MSG_STATUS);
    CHECK_INT(f.job, 0x42);
    CHECK_INT(f.status_msg, 0xAD);
    CHECK_INT(f.code, 0x00);

    /* not one of ours */
    scan[0] = 0x00;
    scan[1] = 0x00;
    CHECK(!esl_solum_stock_parse(scan, sizeof scan, &f));
}

/* ---- the state machine, with a store but no radio ---- */

typedef struct {
    uint8_t buf[64][ESL_PSDU_MAX];
    size_t len[64];
    unsigned n;
} burst_t;

static bool collect(void *ctx, const uint8_t *p, size_t n)
{
    burst_t *b = ctx;
    if (b->n >= 64 || n > ESL_PSDU_MAX)
        return false;
    memcpy(b->buf[b->n], p, n);
    b->len[b->n] = n;
    b->n++;
    return true;
}

static size_t tag_poll(uint8_t *out, uint16_t pan, uint64_t job, uint8_t model)
{
    out[0] = 0x61;
    out[1] = 0xCC;
    out[2] = 1;
    out[3] = (uint8_t)pan;
    out[4] = (uint8_t)(pan >> 8);
    memcpy(out + 5, GW, 8);
    put_le64(out + 13, TAG);
    uint8_t *pl = out + 21;
    pl[0] = 0xAA;
    pl[1] = model;
    pl[2] = 0x1F;
    pl[3] = 0x00;
    pl[4] = 0x50;
    pl[5] = (uint8_t)(job >> 24);
    pl[6] = (uint8_t)(job >> 16);
    pl[7] = (uint8_t)(job >> 8);
    pl[8] = (uint8_t)job;
    memset(pl + 9, 0, 23);
    return 21 + 32;
}

static void test_solum_stock_handle(void)
{
    esl_store_t st;
    void *mem = malloc(esl_solum_stock_mem_size());
    esl_solum_stock_t sk;
    burst_t b;
    uint8_t rx[ESL_PSDU_MAX];
    esl_content_t c;
    esl_set_result_t res;
    CHECK(mem != NULL);

    esl_store_init(&st, 8, NULL, 0);
    CHECK_INT(esl_content_init(&c, ESL_LAYOUT_TEXT, 0), ESL_CONTENT_OK);
    esl_content_add_field(&c, (const uint8_t *)"hello", 5);
    esl_content_trim(&c);
    CHECK_INT(esl_store_set(&st, TAG, &c, &res), ESL_STORE_OK);
    esl_solum_stock_init(&sk, mem, GW, 20);

    /* a POLL with no job: a burst of IMG fragments, the first one an AD */
    size_t n = tag_poll(rx, 0x1234, 0xFFFFFFFF, ESL_SOLUM_STOCK_MODEL_154);
    memset(&b, 0, sizeof b);
    unsigned frames = esl_solum_stock_handle(&sk, &st, NULL, 0x1234, rx, n, -60, 1000, collect, &b);
    CHECK(frames > 0);
    CHECK_INT(b.n, frames);
    CHECK_INT(b.buf[0][21], ESL_SOLUM_STOCK_MSG_IMG);
    CHECK_INT(b.buf[0][0], 0x61);
    CHECK_INT(b.buf[0][1], 0xCC);
    uint16_t total = (uint16_t)(b.buf[0][22] << 8 | b.buf[0][23]);
    CHECK_INT(total, frames);

    /* it is now listed like any other tag: model, signal and last seen */
    esl_tag_t t;
    CHECK(esl_store_get(&st, TAG, &t));
    CHECK_INT(t.model, ESL_MODEL_154);
    CHECK_INT(t.rssi, -60);
    CHECK(t.seen);
    CHECK(t.polls >= 1);

    /* the job number the AP gave, from the stream head of the first fragment */
    const uint8_t *stream = b.buf[0] + 21 + ESL_SOLUM_STOCK_FRAG_HDR;
    uint32_t job = (uint32_t)stream[4] << 24 | stream[5] << 16 | stream[6] << 8 | stream[7];
    CHECK(job != 0 && job != 0xFFFFFFFF);

    /* its STATUS code 0 says the job was taken: no longer "new content waiting" */
    esl_solum_stock_frame_t sf = {0};
    sf.msg = ESL_SOLUM_STOCK_MSG_STATUS;
    sf.mac = TAG;
    sf.job = job;
    sf.code = 0;
    CHECK(esl_solum_stock_status(&sk, &st, &sf));
    CHECK(esl_store_get(&st, TAG, &t));
    CHECK(!esl_tag_pending(&t));
    sf.code = 1;
    CHECK(!esl_solum_stock_status(&sk, &st, &sf));

    /* the same content again, now with the job: nothing new (DE) */
    n = tag_poll(rx, 0x1234, job, ESL_SOLUM_STOCK_MODEL_154);
    memset(&b, 0, sizeof b);
    frames = esl_solum_stock_handle(&sk, &st, NULL, 0x1234, rx, n, -60, 2000, collect, &b);
    CHECK_INT(frames, 1);
    CHECK_INT(b.buf[0][21], ESL_SOLUM_STOCK_MSG_NONE);

    /* a new content: a new job */
    esl_content_add_field(&c, (const uint8_t *)"world", 5);
    esl_content_trim(&c);
    CHECK_INT(esl_store_set(&st, TAG, &c, &res), ESL_STORE_OK);
    n = tag_poll(rx, 0x1234, job, ESL_SOLUM_STOCK_MODEL_154);
    memset(&b, 0, sizeof b);
    frames = esl_solum_stock_handle(&sk, &st, NULL, 0x1234, rx, n, -60, 3000, collect, &b);
    CHECK(frames > 0);
    const uint8_t *s2 = b.buf[0] + 21 + ESL_SOLUM_STOCK_FRAG_HDR;
    uint32_t job2 = (uint32_t)s2[4] << 24 | s2[5] << 16 | s2[6] << 8 | s2[7];
    CHECK(job2 != job);

    /* a frame for another PAN, and one for another AP: ignored */
    n = tag_poll(rx, 0x9999, 0, ESL_SOLUM_STOCK_MODEL_154);
    memset(&b, 0, sizeof b);
    CHECK_INT(esl_solum_stock_handle(&sk, &st, NULL, 0x1234, rx, n, -60, 4000, collect, &b), 0);
    n = tag_poll(rx, 0x1234, 0, ESL_SOLUM_STOCK_MODEL_154);
    rx[5] ^= 0xFF; /* not our EUI-64 */
    CHECK_INT(esl_solum_stock_handle(&sk, &st, NULL, 0x1234, rx, n, -60, 4000, collect, &b), 0);

    esl_store_free(&st);
    free(mem);
}

void test_solum_stock(void)
{
    test_solum_stock_models();
    test_solum_stock_deflate();
    test_solum_stock_job();
    test_solum_stock_stream();
    test_solum_stock_frames();
    test_solum_stock_handle();
}
