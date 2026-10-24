#include "esl_rcache.h"
#include <string.h>

void esl_rcache_init(esl_rcache_t *rc, uint16_t pan, esl_rtag_t *tags, unsigned max_tags, esl_rimage_t *images,
                     unsigned n_images, size_t image_cap, esl_rlog_t *log, unsigned log_cap,
                     const esl_rcache_ops_t *ops)
{
    memset(rc, 0, sizeof(*rc));
    rc->pan = pan;
    rc->tags = tags;
    rc->max_tags = max_tags;
    rc->images = images;
    rc->n_images = n_images;
    rc->image_cap = image_cap;
    rc->log = log;
    rc->log_cap = log_cap;
    rc->default_poll_s = ESL_RCACHE_DEFAULT_POLL_S;
    if (ops)
        rc->ops = *ops;
    for (unsigned i = 0; i < n_images; i++) {
        images[i].id = 0;
        images[i].used = 0;
    }
}

/* ---- tags, images, firmware ---- */

static esl_rtag_t *find_tag(esl_rcache_t *rc, uint64_t mac)
{
    for (unsigned i = 0; i < rc->n_tags; i++)
        if (rc->tags[i].s.mac == mac)
            return &rc->tags[i];
    return NULL;
}

static bool same_state(const esl_rtag_state_t *a, const esl_rtag_state_t *b)
{
    return a->next_poll_s == b->next_poll_s && a->has_cfg == b->has_cfg &&
           (!a->has_cfg || esl_tag_cfg_version(&a->cfg) == esl_tag_cfg_version(&b->cfg)) &&
           a->image_id == b->image_id && a->fw_ver == b->fw_ver && a->image_failed == b->image_failed &&
           a->fw_failed == b->fw_failed;
}

bool esl_rcache_set_tag(esl_rcache_t *rc, const esl_rtag_state_t *s)
{
    esl_rtag_t *t = find_tag(rc, s->mac);
    if (!t) {
        if (rc->n_tags >= rc->max_tags)
            return false;
        t = &rc->tags[rc->n_tags++];
        memset(t, 0, sizeof(*t));
    } else if (!same_state(&t->s, s)) {
        t->waits = 0;
    }
    t->s = *s;
    /* refusals the host knows of (from before a restart of the radio) */
    if (s->image_failed)
        t->image_failed = s->image_failed;
    if (s->fw_failed)
        t->fw_failed = s->fw_failed;
    return true;
}

void esl_rcache_set_default_poll(esl_rcache_t *rc, uint16_t poll_s)
{
    rc->default_poll_s = poll_s ? poll_s : ESL_RCACHE_DEFAULT_POLL_S;
}

void esl_rcache_drop_tag(esl_rcache_t *rc, uint64_t mac)
{
    esl_rtag_t *t = find_tag(rc, mac);
    if (t)
        *t = rc->tags[--rc->n_tags];
}

static esl_rimage_t *find_image(const esl_rcache_t *rc, uint32_t id)
{
    if (!id)
        return NULL;
    for (unsigned i = 0; i < rc->n_images; i++)
        if (rc->images[i].id == id)
            return &rc->images[i];
    return NULL;
}

bool esl_rcache_has_image(const esl_rcache_t *rc, uint32_t id)
{
    return find_image(rc, id) != NULL;
}

static bool image_named(const esl_rcache_t *rc, uint32_t id)
{
    for (unsigned i = 0; i < rc->n_tags; i++)
        if (rc->tags[i].s.image_id == id)
            return true;
    return false;
}

bool esl_rcache_put_image(esl_rcache_t *rc, uint32_t id, uint8_t model, uint8_t fmt, const uint8_t *data,
                          size_t size)
{
    if (!id || size == 0 || size > rc->image_cap || size > 0xFFFF || !rc->n_images)
        return false;
    esl_rimage_t *im = find_image(rc, id);
    if (!im) {
        /* a free slot, else the one used the longest ago that no tag needs, else the
           oldest, but not the one a tag is fetching */
        esl_rimage_t *unneeded = NULL, *oldest = NULL;
        for (unsigned i = 0; i < rc->n_images && !im; i++) {
            esl_rimage_t *c = &rc->images[i];
            if (!c->id)
                im = c;
            else if (rc->mid_blocks && c->id == rc->mid_image && rc->n_images > 1)
                continue;
            else if (!image_named(rc, c->id) && (!unneeded || c->used < unneeded->used))
                unneeded = c;
            else if (!oldest || c->used < oldest->used)
                oldest = c;
        }
        if (!im)
            im = unneeded ? unneeded : oldest;
    }
    memcpy(im->data, data, size);
    im->id = id;
    im->size = (uint16_t)size;
    im->model = model;
    im->fmt = fmt;
    im->used = ++rc->clock;
    return true;
}

static const esl_rfw_t *fw_for(const esl_rcache_t *rc, uint8_t model)
{
    if (model < 1 || model > ESL_RCACHE_MODELS || !rc->fw[model - 1].ver)
        return NULL;
    return &rc->fw[model - 1];
}

void esl_rcache_set_fw(esl_rcache_t *rc, uint8_t model, uint16_t ver, uint16_t size, uint32_t crc)
{
    if (model < 1 || model > ESL_RCACHE_MODELS)
        return;
    esl_rfw_t *f = &rc->fw[model - 1];
    f->ver = size ? ver : 0;
    f->size = size;
    f->crc = crc;
}

/* ---- the log ---- */

static void log_frame(esl_rcache_t *rc, const uint8_t *psdu, size_t len, int8_t rssi, uint64_t now_ms)
{
    if (!rc->log_cap)
        return;
    if (rc->log_len == rc->log_cap) {
        /* the host is far behind: the oldest goes, so what it gets is current */
        rc->log_head = (rc->log_head + 1) % rc->log_cap;
        rc->log_len--;
        rc->stats.log_dropped++;
    }
    esl_rlog_t *e = &rc->log[(rc->log_head + rc->log_len) % rc->log_cap];
    e->t_ms = (uint32_t)now_ms;
    e->rssi = rssi;
    e->len = (uint8_t)len;
    memcpy(e->psdu, psdu, len);
    rc->log_len++;
}

unsigned esl_rcache_log_count(const esl_rcache_t *rc)
{
    return rc->log_len;
}

const esl_rlog_t *esl_rcache_log_at(const esl_rcache_t *rc, unsigned i)
{
    if (i >= rc->log_len)
        return NULL;
    return &rc->log[(rc->log_head + i) % rc->log_cap];
}

void esl_rcache_log_drop(esl_rcache_t *rc, unsigned n)
{
    if (n > rc->log_len)
        n = rc->log_len;
    rc->log_head = rc->log_cap ? (rc->log_head + n) % rc->log_cap : 0;
    rc->log_len -= n;
}

/* ---- replies ---- */

static void want(esl_rcache_t *rc, uint64_t mac, uint32_t image, uint8_t fw_model)
{
    rc->want = true;
    rc->want_mac = mac;
    if (image)
        rc->want_image = image;
    if (fw_model)
        rc->want_fw_model = fw_model;
}

/* Block pos (block 0 when it is past the end) of image im: an IMG message into out. */
static size_t image_block(esl_rcache_t *rc, esl_rimage_t *im, uint16_t pos, uint8_t max, uint16_t next,
                          uint8_t *out)
{
    if (pos >= im->size)
        pos = 0;
    size_t n = im->size - pos;
    if (n > max)
        n = max;
    if (n > ESL_IMG_CHUNK_MAX)
        n = ESL_IMG_CHUNK_MAX;
    im->used = ++rc->clock;
    rc->mid_blocks = pos + n < im->size;
    rc->mid_image = im->id;
    return esl_img_msg_build(out, next, im->id, im->size, pos, im->model, im->fmt, im->data + pos, n);
}

/* Likewise for the firmware f of model; 0 when it cannot be read. */
static size_t fw_block(esl_rcache_t *rc, const esl_rfw_t *f, uint8_t model, uint16_t pos, uint8_t max,
                       uint16_t next, uint8_t *out)
{
    uint8_t data[ESL_FW_CHUNK_MAX];
    if (pos >= f->size)
        pos = 0;
    size_t n = f->size - pos;
    if (n > max)
        n = max;
    if (n > ESL_FW_CHUNK_MAX)
        n = ESL_FW_CHUNK_MAX;
    if (!rc->ops.read_fw || !rc->ops.read_fw(rc->ops.ctx, model, pos, data, n))
        return 0;
    rc->mid_blocks = pos + n < f->size;
    rc->mid_image = 0;
    return esl_fw_msg_build(out, next, f->ver, f->size, pos, f->crc, model, data, n);
}

/* IDLE "come back soon" while the radio waits for the host, ESL_RCACHE_WAIT_TRIES
   times in a row, then next: the host may not be able to deliver, and a tag woken
   every few seconds empties its battery. *waits counts them. */
static size_t wait_reply(esl_rcache_t *rc, uint8_t *waits, uint16_t next, uint8_t *out)
{
    if (*waits >= ESL_RCACHE_WAIT_TRIES)
        return esl_idle_build(out, next);
    (*waits)++;
    rc->stats.waits++;
    return esl_idle_build(out, ESL_RCACHE_WAIT_S);
}

/* The wait count of a tag the radio has no state for. */
static uint8_t *unknown_waits(esl_rcache_t *rc, uint64_t mac)
{
    for (unsigned i = 0; i < ESL_RCACHE_UNKNOWN; i++)
        if (rc->unknown[i].mac == mac)
            return &rc->unknown[i].waits;
    unsigned i = rc->unknown_next++ % ESL_RCACHE_UNKNOWN;
    rc->unknown[i].mac = mac;
    rc->unknown[i].waits = 0;
    return &rc->unknown[i].waits;
}

static size_t on_poll(esl_rcache_t *rc, esl_rtag_t *t, uint64_t mac, const esl_poll_t *poll, uint8_t *out)
{
    if (!t) {
        /* a tag the host has not told it about (new, or the radio restarted); once it
           has waited enough, the default interval */
        if (rc->n_tags >= rc->max_tags) {
            /* no room for it here whatever the host says: no wait, and no early sync */
            return esl_idle_build(out, rc->default_poll_s);
        }
        want(rc, mac, 0, 0);
        return wait_reply(rc, unknown_waits(rc, mac), rc->default_poll_s, out);
    }
    uint16_t next = t->s.next_poll_s;

    /* The refusals, as esl_store_fw_offer() and esl_store_image_offer() note them:
       only about an image or version the tag had started on */
    if ((poll->flags & ESL_POLL_F_DRAWFAIL) && t->image_taken)
        t->image_failed = t->image_taken;
    if (t->s.image_id && poll->content_id == t->s.image_id)
        t->image_failed = 0;
    if ((poll->flags & ESL_POLL_F_FWFAIL) && t->fw_taken)
        t->fw_failed = t->fw_taken;
    if (t->s.fw_ver && poll->fw_ver == t->s.fw_ver)
        t->fw_failed = 0;

    if (t->s.has_cfg && poll->has_cfg_ver && poll->cfg_ver != esl_tag_cfg_version(&t->s.cfg))
        return esl_config_msg_build(out, next, &t->s.cfg);

    uint32_t id = t->s.image_id;
    if (id && (poll->flags & ESL_POLL_F_IMAGE) && poll->content_id != id && t->image_failed != id) {
        esl_rimage_t *im = find_image(rc, id);
        if (!im) {
            want(rc, mac, id, 0);
            return wait_reply(rc, &t->waits, next, out);
        }
        t->waits = 0;
        t->image_taken = 0;
        rc->mid_mac = mac;
        return image_block(rc, im, 0, ESL_IMG_CHUNK_MAX, next, out);
    }

    uint16_t ver = t->s.fw_ver;
    if (ver && (poll->flags & ESL_POLL_F_OTA) && poll->fw_ver != ver && t->fw_failed != ver) {
        const esl_rfw_t *f = fw_for(rc, poll->model);
        if (!f || f->ver != ver) {
            want(rc, mac, 0, poll->model);
            return wait_reply(rc, &t->waits, next, out);
        }
        size_t n = fw_block(rc, f, poll->model, 0, ESL_FW_CHUNK_MAX, next, out);
        if (n) {
            rc->mid_mac = mac;
            t->waits = 0;
            t->fw_taken = 0;
            return n;
        }
    }
    t->waits = 0;
    return esl_idle_build(out, next);
}

size_t esl_rcache_handle(esl_rcache_t *rc, const uint8_t *psdu, size_t psdu_len, int8_t rssi, uint64_t now_ms,
                         uint8_t *tx)
{
    esl_rx_frame_t f;
    esl_poll_t poll;
    esl_img_req_t ireq;
    esl_fw_req_t freq;
    uint8_t reply[ESL_MAX_PAYLOAD];
    size_t n = 0;

    if (psdu_len > ESL_PSDU_MAX || !esl_frame_parse_from_tag(psdu, psdu_len, &f) || f.dst_pan != rc->pan ||
        f.dst_short != ESL_AP_SHORT_ADDR)
        return 0;
    log_frame(rc, psdu, psdu_len, rssi, now_ms);
    rc->last_rx_ms = now_ms;
    /* a frame from the tag that was fetching: its reply says whether it still is; another
       tag's POLL in between leaves it be */
    if (f.src_mac == rc->mid_mac)
        rc->mid_blocks = false;
    esl_rtag_t *t = find_tag(rc, f.src_mac);

    if (esl_img_req_parse(f.payload, f.payload_len, &ireq)) {
        rc->stats.blocks++;
        esl_rimage_t *im = t ? find_image(rc, t->s.image_id) : NULL;
        if (im) {
            if (ireq.id == im->id)
                t->image_taken = im->id;
            n = image_block(rc, im, ireq.id == im->id ? ireq.pos : 0, ireq.max, t->s.next_poll_s, reply);
            rc->mid_mac = f.src_mac;
        } else {
            if (!t || t->s.image_id)
                want(rc, f.src_mac, t ? t->s.image_id : 0, 0);
            n = esl_idle_build(reply, t ? t->s.next_poll_s : rc->default_poll_s);
        }
    } else if (esl_fw_req_parse(f.payload, f.payload_len, &freq)) {
        rc->stats.blocks++;
        const esl_rfw_t *fw = fw_for(rc, freq.model);
        if (t && fw && t->s.fw_ver == fw->ver) {
            bool same = freq.ver == fw->ver && freq.crc == fw->crc;
            if (same)
                t->fw_taken = fw->ver;
            n = fw_block(rc, fw, freq.model, same ? freq.pos : 0, freq.max, t->s.next_poll_s, reply);
            if (n)
                rc->mid_mac = f.src_mac;
        }
        if (!n) {
            if (t && t->s.fw_ver)
                want(rc, f.src_mac, 0, freq.model);
            n = esl_idle_build(reply, t ? t->s.next_poll_s : rc->default_poll_s);
        }
    } else if (esl_poll_parse(f.payload, f.payload_len, &poll)) {
        rc->stats.polls++;
        n = on_poll(rc, t, f.src_mac, &poll, reply);
    } else {
        return 0;
    }
    return esl_frame_build_to_tag(tx, f.seq, rc->pan, ESL_AP_SHORT_ADDR, f.src_mac, reply, n);
}

bool esl_rcache_sync_due(const esl_rcache_t *rc, uint64_t now_ms, uint64_t last_sync_ms)
{
    /* wraps to a large number when last_sync_ms is later than now_ms: due at once */
    uint64_t since = now_ms - last_sync_ms;
    if (since >= ESL_RCACHE_SYNC_MAX_MS)
        return true;
    uint64_t quiet = rc->mid_blocks ? ESL_RCACHE_BLOCKS_MS : ESL_RCACHE_QUIET_MS;
    if (rc->last_rx_ms && now_ms - rc->last_rx_ms < quiet)
        return false;
    return since >= (rc->want ? ESL_RCACHE_SOON_MS : ESL_RCACHE_SYNC_MS);
}

void esl_rcache_want_clear(esl_rcache_t *rc)
{
    rc->want = false;
    rc->want_mac = 0;
    rc->want_image = 0;
    rc->want_fw_model = 0;
}
