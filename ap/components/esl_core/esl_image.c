#include "esl_image.h"
#include "esl_crc32.h"
#include "esl_img2d.h"
#include "esl_png.h"
#include "esl_render.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void lock(esl_image_t *im)
{
    if (im->ops.lock)
        im->ops.lock(im->ops.ctx);
}

static void unlock(esl_image_t *im)
{
    if (im->ops.unlock)
        im->ops.unlock(im->ops.ctx);
}

size_t esl_img_rle_encode(const uint8_t *in, size_t n, uint8_t *out, size_t max)
{
    size_t i = 0, o = 0;
    /* coding in place: the bytes written so far may not reach input not read yet */
    size_t ahead = (uintptr_t)out < (uintptr_t)in ? (size_t)((uintptr_t)in - (uintptr_t)out) : SIZE_MAX / 2;
    while (i < n) {
        size_t r = 1;
        while (i + r < n && r < 129 && in[i + r] == in[i])
            r++;
        if (r >= 2) {
            uint8_t v = in[i];
            if (o + 2 > max || o + 2 > ahead + i)
                return 0;
            if (out) {
                out[o] = (uint8_t)(0x80 + r - 2);
                out[o + 1] = v;
            }
            o += 2;
            i += r;
            continue;
        }
        /* bytes as they are, up to the next three alike (ending before two would let a
           pattern like "x yy z ww" grow 3 bytes to 4) */
        size_t l = 1;
        while (i + l < n && l < 128 && !(i + l + 2 < n && in[i + l] == in[i + l + 1] && in[i + l] == in[i + l + 2]))
            l++;
        if (o + 1 + l > max || o + 1 > ahead + i)
            return 0;
        if (out) {
            out[o] = (uint8_t)(l - 1);
            memmove(out + o + 1, in + i, l);
        }
        o += 1 + l;
        i += l;
    }
    return o;
}

size_t esl_img_rle_decode(const uint8_t *in, size_t n, uint8_t *out, size_t raw)
{
    size_t i = 0, o = 0;
    /* decoding in place: the bytes written may not reach input not read yet */
    size_t ahead = (uintptr_t)out < (uintptr_t)in ? (size_t)((uintptr_t)in - (uintptr_t)out) : SIZE_MAX / 2;
    while (i < n) {
        uint8_t c = in[i];
        if (c & 0x80) {
            size_t r = (size_t)c - 0x80 + 2;
            if (i + 2 > n || o + r > raw || o + r > ahead + i + 2)
                return 0;
            memset(out + o, in[i + 1], r);
            o += r;
            i += 2;
        } else {
            size_t l = (size_t)c + 1;
            if (i + 1 + l > n || o + l > raw || o > ahead + i + 1)
                return 0;
            memmove(out + o, in + i + 1, l);
            o += l;
            i += 1 + l;
        }
    }
    return o == raw ? o : 0;
}

/* ---- pictures from clients ------------------------------------------------------- */

/* white, black, red: the colours of the image planes (none, black plane, red plane) */
static const int16_t ink[3][3] = {{255, 255, 255}, {0, 0, 0}, {255, 0, 0}};

typedef struct {
    bool (*read)(void *ctx, uint8_t *buf, size_t n);
    void *ctx;
    uint8_t head[8];   /* the first bytes, read to tell PNG from raw, given out again */
    size_t head_len, head_pos;
    uint16_t w, h;
    bool red, dither;
    size_t col_bytes, plane;
    uint8_t *out;
    int16_t *err;      /* dither: errors for this row and the next, (w + 2) * 3 each */
} pic_t;

static bool pic_read(void *ctx, uint8_t *buf, size_t n)
{
    pic_t *p = ctx;
    while (n && p->head_pos < p->head_len) {
        *buf++ = p->head[p->head_pos++];
        n--;
    }
    return !n || p->read(p->ctx, buf, n);
}

static bool pic_header(void *ctx, uint32_t w, uint32_t h)
{
    pic_t *p = ctx;
    return w == p->w && h == p->h;
}

/* The nearest colour the model has, weighing green most and blue least, as the eye does */
static unsigned nearest(const int *v, bool red)
{
    unsigned best = 0;
    long best_d = -1;
    for (unsigned k = 0; k < (red ? 3u : 2u); k++) {
        long dr = v[0] - ink[k][0], dg = v[1] - ink[k][1], db = v[2] - ink[k][2];
        long d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (best_d < 0 || d < best_d) {
            best = k;
            best_d = d;
        }
    }
    return best;
}

static bool pic_row(void *ctx, uint32_t y, const uint8_t *rgb)
{
    pic_t *p = ctx;
    size_t stride = ((size_t)p->w + 2) * 3;
    int16_t *e = p->err ? p->err + (y & 1) * stride : NULL, *next = p->err ? p->err + ((y + 1) & 1) * stride : NULL;
    if (next)
        memset(next, 0, stride * sizeof(*next));
    uint8_t bit = (uint8_t)(0x80 >> (y % 8));
    for (unsigned x = 0; x < p->w; x++) {
        int v[3];
        for (unsigned c = 0; c < 3; c++) {
            v[c] = rgb[3 * x + c] + (e ? e[(x + 1) * 3 + c] : 0);
            v[c] = v[c] < 0 ? 0 : v[c] > 255 ? 255 : v[c];
        }
        unsigned k = nearest(v, p->red);
        size_t at = x * p->col_bytes + y / 8;
        if (k == 1)
            p->out[at] |= bit;
        else if (k == 2)
            p->out[p->plane + at] |= bit;
        if (!e)
            continue;
        /* Floyd-Steinberg: 7/16 to the right, 3/16, 5/16 and 1/16 to the row below */
        for (unsigned c = 0; c < 3; c++) {
            int d = v[c] - ink[k][c];
            e[(x + 2) * 3 + c] = (int16_t)(e[(x + 2) * 3 + c] + d * 7 / 16);
            next[x * 3 + c] = (int16_t)(next[x * 3 + c] + d * 3 / 16);
            next[(x + 1) * 3 + c] = (int16_t)(next[(x + 1) * 3 + c] + d * 5 / 16);
            next[(x + 2) * 3 + c] = (int16_t)(next[(x + 2) * 3 + c] + d / 16);
        }
    }
    return true;
}

esl_pic_err_t esl_image_picture(uint8_t model, bool dither, size_t len, bool (*read)(void *ctx, uint8_t *buf, size_t n),
                                void *ctx, uint8_t *out)
{
    pic_t p = {read, ctx, {0}, 0, 0, 0, 0, false, dither, 0, 0, out, NULL};
    size_t raw = esl_render_size(model);
    if (!raw || !esl_render_model(model, &p.w, &p.h, &p.red))
        return ESL_PIC_ERR_SIZE;
    p.col_bytes = ESL_RENDER_COL_BYTES(p.h);
    p.plane = p.col_bytes * p.w;
    p.head_len = len < sizeof(p.head) ? len : sizeof(p.head);
    if (!read(ctx, p.head, p.head_len))
        return ESL_PIC_ERR_READ;
    if (p.head_len < sizeof(p.head) || memcmp(p.head, esl_png_signature, sizeof(p.head)) != 0) {
        /* the image bytes as they are */
        if (len != raw)
            return ESL_PIC_ERR_FORMAT;
        memcpy(out, p.head, p.head_len);
        if (!read(ctx, out + p.head_len, raw - p.head_len))
            return ESL_PIC_ERR_READ;
        for (size_t i = 0; p.red && i < p.plane; i++)
            if (out[i] & out[p.plane + i])
                return ESL_PIC_ERR_BOTH;
        return ESL_PIC_OK;
    }
    memset(out, 0, raw);
    if (dither) {
        p.err = calloc(2 * ((size_t)p.w + 2) * 3, sizeof(int16_t));
        if (!p.err)
            return ESL_PIC_ERR_NOMEM;
    }
    esl_png_io_t io = {&p, pic_read, pic_header, pic_row};
    esl_png_err_t err = esl_png_decode(&io);
    free(p.err);
    switch (err) {
    case ESL_PNG_OK:              return ESL_PIC_OK;
    case ESL_PNG_ERR_READ:        return ESL_PIC_ERR_READ;
    case ESL_PNG_ERR_UNSUPPORTED: return ESL_PIC_ERR_UNSUPPORTED;
    case ESL_PNG_ERR_SIZE:        return ESL_PIC_ERR_SIZE;
    case ESL_PNG_ERR_NOMEM:       return ESL_PIC_ERR_NOMEM;
    default:                      return ESL_PIC_ERR_BAD;
    }
}

size_t esl_image_pack(uint64_t mac, uint8_t model, const uint8_t *raw, uint8_t *out)
{
    size_t n = esl_render_size(model);
    for (int i = 0; i < 8; i++)
        out[i] = (uint8_t)(mac >> (8 * i));
    out[8] = model;
    /* coded only when shorter, as draw() does, so a tag gets the same bytes and id */
    size_t m = n ? esl_img_rle_encode(raw, n, out + ESL_IMAGE_STORED_HDR, n - 1) : 0;
    out[9] = m ? ESL_IMG_FMT_RLE : ESL_IMG_FMT_RAW;
    if (!m) {
        memcpy(out + ESL_IMAGE_STORED_HDR, raw, n);
        m = n;
    }
    return ESL_IMAGE_STORED_HDR + m;
}

size_t esl_image_unpack(uint64_t mac, uint8_t model, bool rle, uint8_t *buf, size_t len, size_t cap, uint8_t *fmt)
{
    size_t raw = esl_render_size(model);
    if (!raw || len < ESL_IMAGE_STORED_HDR || len > ESL_IMAGE_STORED_MAX(raw) || len > cap)
        return 0;
    uint64_t m = 0;
    for (int i = 7; i >= 0; i--)
        m = m << 8 | buf[i];
    size_t n = len - ESL_IMAGE_STORED_HDR;
    if (m != mac || buf[8] != model)
        return 0;
    if (buf[9] == ESL_IMG_FMT_RAW && n == raw) {
        memmove(buf, buf + ESL_IMAGE_STORED_HDR, n);
        *fmt = ESL_IMG_FMT_RAW;
        return n;
    }
    if (buf[9] != ESL_IMG_FMT_RLE || n >= raw)
        return 0;
    *fmt = ESL_IMG_FMT_RLE;
    if (rle) {
        memmove(buf, buf + ESL_IMAGE_STORED_HDR, n);
        return n;
    }
    /* decoded in place, from the end of the buffer */
    if (cap < raw + ESL_IMG_RLE_MARGIN(raw))
        return 0;
    uint8_t *in = buf + cap - n;
    memmove(in, buf + ESL_IMAGE_STORED_HDR, n);
    *fmt = ESL_IMG_FMT_RAW;
    return esl_img_rle_decode(in, n, buf, raw);
}

/* The tag's stored picture, unpacked into buf (cap bytes); 0 when it is missing, or not
   the one content names (it changed meanwhile, or is for another model) */
static size_t stored_picture(esl_store_t *st, uint64_t mac, uint8_t model, const esl_content_t *c, bool rle,
                             uint8_t *buf, size_t cap, uint8_t *fmt)
{
    uint8_t m;
    uint32_t crc;
    if (!esl_content_image(c, &m, &crc) || m != model)
        return 0;
    size_t len = esl_store_get_image(st, mac, buf, cap);
    if (!len || esl_crc32(buf, len) != crc)
        return 0;
    return esl_image_unpack(mac, model, rle, buf, len, cap, fmt);
}

size_t esl_image_screen(esl_store_t *st, esl_layouts_t *ls, uint64_t mac, uint8_t model, const esl_content_t *c,
                        uint8_t *out, size_t cap)
{
    uint8_t fmt;
    if (c->layout == ESL_LAYOUT_IMAGE)
        return stored_picture(st, mac, model, c, false, out, cap, &fmt);
    return esl_layouts_render(ls, model, c, out, cap);
}

size_t esl_image_mem_size(void)
{
    size_t max = 0;
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
        size_t n = esl_render_size(m);
        if (n > max)
            max = n;
    }
    return ESL_IMAGE_BUFS * (ESL_IMAGE_STORED_MAX(max) + ESL_IMG_RLE_MARGIN(max));
}

void esl_image_init(esl_image_t *im, uint8_t *mem, const esl_image_ops_t *ops)
{
    memset(im, 0, sizeof(*im));
    im->cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    for (unsigned i = 0; i < ESL_IMAGE_BUFS; i++)
        im->buf[i].data = mem + i * im->cap;
    if (ops)
        im->ops = *ops;
}

static esl_image_buf_t *find(esl_image_t *im, uint64_t mac)
{
    for (unsigned i = 0; i < ESL_IMAGE_BUFS; i++)
        if (im->buf[i].mac == mac)
            return &im->buf[i];
    return NULL;
}

/* A free buffer, else one whose image has been sent whole, else the one unused the
   longest once its hold is over, else NULL. */
static esl_image_buf_t *take(esl_image_t *im, uint64_t now_ms)
{
    esl_image_buf_t *old = NULL;
    for (unsigned i = 0; i < ESL_IMAGE_BUFS; i++) {
        esl_image_buf_t *b = &im->buf[i];
        if (!b->mac)
            return b;
        if (b->sent)
            old = b;
        else if (now_ms - b->used_ms >= ESL_IMAGE_HOLD_MS && (!old || (!old->sent && b->used_ms < old->used_ms)))
            old = b;
    }
    return old;
}

/* Version of the model's layout file, 0 when it has none. */
static uint32_t layout_ver(esl_layouts_t *ls, uint8_t model)
{
    uint32_t ver = 0;
    if (!esl_layouts_info(ls, model, &ver, NULL, NULL))
        ver = 0;
    return ver;
}

/* Draws the tag's content into b. False when the tag has no content it can be drawn
   for (b is then free). Under the lock. */
static bool draw(esl_store_t *st, esl_layouts_t *ls, const esl_tag_t *t, uint32_t ver, esl_image_buf_t *b,
                 size_t cap, uint64_t now_ms)
{
    esl_content_t c;
    b->mac = 0;
    if (!t->has_content || t->content_err || !t->model)
        return false;
    /* Content not checked against the file yet (from the schedule, or set before the model
       was known) is checked first: with a radio coprocessor (esl_rhost) the image is drawn
       before the tag's POLL gets here (esl_store_check_tag in esl_radio_handle) */
    if (t->content_check && !esl_store_check_tag(st, ls, t->mac))
        return false;
    /* From NVS on the device, in the tag's reply window (well under a millisecond,
       unless NVS is busy with a write elsewhere; then the next POLL offers it again).
       Another content_id than t's: it changed meanwhile, and is drawn on the next POLL */
    if (!esl_store_get_content(st, t->mac, &c) || esl_content_id(&c) != t->content_id)
        return false;
    /* Coded for a tag that takes it: drawn (or a picture read back and unpacked)
       ESL_IMG_RLE_MARGIN in, both codes counted, and the shorter coded down to the start
       (2D is a fraction of RLE on text and bars, but RLE wins on dithered pictures);
       raw when neither is shorter. 2D is counted as coded in place, m ahead, so it is
       only picked when it does not overtake the image (it can on dense columns at the
       left; RLE never does). Should coding still give 0, the image is read again and
       sent raw. */
    bool picture = c.layout == ESL_LAYOUT_IMAGE;
    size_t raw = esl_render_size(t->model), m = ESL_IMG_RLE_MARGIN(raw), n = 0;
    uint8_t fmt = ESL_IMG_FMT_RAW;
    uint16_t w = 0, h = 0;
    bool red, has_h = esl_render_model(t->model, &w, &h, &red);
    bool got = false;
    if (raw && cap >= raw + m && (t->image_rle || (t->image_2d && has_h))) {
        if (picture && stored_picture(st, t->mac, t->model, &c, false, b->data, cap, &fmt) == raw) {
            memmove(b->data + m, b->data, raw);
            got = true;
        } else if (!picture)
            got = esl_layouts_render(ls, t->model, &c, b->data + m, cap - m) == raw;
    }
    if (got) {
        uint8_t *in = b->data + m;
        size_t lr = t->image_rle ? esl_img_rle_encode(in, raw, NULL, raw - 1) : 0;
        size_t l2 = t->image_2d && has_h ? esl_img_2d_count(in, raw, h, m, (lr ? lr : raw) - 1) : 0;
        if (l2) {
            n = esl_img_2d_encode(in, raw, h, b->data, l2);
            fmt = ESL_IMG_FMT_2D;
        } else if (lr) {
            n = esl_img_rle_encode(in, raw, b->data, lr);
            fmt = ESL_IMG_FMT_RLE;
        } else {
            memmove(b->data, in, raw);
            n = raw;
            fmt = ESL_IMG_FMT_RAW;
        }
    }
    if (!n) {
        fmt = ESL_IMG_FMT_RAW;
        n = picture ? stored_picture(st, t->mac, t->model, &c, false, b->data, cap, &fmt)
                    : esl_layouts_render(ls, t->model, &c, b->data, cap);
    }
    if (!n)
        return false;
    uint32_t id = esl_crc32(b->data, n);
    b->mac = t->mac;
    b->content_id = t->content_id;
    b->layout_ver = ver;
    b->sent = false;
    b->id = id ? id : 1;
    b->size = (uint16_t)n;
    b->model = t->model;
    b->fmt = fmt;
    b->used_ms = now_ms;
    return true;
}

/* b holds the image of t's content, drawn with layout file version ver, in a format the
   tag (still) takes: a tag back on older firmware gets it drawn again */
static bool fresh(const esl_image_buf_t *b, const esl_tag_t *t, uint32_t ver)
{
    return t->has_content && !t->content_err && b->content_id == t->content_id && b->layout_ver == ver &&
           (b->fmt == ESL_IMG_FMT_RAW || (b->fmt == ESL_IMG_FMT_RLE && t->image_rle) ||
            (b->fmt == ESL_IMG_FMT_2D && t->image_2d));
}

/* The tag's image in a buffer: the one it has if it is still of its content, else a
   newly drawn one. NULL and *busy when there is no buffer for it, NULL when it has no
   content. Under the lock. */
static esl_image_buf_t *ready(esl_image_t *im, esl_store_t *st, esl_layouts_t *ls, uint64_t mac, uint64_t now_ms,
                              bool *busy)
{
    esl_tag_t t;
    *busy = false;
    if (!esl_store_get(st, mac, &t))
        return NULL;
    uint32_t ver = layout_ver(ls, t.model);
    esl_image_buf_t *b = find(im, mac);
    if (b && fresh(b, &t, ver))
        return b;
    if (!b) {
        if (!t.has_content || t.content_err)
            return NULL;
        b = take(im, now_ms);
        if (!b) {
            *busy = true;
            return NULL;
        }
    }
    if (!draw(st, ls, &t, ver, b, im->cap, now_ms))
        return NULL;
    im->drawn++;
    esl_store_image_known(st, mac, b->content_id, ver, b->id);
    return b;
}

uint32_t esl_image_prepare(esl_image_t *im, esl_store_t *st, esl_layouts_t *ls, uint64_t mac, uint32_t shown,
                           uint64_t now_ms, bool *busy)
{
    esl_tag_t t;
    uint32_t id = 0;
    *busy = false;
    if (!esl_store_get(st, mac, &t) || !t.has_content || t.content_err) {
        esl_image_release(im, mac);   /* nothing to show: a buffer it held is done with */
        return 0;
    }
    lock(im);
    if (t.image_id && t.image_id == shown && t.image_layout_ver == layout_ver(ls, t.model)) {
        /* It shows the image: nothing to draw, and its buffer is done with */
        esl_image_buf_t *b = find(im, mac);
        if (b)
            b->mac = 0;
        id = t.image_id;
    } else {
        esl_image_buf_t *b = ready(im, st, ls, mac, now_ms, busy);
        if (b)
            id = b->id;
    }
    unlock(im);
    return id;
}

uint32_t esl_image_prepare_later(esl_image_t *im, esl_store_t *st, esl_layouts_t *ls, uint64_t mac, uint32_t shown,
                                 uint64_t now_ms, bool *busy)
{
    esl_tag_t t;
    uint32_t id = 0;
    *busy = false;
    if (!esl_store_get(st, mac, &t) || !t.has_content || t.content_err) {
        esl_image_release(im, mac);
        return 0;
    }
    lock(im);
    uint32_t ver = layout_ver(ls, t.model);
    esl_image_buf_t *b = find(im, mac);
    if (t.image_id && t.image_id == shown && t.image_layout_ver == ver) {
        if (b)
            b->mac = 0;
        id = t.image_id;
    } else if (b && fresh(b, &t, ver)) {
        b->used_ms = now_ms; /* kept for the blocks that follow */
        id = b->id;
    } else {
        if (!im->later)
            im->later = mac;
        *busy = true;
    }
    unlock(im);
    return id;
}

bool esl_image_draw_later(esl_image_t *im, esl_store_t *st, esl_layouts_t *ls, uint64_t now_ms)
{
    bool busy, drew = false;
    lock(im);
    uint64_t mac = im->later;
    im->later = 0;
    if (mac)
        drew = ready(im, st, ls, mac, now_ms, &busy) != NULL;
    unlock(im);
    return drew;
}

size_t esl_image_block(esl_image_t *im, esl_store_t *st, esl_layouts_t *ls, uint64_t mac, uint32_t id,
                       uint16_t pos, uint8_t max, uint16_t next_poll_s, uint64_t now_ms, uint8_t *out)
{
    bool busy;
    size_t n = 0;
    lock(im);
    esl_image_buf_t *b = ready(im, st, ls, mac, now_ms, &busy);
    if (b) {
        if (id != b->id || pos >= b->size)
            pos = 0;
        n = b->size - pos;
        if (n > max)
            n = max;
        if (n > ESL_IMG_CHUNK_MAX)
            n = ESL_IMG_CHUNK_MAX;
        b->used_ms = now_ms;
        b->sent = pos + n >= b->size;
        n = esl_img_msg_build(out, next_poll_s, b->id, b->size, (uint16_t)pos, b->model, b->fmt,
                              b->data + pos, n);
    }
    unlock(im);
    if (!n)
        n = esl_idle_build(out, busy ? ESL_IMAGE_BUSY_S : next_poll_s);
    return n;
}

size_t esl_image_draw(esl_image_t *im, esl_store_t *st, esl_layouts_t *ls, uint64_t mac, uint8_t *out, size_t cap,
                      uint32_t *id, uint8_t *model, uint8_t *fmt)
{
    esl_tag_t t;
    esl_image_buf_t b;
    memset(&b, 0, sizeof(b));
    b.data = out;
    if (!esl_store_get(st, mac, &t))
        return 0;
    lock(im);
    uint32_t ver = layout_ver(ls, t.model);
    bool ok = draw(st, ls, &t, ver, &b, cap, 0);
    if (ok) {
        im->drawn++;
        esl_store_image_known(st, mac, b.content_id, ver, b.id);
    }
    unlock(im);
    if (!ok)
        return 0;
    *id = b.id;
    *model = b.model;
    *fmt = b.fmt;
    return b.size;
}

void esl_image_release(esl_image_t *im, uint64_t mac)
{
    lock(im);
    esl_image_buf_t *b = find(im, mac);
    if (b)
        b->mac = 0;
    unlock(im);
}
