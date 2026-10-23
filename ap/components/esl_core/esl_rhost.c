#include "esl_rhost.h"
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
    esl_rhost_reset(h);
}

void esl_rhost_reset(esl_rhost_t *h)
{
    memset(h->sent, 0, h->max_sent * sizeof(h->sent[0]));
    h->default_sent = 0;
}

void esl_rhost_replay(esl_rhost_t *h, const esl_rsync_t *req, const esl_rlog_t *e, uint64_t now_ms)
{
    uint8_t tx[ESL_PSDU_MAX];
    uint32_t age = req->now_ms - e->t_ms; /* the radio's clock, wrapping at 32 bits */
    uint64_t at = age < now_ms ? now_ms - age : 0;
    esl_radio_handle(h->st, h->ls, h->pan, e->psdu, e->len, e->rssi, at, tx, NULL);
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
    uint32_t ver = 0;
    if (!esl_layouts_info(h->ls, t->model, &ver, NULL, NULL))
        ver = 0;
    if (t->image_id && t->image_layout_ver == ver) {
        *id = t->image_id;
        return true;
    }
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
    esl_tag_effective_cfg(t, esl_store_get_next_poll(h->st), &s->cfg);
    if (!image_id(h, t, draws, &s->image_id))
        return false;
    esl_tagfw_img_t img;
    if (h->fw && t->model && esl_tagfw_get(h->fw, t->model, &img) && esl_tagfw_for(h->fw, t->model, t->mac))
        s->fw_ver = img.ver;
    s->image_failed = t->image_failed;
    s->fw_failed = t->fw_failed;
    return true;
}

static bool held(const uint32_t *ids, unsigned n, uint32_t id)
{
    for (unsigned i = 0; i < n; i++)
        if (ids[i] == id)
            return true;
    return false;
}

bool esl_rhost_answer(esl_rhost_t *h, const esl_rsync_t *req, uint64_t now_ms, const esl_rhost_out_t *out)
{
    (void)now_ms;
    unsigned draws = h->max_draws, images = h->max_images;
    bool more = false;
    uint32_t have[ESL_RSYNC_IMAGES + ESL_RHOST_IMAGES * 2];
    unsigned n_have = 0;
    bool fw_needed[ESL_RCACHE_MODELS] = {false};

    h->stats.syncs++;
    if (req->n_tags == 0)
        esl_rhost_reset(h); /* it has nothing: it restarted */
    for (unsigned i = 0; i < req->n_images && n_have < ESL_RSYNC_IMAGES; i++)
        have[n_have++] = req->images[i];
    h->img_id = 0;

    uint16_t def = esl_store_get_next_poll(h->st);
    if (def != h->default_sent) {
        out->default_poll(out->ctx, def);
        h->default_sent = def;
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
        if (!state(h, &t, &draws, &s)) {
            more = true;
            continue;
        }
        if (s.fw_ver && t.model <= ESL_RCACHE_MODELS)
            fw_needed[t.model - 1] = true;
        /* the tag the radio asked for is sent again even if it seems to have it */
        if (!same(prev, &s) || t.mac == req->want_mac) {
            if (out->tag(out->ctx, &s)) {
                *prev = s;
                h->stats.tags++;
            } else {
                h->stats.refused++;
                continue;
            }
        }
        /* its image, when the radio lacks it */
        if (s.image_id && !held(have, n_have, s.image_id)) {
            if (!images || n_have >= sizeof(have) / sizeof(have[0])) {
                more = true;
                continue;
            }
            if (h->img_id != s.image_id && !draw(h, t.mac, &draws)) {
                more = true;
                continue;
            }
            if (h->img_id != s.image_id)
                continue; /* drawn otherwise now (the content changed meanwhile): next time */
            images--;
            have[n_have++] = s.image_id;
            if (out->image(out->ctx, h->img_id, h->img_model, h->img_fmt, h->img, h->img_size))
                h->stats.images++;
            else
                h->stats.refused++;
        }
    }

    /* tag firmware: the model's image where a tag is to get it, or the radio asked; none
       where the host has none */
    for (uint8_t m = 1; m <= ESL_RCACHE_MODELS; m++) {
        esl_tagfw_img_t img;
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
    return more;
}
