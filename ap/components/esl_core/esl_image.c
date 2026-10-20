#include "esl_image.h"
#include "esl_crc32.h"
#include "esl_render.h"
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
    while (i < n) {
        size_t r = 1;
        while (i + r < n && r < 129 && in[i + r] == in[i])
            r++;
        if (r >= 2) {
            uint8_t v = in[i];
            if (o + 2 > max)
                return 0;
            out[o++] = (uint8_t)(0x80 + r - 2);
            out[o++] = v;
            i += r;
            continue;
        }
        /* bytes as they are, up to the next two alike */
        size_t l = 1;
        while (i + l < n && l < 128 && !(i + l + 1 < n && in[i + l] == in[i + l + 1]))
            l++;
        if (o + 1 + l > max)
            return 0;
        out[o++] = (uint8_t)(l - 1);
        memmove(out + o, in + i, l);
        o += l;
        i += l;
    }
    return o;
}

size_t esl_image_mem_size(void)
{
    size_t max = 0;
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
        size_t n = esl_render_size(m);
        if (n > max)
            max = n;
    }
    return ESL_IMAGE_BUFS * (max + ESL_IMG_RLE_MARGIN(max));
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
static bool draw(esl_layouts_t *ls, const esl_tag_t *t, uint32_t ver, esl_image_buf_t *b, size_t cap,
                 uint64_t now_ms)
{
    b->mac = 0;
    if (!t->has_content || t->content_err || !t->model)
        return false;
    /* Coded for a tag that takes it: drawn ESL_IMG_RLE_MARGIN in and coded down to the
       start, else (it would not be shorter) drawn again raw */
    size_t raw = esl_render_size(t->model), m = ESL_IMG_RLE_MARGIN(raw), n = 0;
    uint8_t fmt = ESL_IMG_FMT_RAW;
    if (t->image_rle && raw && cap >= raw + m &&
        esl_layouts_render(ls, t->model, &t->content, b->data + m, cap - m) == raw) {
        n = esl_img_rle_encode(b->data + m, raw, b->data, raw - 1);
        if (n)
            fmt = ESL_IMG_FMT_RLE;
    }
    if (!n)
        n = esl_layouts_render(ls, t->model, &t->content, b->data, cap);
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
    if (b && t.has_content && !t.content_err && b->content_id == t.content_id && b->layout_ver == ver)
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
    if (!draw(ls, &t, ver, b, im->cap, now_ms))
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

void esl_image_release(esl_image_t *im, uint64_t mac)
{
    lock(im);
    esl_image_buf_t *b = find(im, mac);
    if (b)
        b->mac = 0;
    unlock(im);
}
