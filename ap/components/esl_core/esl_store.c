#include "esl_store.h"
#include <stdlib.h>
#include <string.h>

static void lock(esl_store_t *st)
{
    if (st->ops.lock)
        st->ops.lock(st->ops.ctx);
}

static void unlock(esl_store_t *st)
{
    if (st->ops.unlock)
        st->ops.unlock(st->ops.ctx);
}

static void save_lock(esl_store_t *st)
{
    if (st->ops.save_lock)
        st->ops.save_lock(st->ops.ctx);
}

static void save_unlock(esl_store_t *st)
{
    if (st->ops.save_unlock)
        st->ops.save_unlock(st->ops.ctx);
}

/* Slot i (below st->cap) */
static esl_tag_t *at(const esl_store_t *st, unsigned i)
{
    return &st->chunk[i / ESL_STORE_CHUNK][i % ESL_STORE_CHUNK];
}

static void *alloc(esl_store_t *st, size_t n)
{
    return st->ops.alloc ? st->ops.alloc(st->ops.ctx, n) : calloc(1, n);
}

/* Memory for ESL_STORE_CHUNK more slots. False at max or without memory. Under the lock. */
static bool grow(esl_store_t *st)
{
    unsigned k = st->cap / ESL_STORE_CHUNK;
    if (st->cap >= st->max)
        return false;
    esl_tag_t *tags = alloc(st, ESL_STORE_CHUNK * sizeof(esl_tag_t));
    esl_content_t *content = NULL;
    if (tags && !st->ops.load_content) {
        content = alloc(st, ESL_STORE_CHUNK * sizeof(esl_content_t));
        if (!content) {
            free(tags);
            tags = NULL;
        }
    }
    if (!tags)
        return false;
    st->chunk[k] = tags;
    st->content[k] = content;
    st->cap = st->max - st->cap < ESL_STORE_CHUNK ? st->max : st->cap + ESL_STORE_CHUNK;
    return true;
}

static int find(const esl_store_t *st, uint64_t mac)
{
    for (unsigned i = 0; i < st->cap; i++) {
        const esl_tag_t *t = at(st, i);
        if (t->used && t->mac == mac)
            return (int)i;
    }
    return -1;
}

/* A free slot, growing the table when it has none. */
static int find_free(esl_store_t *st)
{
    for (unsigned i = 0; i < st->cap; i++)
        if (!at(st, i)->used)
            return (int)i;
    unsigned i = st->cap;
    return grow(st) ? (int)i : -1;
}

/* Tag without content or settings that was seen longest ago. */
static int find_evictable(const esl_store_t *st)
{
    int best = -1;
    for (unsigned i = 0; i < st->cap; i++) {
        const esl_tag_t *t = at(st, i);
        if (!t->used || esl_tag_holds_data(t))
            continue;
        if (best < 0 || t->last_seen_ms < at(st, (unsigned)best)->last_seen_ms)
            best = (int)i;
    }
    return best;
}

static esl_tag_t *take_slot(esl_store_t *st, unsigned i, uint64_t mac)
{
    esl_tag_t *t = at(st, i);
    st->full = false;
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->mac = mac;
    return t;
}

/* The channel in CONFIG: the one moved to, else the one the radio is on; 0 for the default */
static uint16_t announced_channel(const esl_store_t *st)
{
    uint8_t ch = st->channel_next ? st->channel_next : st->channel;
    return ch == ESL_DEFAULT_CHANNEL ? 0 : ch;
}

static void tag_defaults(const esl_store_t *st, esl_tag_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->poll_s = st->next_poll_s;
    out->channel = announced_channel(st);
}

/* esl_store_tag_ap() under the lock */
static esl_tag_cfg_t tag_ap(const esl_store_t *st, const esl_tag_t *t, const esl_tag_cfg_t *ap)
{
    esl_tag_cfg_t a = *ap;
    if (t->radio && st->radio_channel[t->radio])
        a.channel = st->radio_channel[t->radio];
    return a;
}

static uint16_t tag_poll_s(const esl_store_t *st, const esl_tag_t *t)
{
    uint16_t v = t->cfg.poll_s ? t->cfg.poll_s : st->next_poll_s;
    /* A tag keeps the last next_poll_s it got when a reply has 0, and an image tag may
       last have got ESL_IMAGE_BUSY_S, a tag fetching blocks ESL_BLOCKS_POLL_S: it is
       told its interval outright */
    if (!v && (t->image_capable || (t->poll_flags & ESL_POLL_F_OTA)))
        v = ESL_TAG_BUILTIN_POLL_S;
    return v;
}

/* Slot for an entry loaded at boot: free, or already holding the same MAC. */
static esl_tag_t *restore_slot(esl_store_t *st, unsigned slot, uint64_t mac)
{
    while (slot >= st->cap)
        if (!grow(st))
            return NULL;
    esl_tag_t *t = at(st, slot);
    if (t->used)
        return t->mac == mac ? t : NULL;
    if (find(st, mac) >= 0)
        return NULL;
    return take_slot(st, slot, mac);
}

static void set_content(esl_store_t *st, unsigned slot, const esl_content_t *c)
{
    esl_tag_t *t = at(st, slot);
    if (st->content[slot / ESL_STORE_CHUNK])
        st->content[slot / ESL_STORE_CHUNK][slot % ESL_STORE_CHUNK] = *c;
    t->content_id = esl_content_id(c);
    t->layout_hash = esl_content_layout_hash(c);
    t->has_content = true;
    t->has_image = c->layout == ESL_LAYOUT_IMAGE;
    t->content_err = ESL_CERR_NONE;
    t->content_unfit = false;
    t->content_check = true;  /* against the file at its next POLL (esl_store_check_tag): it may come
                                 from the schedule, or be for a model not known yet */
    t->image_id = 0;          /* drawn again when a tag needs it */
    t->image_failed = 0;      /* a refusal was of the old content's image */
}

const char *esl_content_err_text(uint8_t err)
{
    switch (err) {
    case ESL_CERR_NO_IMAGE:    return "the tag cannot show images drawn by the AP (old firmware, or no SPI flash)";
    case ESL_CERR_NOT_IN_FILE: return "the layout is not in the layout file for this tag's model";
    case ESL_CERR_UNFIT:       return "a barcode, QR code or number does not fit its box in the layout file for this tag's model";
    default:                   return NULL;
    }
}

static bool env_has(const esl_layout_env_t *env, uint32_t layout_hash)
{
    for (int i = 0; env && i < env->count; i++)
        if (esl_layout_name_hash(env->names[i]) == layout_hash)
            return true;
    return false;
}

/*
 * Under the lock: the answer to a POLL for a known tag.
 *
 * Priority when several things are waiting (docs/protokoll.md part 1, "Priority"):
 *   1. CONFIG - settings
 *   2. IDLE   - the caller then offers an image (esl_image) or firmware (esl_tagfw)
 * The AP draws the content itself, so here it only notes why content cannot be
 * shown (content_err). One message per POLL.
 * next_poll_s in every reply is the tag's own interval (eff.poll_s), or shorter
 * when scheduled content is due before then (next_poll).
 */
static uint16_t next_poll(const esl_tag_t *t, uint16_t poll_s, uint64_t now_ms)
{
    uint32_t now_s = (uint32_t)(now_ms / 1000);
    uint32_t interval = poll_s ? poll_s : ESL_TAG_BUILTIN_POLL_S;
    if (!t->due_s)
        return poll_s;
    uint32_t wait = (t->due_s > now_s ? t->due_s - now_s : 0) + ESL_DUE_MARGIN_S;
    return wait < interval ? (uint16_t)wait : poll_s;
}

static size_t poll_answer(esl_store_t *st, esl_tag_t *t, const esl_poll_t *poll, const esl_layout_env_t *env,
                          uint64_t now_ms, uint8_t *reply)
{
    esl_tag_cfg_t ap, eff;

    tag_defaults(st, &ap);
    ap = tag_ap(st, t, &ap);
    esl_tag_effective_cfg(t, &ap, &eff);
    /* tag_poll_s, not eff.poll_s: 0 would leave an image tag at ESL_IMAGE_BUSY_S */
    uint16_t next = next_poll(t, tag_poll_s(st, t), now_ms);
    t->content_err = ESL_CERR_NONE;
    if (t->has_content && !(poll->flags & ESL_POLL_F_IMAGE))
        t->content_err = ESL_CERR_NO_IMAGE;
    else if (t->has_content && t->layout_hash && !env_has(env, t->layout_hash))
        t->content_err = ESL_CERR_NOT_IN_FILE;
    else if (t->has_content && t->content_unfit && env && env->version)
        t->content_err = ESL_CERR_UNFIT;
    /* Only tags that send cfg_ver get CONFIG; older firmware would drop it. */
    if (esl_tag_cfg_pending(t, &ap))
        return esl_config_msg_build(reply, next, &eff);
    return esl_idle_build(reply, next);
}

bool esl_tag_pending(const esl_tag_t *t)
{
    if (!t->has_content || !t->seen)
        return t->has_content;
    return !t->image_id || t->shown_content_id != t->image_id;
}

uint8_t esl_tag_content_err(const esl_tag_t *t)
{
    return t->has_content ? t->content_err : ESL_CERR_NONE;
}

bool esl_tag_holds_data(const esl_tag_t *t)
{
    return t->has_content || !esl_tag_cfg_is_default(&t->cfg);
}

void esl_tag_effective_cfg(const esl_tag_t *t, const esl_tag_cfg_t *ap, esl_tag_cfg_t *out)
{
    *out = t->cfg;
    if (!out->poll_s)
        out->poll_s = ap->poll_s;
    out->channel = ap->channel;
}

bool esl_tag_cfg_pending(const esl_tag_t *t, const esl_tag_cfg_t *ap)
{
    esl_tag_cfg_t eff;
    esl_tag_effective_cfg(t, ap, &eff);
    return t->seen && t->cfg_capable && t->shown_cfg_ver != esl_tag_cfg_version(&eff);
}

void esl_store_init(esl_store_t *st, unsigned max, const esl_store_ops_t *ops, uint16_t next_poll_s)
{
    memset(st, 0, sizeof(*st));
    st->max = max < ESL_STORE_MAX_TAGS ? max : ESL_STORE_MAX_TAGS;
    st->next_poll_s = next_poll_s;
    st->next_poll_builtin = next_poll_s;
    st->channel = st->channel_builtin = ESL_DEFAULT_CHANNEL;
    if (ops)
        st->ops = *ops;
}

void esl_store_free(esl_store_t *st)
{
    for (unsigned k = 0; k < ESL_STORE_MAX_CHUNKS; k++) {
        free(st->chunk[k]);
        free(st->content[k]);
        st->chunk[k] = NULL;
        st->content[k] = NULL;
    }
    st->cap = 0;
}

bool esl_store_full(esl_store_t *st)
{
    lock(st);
    bool full = st->full;
    unlock(st);
    return full;
}

unsigned esl_store_slots(esl_store_t *st)
{
    lock(st);
    unsigned n = st->cap;
    unlock(st);
    return n;
}

bool esl_store_restore(esl_store_t *st, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    lock(st);
    esl_tag_t *t = restore_slot(st, slot, mac);
    if (t)
        set_content(st, slot, c);
    unlock(st);
    return t != NULL;
}

bool esl_store_restore_seen(esl_store_t *st, unsigned slot, uint64_t mac, const esl_seen_t *s)
{
    lock(st);
    esl_tag_t *t = restore_slot(st, slot, mac);
    if (t) {
        t->model = s->model;
        t->fw = s->fw;
        t->battery_mv = s->battery_mv;
        t->rssi = s->rssi;
        t->seen_unix = s->unix_s;
    }
    unlock(st);
    return t != NULL;
}

static void seen_record(const esl_tag_t *t, esl_seen_t *s)
{
    s->model = t->model;
    s->fw = t->fw;
    s->battery_mv = t->battery_mv;
    s->rssi = t->rssi;
    s->unix_s = t->seen_unix;
}

unsigned esl_store_save_seen(esl_store_t *st)
{
    if (!st->ops.persist_seen)
        return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < esl_store_slots(st); i++) {
        esl_seen_t s;
        uint64_t mac;
        bool dirty;
        lock(st);
        esl_tag_t *t = at(st, i);
        dirty = t->used && t->seen_dirty;
        if (dirty) {
            mac = t->mac;
            seen_record(t, &s);
            t->seen_dirty = false;
        }
        unlock(st);
        if (dirty) {
            st->ops.persist_seen(st->ops.ctx, i, mac, &s);
            n++;
        }
    }
    return n;
}

bool esl_store_restore_cfg(esl_store_t *st, unsigned slot, uint64_t mac, const esl_tag_cfg_t *cfg)
{
    lock(st);
    esl_tag_t *t = restore_slot(st, slot, mac);
    if (t)
        t->cfg = *cfg;
    unlock(st);
    return t != NULL;
}

/* The tag's slot, or a new one (free, else evicted). -1 if the table is full.
 * Call with the lock held. */
static int claim(esl_store_t *st, uint64_t mac)
{
    int idx = find(st, mac);
    if (idx < 0) {
        idx = find_free(st);
        if (idx < 0)
            idx = find_evictable(st);
        if (idx < 0) {
            st->full = true;
            return -1;
        }
        take_slot(st, (unsigned)idx, mac);
    }
    return idx;
}

static esl_store_err_t set_saved(esl_store_t *st, uint64_t mac, const esl_content_t *c, esl_set_result_t *res);

esl_store_err_t esl_store_set(esl_store_t *st, uint64_t mac, const esl_content_t *c, esl_set_result_t *res)
{
    save_lock(st);
    esl_store_err_t err = set_saved(st, mac, c, res);
    save_unlock(st);
    return err;
}

/* Under save_lock. Changes are told only by content_id (CRC32): content with the same
   id as the tag's counts as unchanged. */
static esl_store_err_t set_saved(esl_store_t *st, uint64_t mac, const esl_content_t *c, esl_set_result_t *res)
{
    lock(st);
    int idx = claim(st, mac);
    if (idx < 0) {
        unlock(st);
        return ESL_STORE_FULL;
    }
    esl_tag_t *t = at(st, (unsigned)idx);
    esl_tag_t old = *t;
    esl_content_t *ram = st->content[(unsigned)idx / ESL_STORE_CHUNK];
    esl_content_t old_content;
    if (ram)
        old_content = ram[(unsigned)idx % ESL_STORE_CHUNK];
    uint32_t id = esl_content_id(c);
    res->changed = !t->has_content || t->content_id != id;
    if (res->changed)
        set_content(st, (unsigned)idx, c);
    res->content_id = t->content_id;
    res->pending = esl_tag_pending(t);
    unlock(st);

    if (!res->changed || !st->ops.persist_save || st->ops.persist_save(st->ops.ctx, (unsigned)idx, mac, c)) {
        /* a picture it had is not needed any more */
        if (res->changed && old.has_image && c->layout != ESL_LAYOUT_IMAGE && st->ops.persist_image)
            st->ops.persist_image(st->ops.ctx, (unsigned)idx, NULL, 0);
        return ESL_STORE_OK;
    }
    /* Not saved: back to what it had (NVS still holds that), unless it changed meanwhile */
    lock(st);
    t = at(st, (unsigned)idx);
    if (t->used && t->mac == mac && t->content_id == id) {
        t->has_content = old.has_content;
        t->content_id = old.content_id;
        t->layout_hash = old.layout_hash;
        t->has_image = old.has_image;
        t->image_id = old.image_id;
        t->image_failed = old.image_failed;
        t->content_err = old.content_err;
        t->content_unfit = old.content_unfit;
        t->content_check = old.content_check;
        if (ram)
            ram[(unsigned)idx % ESL_STORE_CHUNK] = old_content;
    }
    unlock(st);
    return ESL_STORE_NOT_SAVED;
}

bool esl_store_takes_images(const esl_store_t *st)
{
    return st->ops.persist_image && st->ops.load_image;
}

esl_store_err_t esl_store_set_image(esl_store_t *st, uint64_t mac, const uint8_t *data, size_t len,
                                    const esl_content_t *c, esl_set_result_t *res)
{
    if (!esl_store_takes_images(st))
        return ESL_STORE_NOT_SAVED;
    save_lock(st);
    lock(st);
    int idx = claim(st, mac);
    bool same = idx >= 0 && at(st, (unsigned)idx)->has_content && at(st, (unsigned)idx)->content_id == esl_content_id(c);
    unlock(st);
    esl_store_err_t err = ESL_STORE_FULL;
    if (idx >= 0) {
        /* The slot stays the tag's: deleting it takes save_lock too */
        if (same || st->ops.persist_image(st->ops.ctx, (unsigned)idx, data, len))
            err = set_saved(st, mac, c, res);
        else
            err = ESL_STORE_NOT_SAVED;
    }
    save_unlock(st);
    return err;
}

size_t esl_store_get_image(esl_store_t *st, uint64_t mac, uint8_t *buf, size_t cap)
{
    lock(st);
    int idx = find(st, mac);
    bool has = idx >= 0 && at(st, (unsigned)idx)->has_image;
    unlock(st);
    return has && st->ops.load_image ? st->ops.load_image(st->ops.ctx, (unsigned)idx, buf, cap) : 0;
}

esl_store_err_t esl_store_set_cfg(esl_store_t *st, uint64_t mac, const esl_tag_cfg_t *cfg, esl_cfg_result_t *res)
{
    save_lock(st);
    lock(st);
    int idx = claim(st, mac);
    if (idx < 0) {
        unlock(st);
        save_unlock(st);
        return ESL_STORE_FULL;
    }
    esl_tag_t *t = at(st, (unsigned)idx);
    res->changed = memcmp(&t->cfg, cfg, sizeof(*cfg)) != 0;
    if (res->changed)
        t->cfg = *cfg;
    esl_tag_cfg_t ap;
    tag_defaults(st, &ap);
    ap = tag_ap(st, t, &ap);
    res->pending = esl_tag_cfg_pending(t, &ap);
    bool is_default = esl_tag_cfg_is_default(cfg);
    unlock(st);

    if (res->changed && st->ops.persist_cfg)
        st->ops.persist_cfg(st->ops.ctx, (unsigned)idx, mac, is_default ? NULL : cfg);
    save_unlock(st);
    return ESL_STORE_OK;
}

void esl_store_clear_due(esl_store_t *st)
{
    lock(st);
    for (unsigned i = 0; i < st->cap; i++)
        at(st, i)->due_s = 0;
    unlock(st);
}

void esl_store_note_due(esl_store_t *st, uint64_t mac, uint32_t due_s)
{
    if (!due_s)
        due_s = 1; /* 0 means none */
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0 && (!at(st, (unsigned)idx)->due_s || due_s < at(st, (unsigned)idx)->due_s))
        at(st, (unsigned)idx)->due_s = due_s;
    unlock(st);
}

static void add_u16(uint16_t *v, unsigned n)
{
    *v = *v + n > 0xFFFFu ? 0xFFFFu : (uint16_t)(*v + n);
}

static void note_signal(esl_tag_t *t, const esl_poll_t *poll, int8_t rssi)
{
    uint8_t info = 0;
    if (poll->has_link) {
        /* The first POLL since the AP started: rounds it lost may be the ones while the AP
         * itself was down (a restart, an OTA update), so they are not counted against the link */
        if (t->hist_n) {
            info = (uint8_t)(poll->lost > ESL_HIST_LOST ? ESL_HIST_LOST : poll->lost);
            add_u16(&t->lost_total, poll->lost);
        }
        /* the retry flag is about the round before this one: it marks that POLL's entry */
        if ((poll->link_flags & ESL_POLL_LF_RETRY) && t->hist_n) {
            t->hist_info[(t->hist_at + ESL_LINK_HIST - 1) % ESL_LINK_HIST] |= ESL_HIST_RETRY;
            add_u16(&t->retries, 1);
        }
        t->rx_rssi = poll->rx_rssi;
        t->rx_lqi = poll->rx_lqi;
    }
    t->hist_rssi[t->hist_at] = rssi;
    t->hist_info[t->hist_at] = info;
    t->hist_at = (uint8_t)((t->hist_at + 1) % ESL_LINK_HIST);
    if (t->hist_n < ESL_LINK_HIST)
        t->hist_n++;
}

size_t esl_store_on_poll(esl_store_t *st, uint64_t mac, const esl_poll_t *poll, int8_t rssi,
                         uint64_t now_ms, const esl_layout_env_t *env, uint8_t *reply)
{
    size_t n;
    uint32_t unix_s = st->ops.unix_now ? st->ops.unix_now(st->ops.ctx) : 0;
    bool save = false; /* new tag, or another model or firmware: its record now */
    esl_seen_t rec;
    lock(st);
    int idx = find(st, mac);
    if (idx < 0) {
        idx = find_free(st);
        if (idx >= 0) {
            take_slot(st, (unsigned)idx, mac);
            save = true;
        } else {
            st->full = true;
        }
    }
    if (idx < 0) {
        n = esl_idle_build(reply, st->next_poll_s);
    } else {
        esl_tag_t *t = at(st, (unsigned)idx);
        if (t->model != poll->model || t->fw != poll->fw_ver)
            save = true;
        if (t->model != poll->model)
            t->content_check = true; /* checked against the file of the model it turns out to be */
        if (unix_s)
            t->seen_unix = unix_s;
        t->seen = true;
        t->last_seen_ms = now_ms;
        t->shown_content_id = poll->content_id;
        t->battery_mv = poll->battery_mv;
        t->fw = poll->fw_ver;
        t->model = poll->model;
        t->rssi = rssi;
        t->poll_flags = poll->flags;
        t->cfg_capable = poll->has_cfg_ver;
        t->channel_capable = poll->has_link;
        t->shown_cfg_ver = poll->cfg_ver;
        t->polls++;
        note_signal(t, poll, rssi);
        t->image_capable = (poll->flags & ESL_POLL_F_IMAGE) != 0;
        t->image_rle = t->image_capable && (poll->flags & ESL_POLL_F_IMG_RLE);
        t->image_2d = t->image_capable && (poll->flags & ESL_POLL_F_IMG_2D);
        n = poll_answer(st, t, poll, env, now_ms, reply);
        t->seen_dirty = !save;
        if (save)
            seen_record(t, &rec);
    }
    unlock(st);
    if (save && st->ops.persist_seen)
        st->ops.persist_seen(st->ops.ctx, (unsigned)idx, mac, &rec);
    return n;
}

bool esl_store_fw_offer(esl_store_t *st, uint64_t mac, const esl_poll_t *poll, uint16_t ver)
{
    bool offer = false;
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0) {
        esl_tag_t *t = at(st, (unsigned)idx);
        /* Only once it started on it: see esl_store_fw_taken */
        if ((poll->flags & ESL_POLL_F_FWFAIL) && t->fw_offered && t->fw_taken)
            t->fw_failed = t->fw_offered;
        if (ver && poll->fw_ver == ver)
            t->fw_failed = 0;
        offer = ver && (poll->flags & ESL_POLL_F_OTA) && poll->fw_ver != ver && t->fw_failed != ver;
        if (offer) {
            t->fw_offered = ver;
            t->fw_taken = false;
        }
    }
    unlock(st);
    return offer;
}

bool esl_store_image_offer(esl_store_t *st, uint64_t mac, const esl_poll_t *poll, uint32_t id)
{
    bool offer = false;
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0) {
        esl_tag_t *t = at(st, (unsigned)idx);
        /* Only once it started on it: see esl_store_fw_taken */
        if ((poll->flags & ESL_POLL_FLAG_DRAW_FAILED) && t->image_offered && t->image_taken)
            t->image_failed = t->image_offered;
        if (id && poll->content_id == id)
            t->image_failed = 0;
        offer = id && (poll->flags & ESL_POLL_F_IMAGE) && poll->content_id != id && t->image_failed != id;
        if (offer) {
            t->image_offered = id;
            t->image_taken = false;
        }
    }
    unlock(st);
    return offer;
}

void esl_store_image_taken(esl_store_t *st, uint64_t mac, uint32_t id)
{
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0 && id && at(st, (unsigned)idx)->image_offered == id)
        at(st, (unsigned)idx)->image_taken = true;
    unlock(st);
}

void esl_store_image_known(esl_store_t *st, uint64_t mac, uint32_t content_id, uint32_t layout_ver, uint32_t id)
{
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0 && at(st, (unsigned)idx)->has_content && at(st, (unsigned)idx)->content_id == content_id) {
        at(st, (unsigned)idx)->image_id = id;
        at(st, (unsigned)idx)->image_layout_ver = layout_ver;
    }
    unlock(st);
}

void esl_store_fw_sent(esl_store_t *st, uint64_t mac, uint16_t ver, uint16_t end, bool offer)
{
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0 && !(offer && at(st, (unsigned)idx)->fw_got_ver == ver)) {
        at(st, (unsigned)idx)->fw_got_ver = ver;
        at(st, (unsigned)idx)->fw_got = end;
    }
    unlock(st);
}

void esl_store_fw_taken(esl_store_t *st, uint64_t mac, uint16_t ver)
{
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0 && ver && at(st, (unsigned)idx)->fw_offered == ver)
        at(st, (unsigned)idx)->fw_taken = true;
    unlock(st);
}

void esl_store_fw_counts(esl_store_t *st, uint8_t model, uint16_t ver, esl_fw_counts_t *out)
{
    memset(out, 0, sizeof(*out));
    lock(st);
    for (unsigned i = 0; i < st->cap; i++) {
        const esl_tag_t *t = at(st, i);
        if (!t->used || !t->seen || t->model != model)
            continue;
        if (t->fw == ver)
            out->updated++;
        else if (!(t->poll_flags & ESL_POLL_F_OTA))
            out->no_ota++;
        else if (t->fw_failed == ver)
            out->failed++;
        else
            out->pending++;
    }
    unlock(st);
}

bool esl_store_delete(esl_store_t *st, uint64_t mac)
{
    save_lock(st);
    lock(st);
    int idx = find(st, mac);
    bool had_content = false, had_cfg = false, had_image = false;
    if (idx >= 0) {
        had_content = at(st, (unsigned)idx)->has_content;
        had_image = at(st, (unsigned)idx)->has_image;
        had_cfg = !esl_tag_cfg_is_default(&at(st, (unsigned)idx)->cfg);
        memset(at(st, (unsigned)idx), 0, sizeof(esl_tag_t));
        st->full = false;
    }
    unlock(st);
    if (idx >= 0 && had_content && st->ops.persist_erase)
        st->ops.persist_erase(st->ops.ctx, (unsigned)idx);
    if (idx >= 0 && had_image && st->ops.persist_image)
        st->ops.persist_image(st->ops.ctx, (unsigned)idx, NULL, 0);
    if (idx >= 0 && had_cfg && st->ops.persist_cfg)
        st->ops.persist_cfg(st->ops.ctx, (unsigned)idx, mac, NULL);
    if (idx >= 0 && st->ops.persist_seen)
        st->ops.persist_seen(st->ops.ctx, (unsigned)idx, mac, NULL);
    save_unlock(st);
    return idx >= 0;
}

int esl_store_slot_of(esl_store_t *st, uint64_t mac)
{
    lock(st);
    int idx = find(st, mac);
    unlock(st);
    return idx;
}

bool esl_store_get(esl_store_t *st, uint64_t mac, esl_tag_t *out)
{
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0)
        *out = *at(st, (unsigned)idx);
    unlock(st);
    return idx >= 0;
}

bool esl_store_get_content(esl_store_t *st, uint64_t mac, esl_content_t *out)
{
    lock(st);
    int idx = find(st, mac);
    if (idx < 0 || !at(st, (unsigned)idx)->has_content) {
        unlock(st);
        return false;
    }
    uint32_t id = at(st, (unsigned)idx)->content_id;
    esl_content_t *ram = st->content[(unsigned)idx / ESL_STORE_CHUNK];
    if (ram)
        *out = ram[(unsigned)idx % ESL_STORE_CHUNK];
    unlock(st);
    if (ram)
        return true;
    return st->ops.load_content && st->ops.load_content(st->ops.ctx, (unsigned)idx, mac, out) &&
           esl_content_id(out) == id;
}

/* Notes the outcome of checking content content_id of a tag of model against the file:
   unfit or not, and checked. Nothing when the content or the model changed meanwhile. */
static void mark_checked(esl_store_t *st, uint64_t mac, uint32_t content_id, uint8_t model, bool unfit)
{
    lock(st);
    int idx = find(st, mac);
    esl_tag_t *t = idx >= 0 ? at(st, (unsigned)idx) : NULL;
    if (t && t->has_content && t->content_id == content_id && t->model == model) {
        t->content_unfit = unfit;
        t->content_check = false;
        /* also before its next POLL: in the API, and held back; another reason it has stays */
        if (unfit && t->content_err == ESL_CERR_NONE)
            t->content_err = ESL_CERR_UNFIT;
        else if (!unfit && t->content_err == ESL_CERR_UNFIT)
            t->content_err = ESL_CERR_NONE;
    }
    unlock(st);
}

/* Checks the content of tag t (a copy) against its model's file and notes it. True if it
   fits (or names no layout in the file); else false with the reason in msg. Only content
   naming a layout is read (from NVS on the device). */
static bool check_one(esl_store_t *st, esl_layouts_t *ls, const esl_tag_t *t, char *msg, size_t msglen)
{
    esl_content_t c;
    bool unfit = false;
    if (t->layout_hash && esl_store_get_content(st, t->mac, &c) && esl_content_id(&c) == t->content_id)
        unfit = !esl_layouts_check_content(ls, t->model, &c, msg, msglen);
    mark_checked(st, t->mac, t->content_id, t->model, unfit);
    return !unfit;
}

unsigned esl_store_check_layout(esl_store_t *st, esl_layouts_t *ls, uint8_t model,
                                void (*fn)(void *ctx, uint64_t mac, const char *msg), void *ctx)
{
    unsigned bad = 0, slots = esl_store_slots(st);
    for (unsigned slot = 0; slot < slots; slot++) {
        esl_tag_t t;
        char msg[ESL_LAYOUT_ERR_MAX];
        /* the tags drawn with model's file: its own, and those of models whose same_as names it */
        if (!esl_store_get_slot(st, slot, &t) || !t.has_content || esl_model_layout_model(t.model) != model)
            continue;
        if (!check_one(st, ls, &t, msg, sizeof(msg))) {
            bad++;
            if (fn)
                fn(ctx, t.mac, msg);
        }
    }
    return bad;
}

bool esl_store_check_tag(esl_store_t *st, esl_layouts_t *ls, uint64_t mac)
{
    esl_tag_t t;
    char msg[80]; /* not shown: cut short is fine */
    if (!esl_store_get(st, mac, &t) || !t.has_content || !t.content_check || !esl_model_name(t.model))
        return true;
    return check_one(st, ls, &t, msg, sizeof(msg));
}

bool esl_store_get_slot(esl_store_t *st, unsigned slot, esl_tag_t *out)
{
    bool ok = false;
    lock(st);
    if (slot < st->cap && at(st, slot)->used) {
        *out = *at(st, slot);
        ok = true;
    }
    unlock(st);
    return ok;
}

unsigned esl_store_count(esl_store_t *st)
{
    unsigned n = 0;
    lock(st);
    for (unsigned i = 0; i < st->cap; i++)
        n += at(st, i)->used;
    unlock(st);
    return n;
}

unsigned esl_store_available(esl_store_t *st)
{
    unsigned n = 0;
    lock(st);
    for (unsigned i = 0; i < st->cap; i++)
        n += !at(st, i)->used || !esl_tag_holds_data(at(st, i));
    n += st->max - st->cap; /* if there is memory for them */
    unlock(st);
    return n;
}

static void apply_next_poll(esl_store_t *st, uint16_t next_poll_s)
{
    lock(st);
    st->next_poll_s = next_poll_s;
    unlock(st);
}

void esl_store_set_next_poll(esl_store_t *st, uint16_t next_poll_s)
{
    apply_next_poll(st, next_poll_s);
    if (st->ops.persist_next_poll)
        st->ops.persist_next_poll(st->ops.ctx, &next_poll_s);
}

void esl_store_reset_next_poll(esl_store_t *st)
{
    apply_next_poll(st, st->next_poll_builtin);
    if (st->ops.persist_next_poll)
        st->ops.persist_next_poll(st->ops.ctx, NULL);
}

void esl_store_restore_next_poll(esl_store_t *st, uint16_t next_poll_s)
{
    apply_next_poll(st, next_poll_s);
}

uint16_t esl_store_get_next_poll(esl_store_t *st)
{
    lock(st);
    uint16_t v = st->next_poll_s;
    unlock(st);
    return v;
}

void esl_store_tag_defaults(esl_store_t *st, esl_tag_cfg_t *out)
{
    lock(st);
    tag_defaults(st, out);
    unlock(st);
}

void esl_store_set_builtin_channel(esl_store_t *st, uint8_t channel)
{
    lock(st);
    st->channel = st->channel_builtin = channel;
    st->channel_next = 0;
    unlock(st);
}

static uint64_t tag_poll_ms(const esl_store_t *st, const esl_tag_t *t)
{
    uint16_t s = tag_poll_s(st, t);
    return (uint64_t)(s ? s : ESL_TAG_BUILTIN_POLL_S) * 1000;
}

/* Plans a move to channel (under the lock): long enough for every tag to get CONFIG and
   confirm it at the POLL after (every tag heard since boot, or every tag with all); the
   channel the radio is on cancels one */
static void plan_move(esl_store_t *st, uint8_t channel, bool now, bool all, uint64_t now_ms)
{
    st->channel_hold = false;
    st->channel_stopped = 0;
    if (channel == st->channel) {
        st->channel_next = 0;
        return;
    }
    uint64_t longest = ESL_TAG_BUILTIN_POLL_S * 1000ull;
    for (unsigned i = 0; i < st->cap; i++) {
        const esl_tag_t *t = at(st, i);
        if (t->used && (t->seen || all) && tag_poll_ms(st, t) > longest)
            longest = tag_poll_ms(st, t);
    }
    uint64_t wait = 2 * longest + 60000;
    if (wait > ESL_CHANNEL_MAX_WAIT_S * 1000ull)
        wait = ESL_CHANNEL_MAX_WAIT_S * 1000ull;
    st->channel_next = channel;
    st->channel_due_ms = now ? now_ms : now_ms + wait;
}

/* What persist_channel keeps: the radio's channel (0 = the built-in one) and the move,
   with ESL_CHANNEL_FORCED when it was forced */
static void persist_channel(esl_store_t *st, uint8_t channel, uint8_t next, bool force)
{
    if (st->ops.persist_channel)
        st->ops.persist_channel(st->ops.ctx, channel == st->channel_builtin ? 0 : channel,
                                next && force ? next | ESL_CHANNEL_FORCED : next);
}

void esl_store_restore_channel(esl_store_t *st, uint8_t channel, uint8_t next, uint64_t now_ms)
{
    lock(st);
    if (channel)
        st->channel = channel;
    st->channel_next = 0;
    if (next & ~ESL_CHANNEL_FORCED) {
        plan_move(st, next & ~ESL_CHANNEL_FORCED, false, true, now_ms);  /* announced again, with a new deadline */
        st->channel_hold = st->channel_next != 0;
        st->channel_force = (next & ESL_CHANNEL_FORCED) != 0;
    }
    unlock(st);
}

void esl_store_move_channel(esl_store_t *st, uint8_t channel, bool now, bool force, uint64_t now_ms)
{
    lock(st);
    plan_move(st, channel ? channel : st->channel_builtin, now, false, now_ms);
    st->channel_force = force;
    uint8_t on = st->channel, next = st->channel_next;
    unlock(st);
    persist_channel(st, on, next, force);
}

/* The window after a POLL in which a tag counts as active: 3 of its intervals and a minute */
static uint64_t active_ms(const esl_store_t *st, const esl_tag_t *t)
{
    return ESL_CHANNEL_ACTIVE_POLLS * tag_poll_ms(st, t) + 60000;
}

static bool heard_lately(const esl_store_t *st, const esl_tag_t *t, uint64_t now_ms)
{
    return t->seen && esl_tag_seen_ms_ago(t, now_ms) <= active_ms(st, t);
}

/* Tags heard lately whose firmware cannot follow a move (under the lock) */
static unsigned old_tags(const esl_store_t *st, uint64_t now_ms)
{
    unsigned n = 0;
    for (unsigned i = 0; i < st->cap; i++) {
        const esl_tag_t *t = at(st, i);
        if (t->used && !t->channel_capable && heard_lately(st, t, now_ms))
            n++;
    }
    return n;
}

/* Tags heard lately that do not have the announced channel yet (under the lock) */
static unsigned channel_waiting(const esl_store_t *st, uint64_t now_ms)
{
    esl_tag_cfg_t ap;
    unsigned n = 0;
    tag_defaults(st, &ap);
    for (unsigned i = 0; i < st->cap; i++) {
        const esl_tag_t *t = at(st, i);
        if (!t->used)
            continue;
        esl_tag_cfg_t a = tag_ap(st, t, &ap);
        if (!t->seen ? st->channel_hold : heard_lately(st, t, now_ms) && esl_tag_cfg_pending(t, &a))
            n++;
    }
    return n;
}

uint8_t esl_store_channel_step(esl_store_t *st, uint64_t now_ms)
{
    uint8_t ch = 0;
    bool stop = false;
    lock(st);
    /* a tag with old firmware (or an old radio) that shows up during the wait stops a move off
       the built-in channel: it would confirm the move and stay behind */
    if (st->channel_next && st->channel_next != st->channel_builtin && !st->channel_force) {
        unsigned old = old_tags(st, now_ms);
        if (old || st->channel_radio_old) {
            st->channel_next = 0;
            st->channel_hold = false;
            st->channel_stopped = old ? old : 1;
            stop = true;
        }
    }
    if (st->channel_next && (now_ms >= st->channel_due_ms || !channel_waiting(st, now_ms))) {
        ch = st->channel = st->channel_next;
        st->channel_next = 0;
        st->channel_hold = false;
    }
    uint8_t on = st->channel;
    unlock(st);
    if (ch)
        persist_channel(st, ch, 0, false);  /* kept only once the radio is there (an AP that
                                               restarts before announces it again from where it was) */
    else if (stop)
        persist_channel(st, on, 0, false);
    return ch;
}

void esl_store_channel_state(esl_store_t *st, uint64_t now_ms, esl_channel_state_t *out)
{
    memset(out, 0, sizeof(*out));
    lock(st);
    out->channel = st->channel;
    out->builtin = st->channel_builtin;
    out->next = st->channel_next;
    if (st->channel_next) {
        out->waiting = channel_waiting(st, now_ms);
        out->due_s = st->channel_due_ms > now_ms ? (uint32_t)((st->channel_due_ms - now_ms + 999) / 1000) : 0;
    }
    out->old_tags = old_tags(st, now_ms);
    /* tags heard before the AP started (fw from their stored record) but not since, while they
       may still poll (now_ms counts from boot): the AP does not know their firmware now */
    for (unsigned i = 0; i < st->cap; i++) {
        const esl_tag_t *t = at(st, i);
        if (t->used && !t->seen && t->fw && now_ms <= active_ms(st, t)) {
            out->unheard++;
            uint32_t left = (uint32_t)((active_ms(st, t) - now_ms + 999) / 1000);
            if (left > out->unheard_s)
                out->unheard_s = left;
        }
    }
    out->radio_old = st->channel_radio_old;
    out->stopped = st->channel_stopped;
    for (unsigned r = 1; r < ESL_STORE_RADIOS; r++)
        if (st->radio_channel[r])
            out->taken |= 1u << st->radio_channel[r];
    unlock(st);
}

void esl_store_set_radio_follows(esl_store_t *st, bool follows)
{
    lock(st);
    st->channel_radio_old = !follows;
    unlock(st);
}

void esl_store_set_radio_channel(esl_store_t *st, uint8_t radio, uint8_t channel)
{
    if (!radio || radio >= ESL_STORE_RADIOS)
        return;
    lock(st);
    st->radio_channel[radio] = channel;
    if (!channel)
        for (unsigned i = 0; i < st->cap; i++)
            if (at(st, i)->radio == radio)
                at(st, i)->radio = 0;
    unlock(st);
}

void esl_store_set_tag_radio(esl_store_t *st, uint64_t mac, uint8_t radio)
{
    if (radio >= ESL_STORE_RADIOS)
        return;
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0)
        at(st, (unsigned)idx)->radio = radio;
    unlock(st);
}

esl_tag_cfg_t esl_store_tag_ap(esl_store_t *st, const esl_tag_t *t, const esl_tag_cfg_t *ap)
{
    lock(st);
    esl_tag_cfg_t a = tag_ap(st, t, ap);
    unlock(st);
    return a;
}

uint16_t esl_store_tag_poll_s(esl_store_t *st, uint64_t mac)
{
    lock(st);
    int idx = find(st, mac);
    uint16_t v = idx >= 0 ? tag_poll_s(st, at(st, (unsigned)idx)) : st->next_poll_s;
    unlock(st);
    return v;
}

unsigned esl_tag_signal(const esl_tag_t *t, int8_t *rssi, uint8_t *info)
{
    unsigned n = t->hist_n;
    for (unsigned i = 0; i < n; i++) {
        unsigned at = (t->hist_at + ESL_LINK_HIST - n + i) % ESL_LINK_HIST;
        rssi[i] = t->hist_rssi[at];
        info[i] = t->hist_info[at];
    }
    return n;
}
