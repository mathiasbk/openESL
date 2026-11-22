#include "esl_solum_stock.h"
#include "esl_render.h"
#include <string.h>

/* SoluM's stock firmware (esl_solum_stock.h): the vendor's frames, and the vendor's
 * image format (a 1-bit BMP behind a page block, zlib compressed). An independent
 * implementation; the compressor follows RFC 1950/1951. The frame builders are checked
 * against the expected bytes in ap/test/test_solum_stock.c. */

/* ---- small integer helpers (the frames are big-endian on the wire) ---- */
static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* ---- model bytes ---- */
uint8_t esl_solum_stock_model_to_esl(uint8_t stock_model)
{
    switch (stock_model) {
    case ESL_SOLUM_STOCK_MODEL_154: return ESL_MODEL_154;
    case ESL_SOLUM_STOCK_MODEL_29:  return ESL_MODEL_29;
    default:                  return ESL_MODEL_UNKNOWN;
    }
}

uint8_t esl_solum_stock_model_from_esl(uint8_t model)
{
    switch (model) {
    case ESL_MODEL_154: return ESL_SOLUM_STOCK_MODEL_154;
    case ESL_MODEL_29:  return ESL_SOLUM_STOCK_MODEL_29;
    default:            return 0;
    }
}

/* ---- scratch layout ----------------------------------------------------------
 * One block for the job builder, the compressor and the state machine (they do not
 * run at once), so the caller allocates it once. */
#define RENDER_OFF 0
#define BMP_OFF    ESL_SOLUM_STOCK_RENDER_MAX
#define STREAM_OFF (ESL_SOLUM_STOCK_RENDER_MAX + ESL_SOLUM_STOCK_BMP_MAX)
/* The hash table is read and written as uint16_t, so round the offset up to keep it
 * aligned whatever the caller's block (malloc) is aligned to. */
#define HASH_OFF   (((ESL_SOLUM_STOCK_RENDER_MAX + ESL_SOLUM_STOCK_BMP_MAX + ESL_SOLUM_STOCK_STREAM_MAX) + 3u) & ~3u)
#define HASH_BITS  12
#define HASH_N     (1u << HASH_BITS)
#define HASH_SIZE  (HASH_N * 2u)

size_t esl_solum_stock_mem_size(void)
{
    return HASH_OFF + HASH_SIZE;
}

/* ---- deflate (RFC 1951) and zlib (RFC 1950) ----------------------------------
 * Fixed Huffman codes and a greedy LZ77 pass over a 1 KB window (the window the tag's
 * decoder is built for). Simple, and small enough to check on the host. */

typedef struct {
    uint8_t *out;
    size_t cap, len;
    uint32_t bits;
    unsigned nbits;
} bw_t;

/* n bits, least significant first (the block header and the extra bits) */
static void bw_put(bw_t *w, unsigned v, unsigned n)
{
    w->bits |= (uint32_t)v << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        if (w->len < w->cap)
            w->out[w->len] = (uint8_t)(w->bits & 0xFF);
        w->len++;
        w->bits >>= 8;
        w->nbits -= 8;
    }
}

/* a Huffman code: its bits, most significant first */
static void bw_code(bw_t *w, unsigned code, unsigned n)
{
    for (unsigned i = n; i-- > 0;)
        bw_put(w, (code >> i) & 1, 1);
}

/* RFC 1951 3.2.5, the length and distance codes and their extra bits */
static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,    7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put_lit(bw_t *w, unsigned b)
{
    if (b <= 143)
        bw_code(w, 0x30 + b, 8);
    else
        bw_code(w, 0x190 + (b - 144), 9);
}

static void put_match(bw_t *w, unsigned len, unsigned dist)
{
    unsigned i = 0;
    while (i < 28 && len >= len_base[i + 1])
        i++;
    unsigned lc = 257 + i;
    if (lc <= 279)
        bw_code(w, lc - 256, 7);
    else
        bw_code(w, 0xC0 + (lc - 280), 8);
    bw_put(w, len - len_base[i], len_extra[i]);
    unsigned j = 0;
    while (j < 29 && dist >= dist_base[j + 1])
        j++;
    bw_code(w, j, 5);
    bw_put(w, dist - dist_base[j], dist_extra[j]);
}

static unsigned hash3(const uint8_t *p)
{
    return ((unsigned)p[0] << 10 ^ (unsigned)p[1] << 5 ^ p[2]) & (HASH_N - 1);
}

static size_t deflate_fixed(void *scratch, const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    bw_t w = {out, cap, 0, 0, 0};
    bw_put(&w, 1, 1); /* BFINAL */
    bw_put(&w, 1, 2); /* BTYPE = 01: fixed Huffman */
    uint16_t *head = (uint16_t *)((uint8_t *)scratch + HASH_OFF);
    for (unsigned i = 0; i < HASH_N; i++)
        head[i] = 0xFFFF;
    size_t i = 0;
    while (i < n) {
        unsigned blen = 0, bdist = 0;
        if (i + 3 <= n) {
            unsigned h = hash3(in + i);
            unsigned cand = head[h];
            if (cand != 0xFFFF && cand < i && i - cand <= 1024) {
                size_t maxl = n - i;
                if (maxl > 258)
                    maxl = 258;
                size_t l = 0;
                while (l < maxl && in[cand + l] == in[i + l])
                    l++;
                if (l >= 3) {
                    blen = (unsigned)l;
                    bdist = (unsigned)(i - cand);
                }
            }
            head[h] = (uint16_t)i;
        }
        if (blen >= 3) {
            put_match(&w, blen, bdist);
            for (size_t k = 1; k < blen && i + k + 3 <= n; k++)
                head[hash3(in + i + k)] = (uint16_t)(i + k);
            i += blen;
        } else {
            put_lit(&w, in[i]);
            i++;
        }
    }
    bw_code(&w, 0, 7); /* end of block (symbol 256) */
    if (w.nbits)
        bw_put(&w, 0, 8 - w.nbits); /* to the next byte */
    return w.len > cap ? 0 : w.len;
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a += p[i];
        if (a >= 65521)
            a -= 65521;
        b += a;
        if (b >= 65521)
            b -= 65521;
    }
    return b << 16 | a;
}

size_t esl_solum_stock_deflate(void *scratch, const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    if (!n || n > ESL_SOLUM_STOCK_BMP_MAX || cap < 6)
        return 0;
    out[0] = 0x28; /* CM = 8 (deflate), CINFO = 2 (a 1 KB window) */
    out[1] = 0x15; /* FLEVEL 0, FDICT 0, FCHECK: (0x28 << 8 | 0x15) % 31 == 0 */
    size_t body = deflate_fixed(scratch, in, n, out + 2, cap - 6);
    if (!body)
        return 0;
    uint32_t ad = adler32(in, n);
    put_be32(out + 2 + body, ad);
    return 2 + body + 4;
}

/* ---- the tag's 1-bit BMP -----------------------------------------------------
 * 62-byte header, rows top-down, MSB = left, 1 = white, 0 = black (the palette is
 * ignored). The 1.54" panel is drawn as esl_render gives it,
 * mirrored left to right; the 2.9" panel is mounted portrait, so its landscape image is
 * turned a quarter clockwise, which together with the mirroring is a transpose. */

static void bmp_head(uint8_t *b, uint16_t w, uint16_t h, uint32_t size)
{
    memset(b, 0, 62);
    b[0] = 'B';
    b[1] = 'M';
    b[2] = (uint8_t)(62 + size); /* bfSize, little-endian (the BMP header is LE) */
    b[3] = (uint8_t)((62 + size) >> 8);
    b[4] = (uint8_t)((62 + size) >> 16);
    b[5] = (uint8_t)((62 + size) >> 24);
    b[10] = 62;                    /* bfOffBits */
    b[14] = 40;                    /* biSize */
    b[18] = (uint8_t)w;
    b[19] = (uint8_t)(w >> 8);
    b[22] = (uint8_t)h;
    b[23] = (uint8_t)(h >> 8);
    b[26] = 1;                     /* biPlanes */
    b[28] = 1;                     /* biBitCount */
    b[34] = (uint8_t)size;         /* biSizeImage */
    b[35] = (uint8_t)(size >> 8);
    b[36] = (uint8_t)(size >> 16);
    b[37] = (uint8_t)(size >> 24);
    b[46] = 2;                     /* biClrUsed */
    b[50] = 2;                     /* biClrImportant */
    b[58] = 0xFF;                  /* palette[1] = white */
    b[59] = 0xFF;
    b[60] = 0xFF;
}

static size_t bmp_build(uint8_t *bmp, uint8_t model, const uint8_t *render)
{
    if (model == ESL_MODEL_154) {
        const uint16_t W = 200, H = 200, CB = 25, STRIDE = 28;
        size_t size = (size_t)H * STRIDE;
        bmp_head(bmp, W, H, (uint32_t)size);
        memset(bmp + 62, 0xFF, size); /* white, padding included */
        for (uint16_t y = 0; y < H; y++) {
            uint8_t *row = bmp + 62 + (size_t)y * STRIDE;
            for (uint16_t x = 0; x < W; x++) {
                if (render[(size_t)x * CB + y / 8] & (0x80 >> (y % 8))) {
                    uint16_t c = (uint16_t)(W - 1 - x); /* the firmware mirrors x */
                    row[c / 8] &= (uint8_t)~(0x80 >> (c % 8));
                }
            }
        }
        return 62 + size;
    }
    if (model == ESL_MODEL_29) {
        const uint16_t LW = 296, CB = 16;
        size_t size = (size_t)LW * CB;
        bmp_head(bmp, 128, 296, (uint32_t)size);
        for (uint16_t x = 0; x < LW; x++) {
            uint8_t *row = bmp + 62 + (size_t)x * CB;
            const uint8_t *col = render + (size_t)x * CB;
            for (unsigned k = 0; k < CB; k++)
                row[k] = (uint8_t)~col[k]; /* transpose and invert (1 = white) */
        }
        return 62 + size;
    }
    return 0;
}

/* the 17-byte page block in front of the zlib stream */
static void page_block(uint8_t *out, uint8_t model, size_t comp, size_t decomp)
{
    uint16_t w = model == ESL_MODEL_154 ? 152 : 128;
    uint16_t h = model == ESL_MODEL_154 ? 152 : 296;
    uint8_t x = 0;
    for (size_t i = 0; i < comp; i++)
        x ^= out[17 + i];
    out[0] = 0x01;
    out[1] = 0x02;
    put_be16(out + 2, (uint16_t)comp);
    put_be16(out + 4, (uint16_t)decomp);
    out[6] = 0xFF;
    out[7] = 0xFF ^ x;
    out[8] = out[9] = out[10] = out[11] = 0;
    put_be16(out + 12, w);
    put_be16(out + 14, h);
    out[16] = 0x01;
}

size_t esl_solum_stock_job(void *scratch, uint8_t model, const uint8_t *render, uint8_t *out, size_t cap)
{
    uint8_t *bmp = (uint8_t *)scratch + BMP_OFF;
    size_t blen = bmp_build(bmp, model, render);
    if (!blen || cap < 17)
        return 0;
    size_t z = esl_solum_stock_deflate(scratch, bmp, blen, out + 17, cap - 17);
    if (!z)
        return 0;
    page_block(out, model, z, blen);
    return 17 + z;
}

size_t esl_solum_stock_stream(void *scratch, uint8_t model, const uint8_t *render, uint32_t job, uint8_t channel,
                        uint8_t *out, size_t cap)
{
    if (cap < ESL_SOLUM_STOCK_STREAM_HEAD)
        return 0;
    size_t jl = esl_solum_stock_job(scratch, model, render, out + ESL_SOLUM_STOCK_STREAM_HEAD, cap - ESL_SOLUM_STOCK_STREAM_HEAD);
    if (!jl)
        return 0;
    out[0] = 0x0B;
    out[1] = 0xB8;
    out[2] = 0x96;
    out[3] = (uint8_t)(0x10 | ((channel - 11) & 0x0F)); /* checked on channel 20 (0x19) */
    put_be32(out + 4, job);
    out[8] = out[9] = out[10] = 0x01;
    return ESL_SOLUM_STOCK_STREAM_HEAD + jl;
}

/* ---- frames ------------------------------------------------------------------ */

bool esl_solum_stock_parse(const uint8_t *psdu, size_t psdu_len, esl_solum_stock_frame_t *out)
{
    if (psdu_len < 11)
        return false;
    uint16_t fcf = (uint16_t)(psdu[0] | psdu[1] << 8);
    memset(out, 0, sizeof *out);
    out->seq = psdu[2];
    out->pan = (uint16_t)(psdu[3] | psdu[4] << 8);
    if (fcf == 0x8841) { /* broadcast SCAN: short dst, short src, 9-byte header */
        out->short_addr = (uint16_t)(psdu[7] | psdu[8] << 8);
        out->dst[0] = psdu[5];
        out->dst[1] = psdu[6];
        out->payload = psdu + 9;
        out->payload_len = psdu_len - 9;
    } else if (fcf == 0xCC61) { /* long dst and long src, 21-byte header */
        if (psdu_len < 23)
            return false;
        memcpy(out->dst, psdu + 5, 8);
        out->mac = get_le64(psdu + 13); /* the source: the tag */
        out->payload = psdu + 21;
        out->payload_len = psdu_len - 21;
    } else {
        return false;
    }
    out->msg = out->payload[0];
    switch (out->msg) {
    case ESL_SOLUM_STOCK_MSG_SCAN:
    case ESL_SOLUM_STOCK_MSG_JOIN:
    case ESL_SOLUM_STOCK_MSG_POLL:
    case ESL_SOLUM_STOCK_MSG_STATUS:
        if (out->payload_len >= 2)
            out->model = out->payload[1];
        break;
    default:
        return false;
    }
    if (out->msg == ESL_SOLUM_STOCK_MSG_POLL) {
        if (out->payload_len < 9)
            return false;
        out->job = get_be32(out->payload + 5);
    } else if (out->msg == ESL_SOLUM_STOCK_MSG_STATUS) {
        if (out->payload_len < 8)
            return false;
        out->job = get_be32(out->payload + 2);
        out->status_msg = out->payload[6];
        out->code = out->payload[7];
    }
    return true;
}

static size_t long_head(uint8_t *out, uint16_t pan, uint64_t mac, const uint8_t gw[8], uint8_t seq)
{
    out[0] = 0x61;
    out[1] = 0xCC;
    out[2] = seq;
    out[3] = (uint8_t)pan;
    out[4] = (uint8_t)(pan >> 8);
    put_le64(out + 5, mac);  /* the tag */
    memcpy(out + 13, gw, 8); /* the AP */
    return 21;
}

size_t esl_solum_stock_build_scan_reply(uint8_t *out, uint16_t pan, uint16_t short_addr, const uint8_t gw[8])
{
    static const uint8_t e1[7] = {0xE1, 0x03, 0x0B, 0xB8, 0x05, 0x55, 0x96};
    out[0] = 0x41;
    out[1] = 0xC8;
    out[2] = 0;
    out[3] = (uint8_t)pan;
    out[4] = (uint8_t)(pan >> 8);
    out[5] = (uint8_t)short_addr;
    out[6] = (uint8_t)(short_addr >> 8);
    memcpy(out + 7, gw, 8);
    memcpy(out + 15, e1, sizeof e1);
    return 15 + sizeof e1;
}

size_t esl_solum_stock_build_join_reply(uint8_t *out, uint16_t pan, uint64_t mac, const uint8_t gw[8], uint8_t seq)
{
    static const uint8_t d8[11] = {0xD8, 0x00, 0x00, 0x0B, 0xB8, 0x05, 0x7D, 0x96, 0x00, 0x00, 0x00};
    long_head(out, pan, mac, gw, seq);
    memcpy(out + 21, d8, sizeof d8);
    return 21 + sizeof d8;
}

size_t esl_solum_stock_build_none(uint8_t *out, uint16_t pan, uint64_t mac, const uint8_t gw[8], uint32_t job, uint8_t seq)
{
    /* DE <job BE32> then a fixed 11-byte tail */
    static const uint8_t tail[11] = {0x00, 0x19, 0x0B, 0xB8, 0x06, 0xE3, 0x96, 0x6A, 0xFF, 0xF6, 0x7C};
    long_head(out, pan, mac, gw, seq);
    out[21] = ESL_SOLUM_STOCK_MSG_NONE;
    put_be32(out + 22, job);
    memcpy(out + 26, tail, sizeof tail);
    return 21 + 16;
}

size_t esl_solum_stock_build_img(uint8_t *out, uint16_t pan, uint64_t mac, const uint8_t gw[8], uint8_t seq,
                           uint16_t total, uint16_t idx, uint16_t ts, const uint8_t *data, size_t n)
{
    if (n > ESL_SOLUM_STOCK_FRAG_DATA)
        return 0;
    long_head(out, pan, mac, gw, seq);
    out[21] = ESL_SOLUM_STOCK_MSG_IMG;
    put_be16(out + 22, total);
    put_be16(out + 24, idx);
    put_be16(out + 26, ts);
    memcpy(out + 28, data, n);
    return 21 + ESL_SOLUM_STOCK_FRAG_HDR + n;
}

/* ---- the state machine -------------------------------------------------------- */

void esl_solum_stock_init(esl_solum_stock_t *sk, void *mem, const uint8_t gw[8], uint8_t channel)
{
    memset(sk, 0, sizeof *sk);
    memcpy(sk->gw, gw, 8);
    sk->channel = channel;
    sk->scratch = mem;
    sk->next_job = 0x100; /* above the small job numbers a tag starts with */
}

static esl_solum_stock_tag_t *tag_get(esl_solum_stock_t *sk, uint64_t mac)
{
    esl_solum_stock_tag_t *free = NULL;
    for (int i = 0; i < ESL_SOLUM_STOCK_TAGS; i++) {
        if (sk->tag[i].used) {
            if (sk->tag[i].mac == mac)
                return &sk->tag[i];
        } else if (!free) {
            free = &sk->tag[i];
        }
    }
    if (free) {
        memset(free, 0, sizeof *free);
        free->used = true;
        free->mac = mac;
    }
    return free;
}

static unsigned send_none(esl_solum_stock_t *sk, uint16_t pan, uint64_t mac, uint32_t job, esl_solum_stock_send_fn send, void *ctx)
{
    uint8_t tx[ESL_PSDU_MAX];
    size_t n = esl_solum_stock_build_none(tx, pan, mac, sk->gw, job, sk->seq++);
    return send && send(ctx, tx, n) ? 1u : 0u;
}

bool esl_solum_stock_status(esl_solum_stock_t *sk, esl_store_t *st, const esl_solum_stock_frame_t *f)
{
    if (!f || f->msg != ESL_SOLUM_STOCK_MSG_STATUS || f->code != 0 || !f->job)
        return false;
    for (int i = 0; i < ESL_SOLUM_STOCK_TAGS; i++) {
        esl_solum_stock_tag_t *t = &sk->tag[i];
        if (t->used && t->mac == f->mac && t->job == f->job)
            return esl_store_note_stock_shown(st, f->mac, t->content_id);
    }
    return false;
}

unsigned esl_solum_stock_handle(esl_solum_stock_t *sk, esl_store_t *st, esl_layouts_t *ls, uint16_t pan,
                          const uint8_t *psdu, size_t psdu_len, int8_t rssi, uint64_t now_ms,
                          esl_solum_stock_send_fn send, void *ctx)
{
    esl_solum_stock_frame_t f;
    if (!esl_solum_stock_parse(psdu, psdu_len, &f) || f.pan != pan)
        return 0;

    if (f.msg == ESL_SOLUM_STOCK_MSG_SCAN) {
        uint8_t tx[ESL_PSDU_MAX];
        size_t n = esl_solum_stock_build_scan_reply(tx, pan, f.short_addr, sk->gw);
        return send && send(ctx, tx, n) ? 1u : 0u;
    }
    if (f.msg != ESL_SOLUM_STOCK_MSG_JOIN && f.msg != ESL_SOLUM_STOCK_MSG_POLL)
        return 0; /* STATUS is a report: the caller logs it, nothing to answer */
    if (memcmp(f.dst, sk->gw, 8) != 0)
        return 0; /* not addressed to this AP */

    /* It is here: add it to the table and note what it said, so it is listed like any
       other tag (its content, if any, is checked against its model's layout file). */
    if (f.msg == ESL_SOLUM_STOCK_MSG_POLL)
        esl_store_note_stock_poll(st, f.mac, now_ms);
    esl_store_note_seen(st, f.mac, esl_solum_stock_model_to_esl(f.model), 0, rssi, now_ms);
    esl_store_check_tag(st, ls, f.mac);

    if (f.msg == ESL_SOLUM_STOCK_MSG_JOIN) {
        uint8_t tx[ESL_PSDU_MAX];
        size_t n = esl_solum_stock_build_join_reply(tx, pan, f.mac, sk->gw, sk->seq++);
        return send && send(ctx, tx, n) ? 1u : 0u;
    }

    uint8_t model = esl_solum_stock_model_to_esl(f.model);
    esl_tag_t stt;
    esl_solum_stock_tag_t *t = tag_get(sk, f.mac);
    if (!model || !t || !esl_store_get(st, f.mac, &stt) || !stt.has_content)
        return send_none(sk, pan, f.mac, f.job, send, ctx);

    /* the tag holds the job we last gave it: nothing new */
    if (t->job && t->job == f.job && t->content_id == stt.content_id)
        return send_none(sk, pan, f.mac, f.job, send, ctx);

    t->model = model;
    if (!t->job || t->content_id != stt.content_id)
        t->job = ++sk->next_job; /* new content: a new job number */
    t->content_id = stt.content_id;

    esl_content_t c;
    uint8_t *render = (uint8_t *)sk->scratch + RENDER_OFF;
    if (!esl_store_get_content(st, f.mac, &c) || !esl_layouts_render(ls, model, &c, render, ESL_SOLUM_STOCK_RENDER_MAX))
        return send_none(sk, pan, f.mac, f.job, send, ctx);

    uint8_t *stream = (uint8_t *)sk->scratch + STREAM_OFF;
    size_t sl = esl_solum_stock_stream(sk->scratch, model, render, t->job, sk->channel, stream, ESL_SOLUM_STOCK_STREAM_MAX);
    if (!sl)
        return send_none(sk, pan, f.mac, f.job, send, ctx);

    unsigned frames = 0;
    size_t nfrag = (sl + ESL_SOLUM_STOCK_FRAG_DATA - 1) / ESL_SOLUM_STOCK_FRAG_DATA;
    for (size_t i = 0; i < nfrag; i++) {
        size_t off = i * ESL_SOLUM_STOCK_FRAG_DATA;
        size_t len = sl - off;
        if (len > ESL_SOLUM_STOCK_FRAG_DATA)
            len = ESL_SOLUM_STOCK_FRAG_DATA;
        uint16_t ts = (uint16_t)(0x088D + i);
        uint8_t tx[ESL_PSDU_MAX];
        size_t n = esl_solum_stock_build_img(tx, pan, f.mac, sk->gw, sk->seq++, (uint16_t)nfrag, (uint16_t)i, ts,
                                       stream + off, len);
        if (!send || !send(ctx, tx, n)) {
            sk->dropped++;
            break;
        }
        frames++;
    }
    t->sent_ms = now_ms;
    sk->jobs++;
    return frames;
}
