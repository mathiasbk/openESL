#include "esl_nlink.h"
#include "esl_link.h"
#include "esl_radio.h"
#include <string.h>

/* ---- numbers, little-endian ---- */

typedef struct {
    uint8_t *p;
    size_t n, cap;
} wbuf_t;

static void put8(wbuf_t *w, uint8_t v)
{
    if (w->n < w->cap)
        w->p[w->n] = v;
    w->n++;
}

static void put16(wbuf_t *w, uint16_t v)
{
    put8(w, (uint8_t)v);
    put8(w, (uint8_t)(v >> 8));
}

static void put32(wbuf_t *w, uint32_t v)
{
    put16(w, (uint16_t)v);
    put16(w, (uint16_t)(v >> 16));
}

static void put64(wbuf_t *w, uint64_t v)
{
    put32(w, (uint32_t)v);
    put32(w, (uint32_t)(v >> 32));
}

typedef struct {
    const uint8_t *p;
    size_t n, at;
    bool bad;    /* read past the end */
} rbuf_t;

static uint8_t get8(rbuf_t *r)
{
    if (r->at >= r->n) {
        r->bad = true;
        return 0;
    }
    return r->p[r->at++];
}

static uint16_t get16(rbuf_t *r)
{
    uint16_t v = get8(r);
    return (uint16_t)(v | get8(r) << 8);
}

static uint32_t get32(rbuf_t *r)
{
    uint32_t v = get16(r);
    return v | (uint32_t)get16(r) << 16;
}

static uint64_t get64(rbuf_t *r)
{
    uint64_t v = get32(r);
    return v | (uint64_t)get32(r) << 32;
}

/* One frame: its header, then the payload in one or two parts (the second may be NULL). */
static bool send_frame(esl_nlink_write_t write, void *ctx, uint8_t type, const uint8_t *a, size_t na,
                       const uint8_t *b, size_t nb)
{
    size_t len = 1 + na + nb;
    if (len > 0xFFFF)
        return false;
    uint8_t hdr[ESL_NLINK_HDR] = {(uint8_t)len, (uint8_t)(len >> 8), type};
    return write(ctx, hdr, sizeof(hdr)) && (!na || write(ctx, a, na)) && (!nb || write(ctx, b, nb));
}

static bool send_buf(esl_nlink_write_t write, void *ctx, uint8_t type, const wbuf_t *w)
{
    return w->n <= w->cap && send_frame(write, ctx, type, w->p, w->n, NULL, 0);
}

/* ---- frames ---- */

void esl_nlink_rx_init(esl_nlink_rx_t *rx, uint8_t *buf, size_t cap)
{
    rx->buf = buf;
    rx->cap = cap;
    rx->got = 0;
}

bool esl_nlink_feed(esl_nlink_rx_t *rx, const uint8_t *data, size_t n,
                    bool (*on)(void *ctx, uint8_t type, const uint8_t *p, size_t len), void *ctx)
{
    while (n) {
        /* the header first, then as much of the frame as it says */
        size_t want = rx->got >= 2 ? 2 + (rx->buf[0] | (size_t)rx->buf[1] << 8) : ESL_NLINK_HDR;
        size_t take = want - rx->got < n ? want - rx->got : n;
        memcpy(rx->buf + rx->got, data, take);
        rx->got += take;
        data += take;
        n -= take;
        if (rx->got < 2)
            continue;
        size_t flen = rx->buf[0] | (size_t)rx->buf[1] << 8;
        if (flen == 0 || 2 + flen > rx->cap) {
            rx->got = 0;
            return false;
        }
        if (rx->got == 2 + flen) {
            size_t len = rx->got - ESL_NLINK_HDR;
            rx->got = 0;
            if (!on(ctx, rx->buf[2], rx->buf + ESL_NLINK_HDR, len))
                return false;
        }
    }
    return true;
}

/* ---- the radio's side ---- */

void esl_nlink_radio_init(esl_nlink_radio_t *r, esl_rcache_t *rc, const esl_nlink_radio_ops_t *ops,
                          esl_nlink_write_t write, void *wctx)
{
    memset(r, 0, sizeof(*r));
    r->rc = rc;
    if (ops)
        r->ops = *ops;
    r->write = write;
    r->wctx = wctx;
}

bool esl_nlink_radio_hello(esl_nlink_radio_t *r, const esl_nlink_hello_t *h)
{
    uint8_t b[32];
    wbuf_t w = {b, 0, sizeof(b)};
    r->in_sync = false;
    put8(&w, ESL_NLINK_VERSION);
    put32(&w, h->boot);
    put16(&w, h->radio_ver);
    put64(&w, h->id);
    put16(&w, h->max_tags);
    put16(&w, h->n_images);
    put32(&w, h->image_cap);
    put8(&w, h->fw_models);
    return send_buf(r->write, r->wctx, ESL_NLINK_HELLO, &w);
}

bool esl_nlink_radio_sync(esl_nlink_radio_t *r, uint64_t now_ms)
{
    esl_rsync_t q;
    uint8_t b[64 + 8 * ESL_RCACHE_WANTS + 4 * ESL_RSYNC_IMAGES + 9 * ESL_RCACHE_MODELS];
    wbuf_t w = {b, 0, sizeof(b)};
    esl_rcache_sync_req(r->rc, now_ms, &q);
    put32(&w, q.boot);
    put32(&w, q.ack);
    put32(&w, q.now_ms);
    put16(&w, (uint16_t)q.log_n);
    put32(&w, q.log_dropped);
    put8(&w, q.want);
    put64(&w, q.want_mac);
    put32(&w, q.want_image);
    put8(&w, q.want_fw_model);
    for (unsigned i = 0; i < ESL_RCACHE_WANTS; i++)
        put64(&w, q.wants[i]);
    put16(&w, (uint16_t)q.n_tags);
    put16(&w, (uint16_t)q.max_tags);
    put16(&w, (uint16_t)q.free_images);
    put8(&w, (uint8_t)q.n_images);
    for (unsigned i = 0; i < q.n_images; i++)
        put32(&w, q.images[i]);
    uint8_t nfw = 0;
    for (unsigned m = 0; m < ESL_RCACHE_MODELS; m++)
        nfw += q.fw[m].ver != 0;
    put8(&w, nfw);
    for (unsigned m = 0; m < ESL_RCACHE_MODELS; m++)
        if (q.fw[m].ver) {
            put8(&w, (uint8_t)(m + 1));
            put16(&w, q.fw[m].ver);
            put16(&w, q.fw[m].size);
            put32(&w, q.fw[m].crc);
        }
    if (!send_buf(r->write, r->wctx, ESL_NLINK_SYNC, &w))
        return false;
    for (unsigned i = 0; i < q.log_n; i++) {
        const esl_rlog_t *e = esl_rcache_log_at(r->rc, i);
        uint8_t h[10];
        wbuf_t lw = {h, 0, sizeof(h)};
        put32(&lw, e->seq);
        put32(&lw, e->t_ms);
        put8(&lw, (uint8_t)e->rssi);
        put8(&lw, e->len);
        if (!send_frame(r->write, r->wctx, ESL_NLINK_LOG, h, lw.n, e->psdu, e->len))
            return false;
    }
    r->in_sync = true;
    r->syncs++;
    return true;
}

static void get_cfg(rbuf_t *g, esl_tag_cfg_t *c)
{
    c->poll_s = get16(g);
    c->max_poll_s = get16(g);
    c->lost_ap_s = get16(g);
}

bool esl_nlink_radio_on(esl_nlink_radio_t *r, uint8_t type, const uint8_t *p, size_t len)
{
    rbuf_t g = {p, len, 0, false};
    esl_rcache_t *rc = r->rc;
    switch (type) {
    case ESL_NLINK_CONFIG: {
        uint8_t ch = get8(&g);
        uint16_t pan = get16(&g);
        /* a channel 802.15.4 does not have would stop the radio (the driver asserts) */
        if (g.bad || ch < 11 || ch > 26)
            return false;
        rc->pan = pan;
        if (r->ops.config)
            r->ops.config(r->ops.ctx, ch, pan);
        if (g.at < len && r->ops.tx_power) {
            int8_t dbm = (int8_t)get8(&g);
            if (dbm >= ESL_TX_POWER_MIN && dbm <= ESL_TX_POWER_MAX)
                r->ops.tx_power(r->ops.ctx, dbm);
        }
        return true;
    }
    case ESL_NLINK_CLEAR:
        esl_rcache_clear_tags(rc);
        return true;
    case ESL_NLINK_DEFAULT_POLL: {
        uint16_t s = get16(&g);
        if (g.bad)
            return false;
        esl_rcache_set_default_poll(rc, s);
        return true;
    }
    case ESL_NLINK_TAG: {
        esl_rtag_state_t s;
        memset(&s, 0, sizeof(s));
        s.mac = get64(&g);
        s.next_poll_s = get16(&g);
        s.has_cfg = get8(&g) != 0;
        get_cfg(&g, &s.cfg);
        s.image_id = get32(&g);
        s.fw_ver = get16(&g);
        s.image_failed = get32(&g);
        s.fw_failed = get16(&g);
        if (g.at + 4 <= g.n) { /* the long backoff (#261), after the fields older hosts send */
            s.cfg.long_poll_s = get16(&g);
            s.cfg.long_after_min = get16(&g);
        }
        if (g.bad)
            return false;
        esl_rcache_set_tag(rc, &s); /* no room: the host sends it again (it asks, ack) */
        return true;
    }
    case ESL_NLINK_DROP: {
        uint64_t mac = get64(&g);
        if (g.bad)
            return false;
        esl_rcache_drop_tag(rc, mac);
        return true;
    }
    case ESL_NLINK_IMAGE: {
        uint32_t id = get32(&g);
        uint8_t model = get8(&g), fmt = get8(&g);
        if (g.bad)
            return false;
        esl_rcache_put_image(rc, id, model, fmt, p + g.at, len - g.at);
        return true;
    }
    case ESL_NLINK_FW: {
        uint8_t model = get8(&g);
        uint16_t ver = get16(&g), size = get16(&g);
        uint32_t crc = get32(&g);
        if (g.bad || model < 1 || model > ESL_RCACHE_MODELS)
            return false;
        /* not offered while it arrives; ver 0: none */
        esl_rcache_set_fw(rc, model, 0, 0, 0);
        r->fw[model - 1].ver = ver;
        r->fw[model - 1].size = size;
        r->fw[model - 1].crc = crc;
        r->fw[model - 1].got = 0;
        return true;
    }
    case ESL_NLINK_FW_DATA: {
        uint8_t model = get8(&g);
        uint32_t off = get32(&g);
        size_t n = len - g.at;
        if (g.bad || model < 1 || model > ESL_RCACHE_MODELS)
            return false;
        esl_nlink_fw_rx_t *f = &r->fw[model - 1];
        if (!f->ver || off != f->got || off + n > f->size)
            return true; /* not the part it waits for: the firmware stays off */
        if (!r->ops.write_fw || !r->ops.write_fw(r->ops.ctx, model, off, p + g.at, n)) {
            f->ver = 0;
            return true;
        }
        f->got += (uint32_t)n;
        if (f->got == f->size)
            esl_rcache_set_fw(rc, model, f->ver, f->size, f->crc);
        return true;
    }
    case ESL_NLINK_END: {
        uint32_t seq = get32(&g);
        bool more = get8(&g) != 0;
        if (g.bad || !r->in_sync)
            return false;
        esl_rcache_sync_end(rc, seq, more);
        r->in_sync = false;
        r->ends++;
        return true;
    }
    default:
        return true; /* a newer host's: ignored */
    }
}

/* ---- the host's side ---- */

static bool out_ok(esl_nlink_host_t *n, bool ok)
{
    if (!ok)
        n->broken = true;
    return ok;
}

static void host_clear(void *ctx)
{
    esl_nlink_host_t *n = ctx;
    out_ok(n, send_frame(n->write, n->wctx, ESL_NLINK_CLEAR, NULL, 0, NULL, 0));
}

static bool host_tag(void *ctx, const esl_rtag_state_t *s)
{
    esl_nlink_host_t *n = ctx;
    uint8_t b[40];
    wbuf_t w = {b, 0, sizeof(b)};
    put64(&w, s->mac);
    put16(&w, s->next_poll_s);
    put8(&w, s->has_cfg);
    put16(&w, s->cfg.poll_s);
    put16(&w, s->cfg.max_poll_s);
    put16(&w, s->cfg.lost_ap_s);
    put32(&w, s->image_id);
    put16(&w, s->fw_ver);
    put32(&w, s->image_failed);
    put16(&w, s->fw_failed);
    put16(&w, s->cfg.long_poll_s);
    put16(&w, s->cfg.long_after_min);
    /* sent is not refused: whether the radio had room shows in its next SYNC */
    return out_ok(n, send_buf(n->write, n->wctx, ESL_NLINK_TAG, &w));
}

static void host_drop(void *ctx, uint64_t mac)
{
    esl_nlink_host_t *n = ctx;
    uint8_t b[8];
    wbuf_t w = {b, 0, sizeof(b)};
    put64(&w, mac);
    out_ok(n, send_buf(n->write, n->wctx, ESL_NLINK_DROP, &w));
}

static bool host_image(void *ctx, uint32_t id, uint8_t model, uint8_t fmt, const uint8_t *data, size_t size)
{
    esl_nlink_host_t *n = ctx;
    if (n->hello && size > n->radio.image_cap)
        return false; /* the radio could not keep it */
    uint8_t b[6];
    wbuf_t w = {b, 0, sizeof(b)};
    put32(&w, id);
    put8(&w, model);
    put8(&w, fmt);
    return out_ok(n, send_frame(n->write, n->wctx, ESL_NLINK_IMAGE, b, w.n, data, size));
}

static void host_fw(void *ctx, uint8_t model, const esl_tagfw_img_t *img)
{
    esl_nlink_host_t *n = ctx;
    uint8_t b[16];
    wbuf_t w = {b, 0, sizeof(b)};
    put8(&w, model);
    put16(&w, img->ver);
    put16(&w, img->size);
    put32(&w, img->crc);
    if (!out_ok(n, send_buf(n->write, n->wctx, ESL_NLINK_FW, &w)) || !img->ver)
        return;
    uint8_t part[ESL_NLINK_FW_PART];
    for (uint32_t off = 0; off < img->size; off += ESL_NLINK_FW_PART) {
        size_t len = img->size - off < ESL_NLINK_FW_PART ? img->size - off : ESL_NLINK_FW_PART;
        if (!n->fw || !esl_tagfw_read(n->fw, model, off, part, len))
            return; /* changed meanwhile: the radio does not offer it, and asks again */
        uint8_t h[5];
        wbuf_t hw = {h, 0, sizeof(h)};
        put8(&hw, model);
        put32(&hw, off);
        if (!out_ok(n, send_frame(n->write, n->wctx, ESL_NLINK_FW_DATA, h, hw.n, part, len)))
            return;
    }
}

static void host_default(void *ctx, uint16_t poll_s)
{
    esl_nlink_host_t *n = ctx;
    uint8_t b[2];
    wbuf_t w = {b, 0, sizeof(b)};
    put16(&w, poll_s);
    out_ok(n, send_buf(n->write, n->wctx, ESL_NLINK_DEFAULT_POLL, &w));
}

static void host_end(void *ctx, uint32_t seq, bool more)
{
    esl_nlink_host_t *n = ctx;
    uint8_t b[5];
    wbuf_t w = {b, 0, sizeof(b)};
    put32(&w, seq);
    put8(&w, more);
    out_ok(n, send_buf(n->write, n->wctx, ESL_NLINK_END, &w));
}

static bool send_config(esl_nlink_host_t *n)
{
    uint8_t b[4];
    wbuf_t w = {b, 0, sizeof(b)};
    put8(&w, n->channel);
    put16(&w, n->pan);
    if (n->tx_set)
        put8(&w, (uint8_t)n->tx_power);
    n->config_due = false;
    return out_ok(n, send_buf(n->write, n->wctx, ESL_NLINK_CONFIG, &w));
}

void esl_nlink_host_init(esl_nlink_host_t *n, esl_rhost_t *h, esl_tagfw_t *fw, uint8_t channel, uint16_t pan,
                         esl_nlink_write_t write, void *wctx, uint64_t (*now_ms)(void))
{
    memset(n, 0, sizeof(*n));
    n->h = h;
    n->fw = fw;
    n->channel = channel;
    n->pan = pan;
    n->write = write;
    n->wctx = wctx;
    n->now_ms = now_ms;
}

void esl_nlink_host_set_config(esl_nlink_host_t *n, uint8_t channel, uint16_t pan)
{
    n->channel = channel;
    n->pan = pan;
    n->config_due = true;
}

void esl_nlink_host_set_tx_power(esl_nlink_host_t *n, int8_t dbm)
{
    n->tx_power = dbm;
    n->tx_set = true;
    n->config_due = true;
}

static void answer(esl_nlink_host_t *n)
{
    const esl_rhost_out_t out = {n, host_clear, host_tag, host_drop, host_image, host_fw, host_default, host_end};
    if (n->config_due)
        send_config(n);
    esl_rhost_answer(n->h, &n->req, &out);
    n->in_sync = false;
}

static bool parse_sync(rbuf_t *g, esl_rsync_t *q)
{
    memset(q, 0, sizeof(*q));
    q->boot = get32(g);
    q->ack = get32(g);
    q->now_ms = get32(g);
    q->log_n = get16(g);
    q->log_dropped = get32(g);
    q->want = get8(g) != 0;
    q->want_mac = get64(g);
    q->want_image = get32(g);
    q->want_fw_model = get8(g);
    for (unsigned i = 0; i < ESL_RCACHE_WANTS; i++)
        q->wants[i] = get64(g);
    q->n_tags = get16(g);
    q->max_tags = get16(g);
    q->free_images = get16(g);
    q->n_images = get8(g);
    if (q->n_images > ESL_RSYNC_IMAGES)
        return false;
    for (unsigned i = 0; i < q->n_images; i++)
        q->images[i] = get32(g);
    unsigned nfw = get8(g);
    for (unsigned i = 0; i < nfw; i++) {
        uint8_t m = get8(g);
        uint16_t ver = get16(g), size = get16(g);
        uint32_t crc = get32(g);
        if (m >= 1 && m <= ESL_RCACHE_MODELS) {
            q->fw[m - 1].ver = ver;
            q->fw[m - 1].size = size;
            q->fw[m - 1].crc = crc;
        }
    }
    return !g->bad;
}

bool esl_nlink_host_on(esl_nlink_host_t *n, uint8_t type, const uint8_t *p, size_t len)
{
    rbuf_t g = {p, len, 0, false};
    switch (type) {
    case ESL_NLINK_HELLO: {
        uint8_t ver = get8(&g);
        n->radio.boot = get32(&g);
        n->radio.radio_ver = get16(&g);
        n->radio.id = get64(&g);
        n->radio.max_tags = get16(&g);
        n->radio.n_images = get16(&g);
        n->radio.image_cap = get32(&g);
        n->radio.fw_models = get8(&g);
        if (g.bad || ver != ESL_NLINK_VERSION)
            return false;
        n->hello = true;
        n->in_sync = false;
        n->h->fw_models = n->radio.fw_models;
        return send_config(n);
    }
    case ESL_NLINK_SYNC:
        if (!n->hello || n->in_sync || !parse_sync(&g, &n->req))
            return false;
        esl_rhost_begin(n->h, &n->req);
        n->in_sync = true;
        n->log_left = n->req.log_n;
        if (!n->log_left)
            answer(n);
        return !n->broken;
    case ESL_NLINK_LOG: {
        esl_rlog_t e;
        if (!n->in_sync || !n->log_left)
            return false;
        e.seq = get32(&g);
        e.t_ms = get32(&g);
        e.rssi = (int8_t)get8(&g);
        e.len = get8(&g);
        if (g.bad || e.len > ESL_PSDU_MAX || len - g.at != e.len)
            return false;
        memcpy(e.psdu, p + g.at, e.len);
        esl_rhost_replay(n->h, &n->req, &e, n->now_ms());
        if (--n->log_left == 0)
            answer(n);
        return !n->broken;
    }
    default:
        return true; /* a newer radio's: ignored */
    }
}
