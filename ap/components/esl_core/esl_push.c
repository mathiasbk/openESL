#include "esl_push.h"
#include <string.h>
#include "esl_proto.h"

/* See include/esl_push.h and docs/ble-tags.md */

void esl_push_init(esl_push_t *p, const esl_push_out_t *out, uint8_t *buf, size_t cap)
{
    memset(p, 0, sizeof(*p));
    p->out = *out;
    p->buf = buf;
    p->cap = cap;
}

static esl_push_tag_t *find(esl_push_t *p, uint64_t mac)
{
    for (unsigned i = 0; mac && i < ESL_PUSH_TAGS; i++)
        if (p->tags[i].mac == mac)
            return &p->tags[i];
    return NULL;
}

bool esl_push_add(esl_push_t *p, uint64_t mac, uint8_t model, uint32_t shown)
{
    if (!mac)
        return false;
    esl_push_tag_t *t = find(p, mac);
    if (t) {
        t->model = model;
        return true;
    }
    for (unsigned i = 0; !t && i < ESL_PUSH_TAGS; i++)
        if (!p->tags[i].mac)
            t = &p->tags[i];
    if (!t)
        return false;
    memset(t, 0, sizeof(*t));
    t->mac = mac;
    t->model = model;
    t->shown = shown;
    return true;
}

bool esl_push_remove(esl_push_t *p, uint64_t mac)
{
    esl_push_tag_t *t = find(p, mac);
    if (t)
        memset(t, 0, sizeof(*t));
    return t != NULL;
}

const esl_push_tag_t *esl_push_get(const esl_push_t *p, uint64_t mac)
{
    return find((esl_push_t *)p, mac);
}

unsigned esl_push_count(const esl_push_t *p)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ESL_PUSH_TAGS; i++)
        n += p->tags[i].mac != 0;
    return n;
}

/* The store is told about the tag as if it had polled: its model, battery and the image
   it shows. A tag that takes images (ESL_POLL_F_IMAGE) reports that image's id as what
   it shows, as tag/src/app.c does; no cfg_ver, so it is never sent CONFIG. The reply is
   not used: this AP talks to the tag only through the transport. */
static void note(esl_store_t *st, esl_layouts_t *ls, esl_push_tag_t *t, int8_t rssi, uint64_t now_ms)
{
    if (rssi)
        t->rssi = rssi;
    rssi = t->rssi; /* the last one known: an unknown signal is not a perfect 0 dBm */
    esl_poll_t poll;
    esl_layout_env_t env;
    uint8_t reply[ESL_MAX_PAYLOAD];
    memset(&poll, 0, sizeof(poll));
    poll.battery_mv = t->battery_mv;
    poll.content_id = t->shown;
    poll.flags = ESL_POLL_F_IMAGE;
    poll.model = t->model;
    esl_layouts_env(ls, t->model, &env);
    esl_store_on_poll(st, t->mac, &poll, rssi, now_ms, &env, reply);
    esl_store_check_tag(st, ls, t->mac);
}

bool esl_push_seen(esl_push_t *p, esl_store_t *st, esl_layouts_t *ls, uint64_t mac, int8_t rssi,
                   uint16_t battery_mv, uint64_t now_ms)
{
    esl_push_tag_t *t = find(p, mac);
    if (!t)
        return false;
    if (battery_mv)
        t->battery_mv = battery_mv;
    if (t->fails && !t->woken) {
        /* It is there again: one try at the next round, not one a round. Each try blocks the
           push task for a connection attempt, and a tag that is heard but cannot be connected
           to (busy, weak, refusing) would otherwise be tried every round */
        t->next_ms = now_ms;
        t->woken = true;
    }
    if (!t->noted_ms || now_ms - t->noted_ms >= ESL_PUSH_SEEN_MS) {
        t->noted_ms = now_ms;
        note(st, ls, t, rssi, now_ms);
    } else if (rssi) {
        t->rssi = rssi;
    }
    return true;
}

/* Whether the tag's content gives another image than the one it shows (or refused) */
static bool needs(esl_store_t *st, esl_layouts_t *ls, const esl_push_tag_t *pt)
{
    esl_tag_t t;
    if (!esl_store_get(st, pt->mac, &t) || !t.has_content || t.content_err)
        return false;
    if (!t.image_id) /* not drawn since its content was set */
        return true;
    if (t.image_layout_ver != esl_layouts_image_ver(ls, t.model, t.layout_hash))
        return true; /* the layout file changed: drawn again, and sent if that gives another image */
    return t.image_id != pt->shown && t.image_id != pt->refused;
}

static uint64_t backoff_ms(uint8_t fails)
{
    uint64_t ms = ESL_PUSH_RETRY_MS;
    for (uint8_t i = 1; i < fails && ms < ESL_PUSH_RETRY_MAX_MS; i++)
        ms *= 2;
    return ms < ESL_PUSH_RETRY_MAX_MS ? ms : ESL_PUSH_RETRY_MAX_MS;
}

const esl_push_tag_t *esl_push_unsaved(const esl_push_t *p)
{
    for (unsigned i = 0; i < ESL_PUSH_TAGS; i++)
        if (p->tags[i].mac && p->tags[i].dirty)
            return &p->tags[i];
    return NULL;
}

void esl_push_saved(esl_push_t *p, uint64_t mac)
{
    esl_push_tag_t *t = find(p, mac);
    if (t)
        t->dirty = false;
}

unsigned esl_push_round(esl_push_t *p, esl_store_t *st, esl_layouts_t *ls, esl_image_t *im, uint64_t now_ms,
                        unsigned max_sends)
{
    unsigned sends = 0;
    unsigned start = p->cursor % ESL_PUSH_TAGS;
    for (unsigned k = 0; k < ESL_PUSH_TAGS && sends < max_sends; k++) {
        unsigned i = (start + k) % ESL_PUSH_TAGS;
        esl_push_tag_t *t = &p->tags[i];
        if (!t->mac || t->next_ms > now_ms)
            continue;
        /* A tag the store does not know as this model yet (just paired, or deleted from
           the store and paired again): recorded first, so its content is checked and
           drawn for that model */
        esl_tag_t st_tag;
        if (!esl_store_get(st, t->mac, &st_tag) || st_tag.model != t->model)
            note(st, ls, t, 0, now_ms);
        if (!needs(st, ls, t))
            continue;
        uint32_t id = 0;
        uint8_t model = 0, fmt = 0;
        size_t len = esl_image_draw(im, st, ls, t->mac, p->buf, p->cap, &id, &model, &fmt);
        if (!len)
            continue; /* nothing it can be drawn for (yet: a layout with dates waits for the time) */
        p->stats.drawn++;
        if (id == t->shown || id == t->refused || model != t->model || fmt != ESL_IMG_FMT_RAW)
            continue; /* drawn again to the same image; or not one the transport takes */
        esl_push_info_t info;
        memset(&info, 0, sizeof(info));
        sends++;
        p->cursor = i + 1;
        esl_push_res_t res = p->out.send(p->out.ctx, t->mac, t->model, p->buf, len, &info);
        if (info.battery_mv)
            t->battery_mv = info.battery_mv;
        if (res == ESL_PUSH_AWAY) {
            p->stats.away++;
            if (t->fails < 255)
                t->fails++;
            t->next_ms = now_ms + backoff_ms(t->fails);
            continue;
        }
        t->fails = 0;
        t->woken = false;
        t->next_ms = 0;
        if (res == ESL_PUSH_SENT) {
            p->stats.sent++;
            t->shown = id;
            t->dirty = true;
            t->refused = 0;
        } else {
            p->stats.refused++;
            t->refused = id;
        }
        note(st, ls, t, info.rssi, now_ms);
    }
    return sends;
}
