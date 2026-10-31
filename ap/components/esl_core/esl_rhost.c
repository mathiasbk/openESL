#include "esl_rhost.h"
#include "esl_clock.h"
#include "esl_proto.h"
#include "esl_radio.h"
#include <string.h>

void esl_rhost_init(esl_rhost_t *h, esl_store_t *st, esl_layouts_t *ls, esl_image_t *im, esl_tagfw_t *fw,
                    uint16_t pan, esl_rtag_state_t *sent, unsigned max_sent, uint8_t *img, size_t img_cap)
{
    memset(h, 0, sizeof(*h));
    h->st = st;
    h->ls = ls;
    h->im = im;
    h->fw = fw;
    h->pan = pan;
    h->sent = sent;
    h->max_sent = max_sent;
    h->img = img;
    h->img_cap = img_cap;
    h->max_draws = ESL_RHOST_DRAWS;
    h->max_images = ESL_RHOST_IMAGES;
    memset(sent, 0, max_sent * sizeof(sent[0]));
}

void esl_rowners_init(esl_rowners_t *o, esl_rowner_t *by_slot, unsigned n)
{
    o->by_slot = by_slot;
    o->n = n;
    o->radios = 0;
    memset(by_slot, 0, n * sizeof(by_slot[0]));
}

uint8_t esl_rowners_get(const esl_rowners_t *o, esl_store_t *st, uint64_t mac)
{
    int slot = esl_store_slot_of(st, mac);
    if (!o || slot < 0 || (unsigned)slot >= o->n || o->by_slot[slot].mac != mac)
        return ESL_RHOST_NONE;
    return o->by_slot[slot].radio;
}

unsigned esl_rowners_count(const esl_rowners_t *o, uint8_t radio)
{
    unsigned n = 0;
    for (unsigned i = 0; i < o->n; i++)
        n += o->by_slot[i].mac && o->by_slot[i].radio == radio;
    return n;
}

void esl_rowners_forget(esl_rowners_t *o, uint8_t radio)
{
    /* the store's side (esl_tag_t.radio) goes with esl_store_set_radio_channel(radio, 0) */
    for (unsigned i = 0; i < o->n; i++)
        if (o->by_slot[i].radio == radio)
            memset(&o->by_slot[i], 0, sizeof(o->by_slot[i]));
}

/* = CFG_SCAN_CHANNELS (tag/src/config.h; test_rhost.c checks they match) */
static const uint8_t scan_order[] = {15, 20, 26, 11, 12, 13, 14, 16, 17, 18, 19, 21, 22, 23, 24, 25};

uint8_t esl_rhost_pick_channel(uint32_t taken)
{
    for (unsigned i = 0; i < sizeof(scan_order); i++)
        if (!(taken >> scan_order[i] & 1))
            return scan_order[i];
    return 0;
}

void esl_rhost_share(esl_rhost_t *h, esl_rowners_t *owners, uint8_t index)
{
    h->owners = owners;
    h->index = index;
}

/* The tag in the frame was heard by this radio at at: it owns the tag, unless another
   radio heard it later (their logs come in at their own syncs). */
static void own(esl_rhost_t *h, const uint8_t *psdu, size_t len, uint64_t at)
{
    esl_rx_frame_t f;
    if (!h->owners || !esl_frame_parse_from_tag(psdu, len, &f) || f.dst_pan != h->pan)
        return;
    int slot = esl_store_slot_of(h->st, f.src_mac);
    if (slot < 0 || (unsigned)slot >= h->owners->n)
        return;
    esl_rowner_t *o = &h->owners->by_slot[slot];
    if (o->mac == f.src_mac && o->radio != h->index && o->at_ms > at)
        return;
    o->mac = f.src_mac;
    o->at_ms = at;
    o->radio = h->index;
    esl_store_set_tag_radio(h->st, f.src_mac, h->index);
}

/* Whether this radio serves the tag: it owns it, nobody does yet, or it asked for it. */
static bool serves(const esl_rhost_t *h, const esl_rsync_t *req, unsigned slot, uint64_t mac, bool *owned)
{
    *owned = !h->owners;
    if (!h->owners || slot >= h->owners->n)
        return true;
    const esl_rowner_t *o = &h->owners->by_slot[slot];
    if (o->mac != mac)
        return true; /* nobody's yet */
    *owned = o->radio == h->index;
    if (*owned || mac == req->want_mac)
        return true;
    for (unsigned i = 0; i < ESL_RCACHE_WANTS; i++)
        if (req->wants[i] == mac)
            return true;
    return false;
}

void esl_rhost_begin(esl_rhost_t *h, const esl_rsync_t *req)
{
    bool restarted = !h->have_radio || req->boot != h->radio_boot;
    /* a radio that has room now but fewer tags than it was told: it had no room for some
       (it does not say which), so it gets them all again */
    unsigned told = 0;
    for (unsigned i = 0; i < h->max_sent; i++)
        told += h->sent[i].mac != 0;
    bool missing = req->n_tags < told && req->n_tags < req->max_tags;
    if (restarted || req->ack != h->seq || missing) {
        /* a radio that restarted (or one the host does not know: it restarted), one that
           missed the last answer, or one missing tags: it forgets its tags and is told
           everything */
        memset(h->sent, 0, h->max_sent * sizeof(h->sent[0]));
        h->default_sent = 0;
        h->clear = true;
        h->stats.resets++;
    }
    if (restarted) {
        h->have_radio = true;
        h->radio_boot = req->boot;
        h->replayed_any = false; /* its log counts from 0 again */
    }
    h->stats.radio_log_dropped = req->log_dropped;
}

void esl_rhost_replay(esl_rhost_t *h, const esl_rsync_t *req, const esl_rlog_t *e, uint64_t now_ms)
{
    if (h->replayed_any && (int32_t)(e->seq - h->replay_next) < 0) {
        h->stats.skipped++; /* sent again after an answer it did not get */
        return;
    }
    h->replayed_any = true;
    h->replay_next = e->seq + 1;
    uint32_t age = req->now_ms - e->t_ms; /* the radio's clock, wrapping at 32 bits */
    uint64_t at = age < now_ms ? now_ms - age : 0;
    esl_radio_replay(h->st, h->ls, h->pan, e->psdu, e->len, e->rssi, at);
    own(h, e->psdu, e->len, at); /* after the replay: a new tag has a slot now */
    h->stats.replayed++;
}

static bool same(const esl_rtag_state_t *a, const esl_rtag_state_t *b)
{
    return a->mac == b->mac && a->next_poll_s == b->next_poll_s && a->has_cfg == b->has_cfg &&
           (!a->has_cfg || esl_tag_cfg_version(&a->cfg) == esl_tag_cfg_version(&b->cfg)) &&
           a->image_id == b->image_id && a->fw_ver == b->fw_ver && a->image_failed == b->image_failed &&
           a->fw_failed == b->fw_failed;
}

/* The tag's image drawn into h->img; false when it has none (or the draws are used up). */
static bool draw(esl_rhost_t *h, uint64_t mac, unsigned *draws)
{
    if (!*draws || !h->im)
        return false;
    (*draws)--;
    h->img_id = 0;
    size_t n = esl_image_draw(h->im, h->st, h->ls, mac, h->img, h->img_cap, &h->img_id, &h->img_model, &h->img_fmt);
    h->stats.drawn++;
    h->img_size = (uint16_t)n;
    if (!n)
        h->img_id = 0;
    return n != 0;
}

/* The id of the image the tag should show, as esl_image_prepare() gives it: known
   when it was drawn for the content and layout file it has, else drawn now. False
   when it has to be drawn and the draws are used up. */
static bool image_id(esl_rhost_t *h, const esl_tag_t *t, unsigned *draws, uint32_t *id)
{
    *id = 0;
    if (!t->image_capable || !t->has_content || t->content_err || !t->model || !h->im)
        return true;
    if (t->image_id && t->image_layout_ver == esl_layouts_image_ver(h->ls, t->model, t->layout_hash)) {
        *id = t->image_id;
        return true;
    }
    /* a layout with date fields is not drawn while the time is unknown (esl_image.c draw()):
       no draw is spent on it, so the others are not held up */
    int32_t today;
    if (!esl_clock_today(&today) && esl_layouts_clock(h->ls, t->model, t->layout_hash) != ESL_CLOCK_NONE)
        return true;
    if (!*draws)
        return false;
    if (draw(h, t->mac, draws))
        *id = h->img_id;
    return true;
}

/* What esl_radio_handle() would answer the tag with: false when its image has to be
   drawn first and the draws are used up. */
static bool state(esl_rhost_t *h, const esl_tag_t *t, unsigned *draws, esl_rtag_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->mac = t->mac;
    s->next_poll_s = esl_store_tag_poll_s(h->st, t->mac);
    /* a tag keeps the last interval it got when a reply has 0, and from the radio that
       may have been ESL_RCACHE_WAIT_S: it is told its interval outright */
    if (!s->next_poll_s)
        s->next_poll_s = ESL_TAG_BUILTIN_POLL_S;
    s->has_cfg = t->cfg_capable;
    esl_tag_cfg_t ap;
    esl_store_tag_defaults(h->st, &ap);
    ap = esl_store_tag_ap(h->st, t, &ap); /* the channel of the radio that serves it */
    esl_tag_effective_cfg(t, &ap, &s->cfg);
    if (!image_id(h, t, draws, &s->image_id))
        return false;
    esl_tagfw_img_t img;
    if (h->fw && t->model && esl_tagfw_get(h->fw, t->model, &img) && esl_tagfw_for(h->fw, t->model, t->mac))
        s->fw_ver = img.ver;
    s->image_failed = t->image_failed;
    s->fw_failed = t->fw_failed;
    return true;
}

/* What one answer sends, and what the radio is known to hold. */
typedef struct {
    const esl_rhost_out_t *out;
    unsigned draws, images, free;
    uint32_t have[ESL_RSYNC_IMAGES + ESL_RHOST_IMAGES * 2];
    unsigned n_have;
    bool more;
} answer_t;

static bool held(const answer_t *a, uint32_t id)
{
    for (unsigned i = 0; i < a->n_have; i++)
        if (a->have[i] == id)
            return true;
    return false;
}

/* The tag needs its image at the radio: it does not show it, has not refused it, and
   the radio lacks it. */
static bool needs_image(const answer_t *a, const esl_tag_t *t, uint32_t id)
{
    return id && t->shown_content_id != id && t->image_failed != id && !held(a, id);
}

/* Sends the tag's image (id, as its state names it). The image drawn now may have
   another id (the tag's flags changed what format it takes): *id is then the new one.
   False when it could not be drawn or sent. */
static bool send_image(esl_rhost_t *h, answer_t *a, uint64_t mac, uint32_t *id)
{
    if (!a->images || a->n_have >= sizeof(a->have) / sizeof(a->have[0])) {
        a->more = true;
        return false;
    }
    if (h->img_id != *id && !draw(h, mac, &a->draws)) {
        a->more = a->more || a->draws == 0;
        return false;
    }
    *id = h->img_id;
    a->images--;
    if (a->free)
        a->free--;
    a->have[a->n_have++] = h->img_id;
    if (!a->out->image(a->out->ctx, h->img_id, h->img_model, h->img_fmt, h->img, h->img_size)) {
        h->stats.refused++;
        return false;
    }
    h->stats.images++;
    return true;
}

void esl_rhost_answer(esl_rhost_t *h, const esl_rsync_t *req, const esl_rhost_out_t *out)
{
    answer_t a;
    bool fw_needed[ESL_RCACHE_MODELS] = {false};
    memset(&a, 0, sizeof(a));
    a.out = out;
    a.draws = h->max_draws;
    a.images = h->max_images;
    a.free = req->free_images;
    for (unsigned i = 0; i < req->n_images && a.n_have < ESL_RSYNC_IMAGES; i++)
        a.have[a.n_have++] = req->images[i];
    h->stats.syncs++;
    h->img_id = 0;

    if (h->clear) {
        out->clear(out->ctx);
        h->clear = false;
    }
    uint16_t def = esl_store_get_next_poll(h->st);
    if (def != h->default_sent) {
        out->default_poll(out->ctx, def);
        h->default_sent = def;
    }

    /* the images of the tags it lacked something for first: they are awake now */
    for (unsigned i = 0; i < ESL_RCACHE_WANTS; i++) {
        esl_tag_t t;
        esl_rtag_state_t s;
        if (req->wants[i] && esl_store_get(h->st, req->wants[i], &t) && state(h, &t, &a.draws, &s) &&
            needs_image(&a, &t, s.image_id))
            send_image(h, &a, t.mac, &s.image_id);
    }

    unsigned slots = esl_store_slots(h->st);
    if (slots > h->max_sent)
        slots = h->max_sent;
    for (unsigned slot = 0; slot < slots; slot++) {
        esl_rtag_state_t *prev = &h->sent[slot], s;
        esl_tag_t t;
        bool used = esl_store_get_slot(h->st, slot, &t);
        if (prev->mac && (!used || prev->mac != t.mac)) {
            out->drop(out->ctx, prev->mac);
            h->stats.drops++;
            memset(prev, 0, sizeof(*prev));
        }
        if (!used)
            continue;
        bool owned;
        if (!serves(h, req, slot, t.mac, &owned)) {
            /* another radio has it now */
            if (prev->mac) {
                out->drop(out->ctx, prev->mac);
                h->stats.drops++;
                memset(prev, 0, sizeof(*prev));
            }
            continue;
        }
        if (!state(h, &t, &a.draws, &s)) {
            a.more = true;
            continue;
        }
        if (s.fw_ver && t.model <= ESL_RCACHE_MODELS)
            fw_needed[t.model - 1] = true;
        /* its image ahead of the tag, while the radio has a slot to spare for it */
        if (needs_image(&a, &t, s.image_id) && a.free && (owned || h->owners->radios <= 1))
            send_image(h, &a, t.mac, &s.image_id);
        /* the tag the radio asked for is sent again even if it seems to have it */
        if (!same(prev, &s) || t.mac == req->want_mac) {
            if (out->tag(out->ctx, &s)) {
                *prev = s;
                h->stats.tags++;
            } else {
                h->stats.refused++;
            }
        }
    }

    /* tag firmware: the model's image where a tag is to get it, or the radio asked; none
       where the host has none */
    for (uint8_t m = 1; m <= ESL_RCACHE_MODELS; m++) {
        esl_tagfw_img_t img;
        if (h->fw_models && m > h->fw_models)
            break; /* it has nowhere to keep it, and would ask for it at every sync */
        const esl_rfw_t *r = &req->fw[m - 1];
        bool has = h->fw && esl_tagfw_get(h->fw, m, &img);
        if (!has) {
            if (r->ver) {
                memset(&img, 0, sizeof(img));
                out->fw(out->ctx, m, &img);
                h->stats.fws++;
            }
        } else if ((fw_needed[m - 1] || req->want_fw_model == m) && (r->ver != img.ver || r->crc != img.crc)) {
            out->fw(out->ctx, m, &img);
            h->stats.fws++;
        }
    }
    out->end(out->ctx, ++h->seq, a.more);
}
