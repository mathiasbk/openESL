#include "esl_store.h"
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

static int find(const esl_store_t *st, uint64_t mac)
{
    for (unsigned i = 0; i < st->cap; i++)
        if (st->tags[i].used && st->tags[i].mac == mac)
            return (int)i;
    return -1;
}

static int find_free(const esl_store_t *st)
{
    for (unsigned i = 0; i < st->cap; i++)
        if (!st->tags[i].used)
            return (int)i;
    return -1;
}

/* Tag without content or settings that was seen longest ago. */
static int find_evictable(const esl_store_t *st)
{
    int best = -1;
    for (unsigned i = 0; i < st->cap; i++) {
        const esl_tag_t *t = &st->tags[i];
        if (!t->used || esl_tag_holds_data(t))
            continue;
        if (best < 0 || t->last_seen_ms < st->tags[best].last_seen_ms)
            best = (int)i;
    }
    return best;
}

static uint16_t tag_poll_s(const esl_store_t *st, const esl_tag_t *t)
{
    return t->cfg.poll_s ? t->cfg.poll_s : st->next_poll_s;
}

/* Slot for an entry loaded at boot: free, or already holding the same MAC. */
static esl_tag_t *restore_slot(esl_store_t *st, unsigned slot, uint64_t mac)
{
    if (slot >= st->cap)
        return NULL;
    esl_tag_t *t = &st->tags[slot];
    if (t->used)
        return t->mac == mac ? t : NULL;
    if (find(st, mac) >= 0)
        return NULL;
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->mac = mac;
    return t;
}

static void set_content(esl_tag_t *t, const esl_content_t *c)
{
    t->content = *c;
    t->content_id = esl_content_id(c);
    t->has_content = true;
    t->content_err = ESL_CERR_NONE;
}

const char *esl_content_err_text(uint8_t err)
{
    switch (err) {
    case ESL_CERR_NO_LAYOUTS:  return "the tag firmware cannot download layout files";
    case ESL_CERR_NOT_IN_FILE: return "the layout is not in the layout file for this tag's model";
    case ESL_CERR_REJECTED:    return "the tag rejected the layout file";
    default:                   return NULL;
    }
}

static int env_find(const esl_layout_env_t *env, const char *name)
{
    for (int i = 0; env && i < env->count; i++)
        if (strcmp(env->names[i], name) == 0)
            return i;
    return -1;
}

/*
 * Under the lock: the answer to a POLL for a known tag. 0 = send the layout file first.
 *
 * Priority when several things are waiting (docs/protokoll.md part 1, "Priority"):
 *   1. LAYOUT  - the layout file, since the content may use a layout from it
 *   2. CONTENT - what the client is waiting for
 *   3. CONFIG  - settings; also when content is held back (ESL_CERR_*)
 *   4. IDLE
 * One message per POLL; the next item goes out in the next round (after a layout
 * download the tag polls again at once, so content follows in the same wake-up).
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
    uint32_t v = env ? env->version : 0;
    esl_tag_cfg_t eff;

    esl_tag_effective_cfg(t, st->next_poll_s, &eff);
    uint16_t next = next_poll(t, eff.poll_s, now_ms);
    t->content_err = ESL_CERR_NONE;
    if (poll->has_layout_ver && poll->layout_ver == v)
        t->layout_failed = 0;
    if (poll->has_layout_ver && poll->layout_ver != v) {
        if ((poll->flags & ESL_POLL_FLAG_LAYOUT_FAILED) && v && t->layout_offered == v)
            t->layout_failed = v;
        if (!v || t->layout_failed != v) {
            t->layout_offered = v;
            return 0;
        }
    }
    if (t->has_content && poll->content_id != t->content_id) {
        if (t->content.layout != ESL_LAYOUT_NAMED)
            return esl_content_msg_build(reply, next, t->content_id, &t->content);
        int idx = env_find(env, t->content.name);
        if (!poll->has_layout_ver)
            t->content_err = ESL_CERR_NO_LAYOUTS;
        else if (idx < 0)
            t->content_err = ESL_CERR_NOT_IN_FILE;
        else if (poll->layout_ver != v)
            t->content_err = ESL_CERR_REJECTED; /* see the top: only reached when it rejected v */
        if (!t->content_err) {
            esl_content_t sent;
            esl_content_resolve(&t->content, (uint8_t)idx, &sent);
            return esl_content_msg_build(reply, next, t->content_id, &sent);
        }
    }
    /* Only tags that send cfg_ver get CONFIG; older firmware would drop it. */
    if (esl_tag_cfg_pending(t, st->next_poll_s))
        return esl_config_msg_build(reply, next, &eff);
    return esl_idle_build(reply, next);
}

bool esl_tag_pending(const esl_tag_t *t)
{
    return t->has_content && (!t->seen || t->shown_content_id != t->content_id);
}

bool esl_tag_holds_data(const esl_tag_t *t)
{
    return t->has_content || !esl_tag_cfg_is_default(&t->cfg);
}

void esl_tag_effective_cfg(const esl_tag_t *t, uint16_t default_poll_s, esl_tag_cfg_t *out)
{
    *out = t->cfg;
    if (!out->poll_s)
        out->poll_s = default_poll_s;
}

bool esl_tag_cfg_pending(const esl_tag_t *t, uint16_t default_poll_s)
{
    esl_tag_cfg_t eff;
    esl_tag_effective_cfg(t, default_poll_s, &eff);
    return t->seen && t->cfg_capable && t->shown_cfg_ver != esl_tag_cfg_version(&eff);
}

void esl_store_init(esl_store_t *st, esl_tag_t *tags, unsigned cap, const esl_store_ops_t *ops, uint16_t next_poll_s)
{
    memset(st, 0, sizeof(*st));
    memset(tags, 0, sizeof(*tags) * cap);
    st->tags = tags;
    st->cap = cap;
    st->next_poll_s = next_poll_s;
    st->next_poll_builtin = next_poll_s;
    if (ops)
        st->ops = *ops;
}

bool esl_store_restore(esl_store_t *st, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    lock(st);
    esl_tag_t *t = restore_slot(st, slot, mac);
    if (t)
        set_content(t, c);
    unlock(st);
    return t != NULL;
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
        if (idx < 0)
            return -1;
        memset(&st->tags[idx], 0, sizeof(esl_tag_t));
        st->tags[idx].used = true;
        st->tags[idx].mac = mac;
    }
    return idx;
}

esl_store_err_t esl_store_set(esl_store_t *st, uint64_t mac, const esl_content_t *c, esl_set_result_t *res)
{
    bool persist = false;
    lock(st);
    int idx = claim(st, mac);
    if (idx < 0) {
        unlock(st);
        return ESL_STORE_FULL;
    }
    esl_tag_t *t = &st->tags[idx];
    res->changed = !t->has_content || !esl_content_equal(&t->content, c);
    if (res->changed) {
        set_content(t, c);
        persist = true;
    }
    res->content_id = t->content_id;
    res->pending = esl_tag_pending(t);
    esl_content_t saved = t->content;
    unlock(st);

    if (persist && st->ops.persist_save)
        st->ops.persist_save(st->ops.ctx, (unsigned)idx, mac, &saved);
    return ESL_STORE_OK;
}

esl_store_err_t esl_store_set_cfg(esl_store_t *st, uint64_t mac, const esl_tag_cfg_t *cfg, esl_cfg_result_t *res)
{
    lock(st);
    int idx = claim(st, mac);
    if (idx < 0) {
        unlock(st);
        return ESL_STORE_FULL;
    }
    esl_tag_t *t = &st->tags[idx];
    res->changed = memcmp(&t->cfg, cfg, sizeof(*cfg)) != 0;
    if (res->changed)
        t->cfg = *cfg;
    res->pending = esl_tag_cfg_pending(t, st->next_poll_s);
    bool is_default = esl_tag_cfg_is_default(cfg);
    unlock(st);

    if (res->changed && st->ops.persist_cfg)
        st->ops.persist_cfg(st->ops.ctx, (unsigned)idx, mac, is_default ? NULL : cfg);
    return ESL_STORE_OK;
}

void esl_store_clear_due(esl_store_t *st)
{
    lock(st);
    for (unsigned i = 0; i < st->cap; i++)
        st->tags[i].due_s = 0;
    unlock(st);
}

void esl_store_note_due(esl_store_t *st, uint64_t mac, uint32_t due_s)
{
    if (!due_s)
        due_s = 1; /* 0 means none */
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0 && (!st->tags[idx].due_s || due_s < st->tags[idx].due_s))
        st->tags[idx].due_s = due_s;
    unlock(st);
}

size_t esl_store_on_poll(esl_store_t *st, uint64_t mac, const esl_poll_t *poll, int8_t rssi,
                         uint64_t now_ms, const esl_layout_env_t *env, uint8_t *reply)
{
    size_t n;
    lock(st);
    int idx = find(st, mac);
    if (idx < 0) {
        idx = find_free(st);
        if (idx >= 0) {
            memset(&st->tags[idx], 0, sizeof(esl_tag_t));
            st->tags[idx].used = true;
            st->tags[idx].mac = mac;
        }
    }
    if (idx < 0) {
        n = esl_idle_build(reply, st->next_poll_s);
    } else {
        esl_tag_t *t = &st->tags[idx];
        t->seen = true;
        t->last_seen_ms = now_ms;
        t->shown_content_id = poll->content_id;
        t->battery_mv = poll->battery_mv;
        t->fw = poll->fw_ver;
        t->model = poll->model;
        t->rssi = rssi;
        t->poll_flags = poll->flags;
        t->cfg_capable = poll->has_cfg_ver;
        t->shown_cfg_ver = poll->cfg_ver;
        t->polls++;
        t->layout_capable = poll->has_layout_ver;
        t->layout_ver = poll->layout_ver;
        n = poll_answer(st, t, poll, env, now_ms, reply);
    }
    unlock(st);
    return n;
}

bool esl_store_fw_offer(esl_store_t *st, uint64_t mac, const esl_poll_t *poll, uint16_t ver)
{
    bool offer = false;
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0) {
        esl_tag_t *t = &st->tags[idx];
        if ((poll->flags & ESL_POLL_F_FWFAIL) && t->fw_offered)
            t->fw_failed = t->fw_offered;
        if (ver && poll->fw_ver == ver)
            t->fw_failed = 0;
        offer = ver && (poll->flags & ESL_POLL_F_OTA) && poll->fw_ver != ver && t->fw_failed != ver;
        if (offer)
            t->fw_offered = ver;
    }
    unlock(st);
    return offer;
}

void esl_store_fw_counts(esl_store_t *st, uint8_t model, uint16_t ver, esl_fw_counts_t *out)
{
    memset(out, 0, sizeof(*out));
    lock(st);
    for (unsigned i = 0; i < st->cap; i++) {
        const esl_tag_t *t = &st->tags[i];
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
    lock(st);
    int idx = find(st, mac);
    bool had_content = false, had_cfg = false;
    if (idx >= 0) {
        had_content = st->tags[idx].has_content;
        had_cfg = !esl_tag_cfg_is_default(&st->tags[idx].cfg);
        memset(&st->tags[idx], 0, sizeof(esl_tag_t));
    }
    unlock(st);
    if (idx >= 0 && had_content && st->ops.persist_erase)
        st->ops.persist_erase(st->ops.ctx, (unsigned)idx);
    if (idx >= 0 && had_cfg && st->ops.persist_cfg)
        st->ops.persist_cfg(st->ops.ctx, (unsigned)idx, mac, NULL);
    return idx >= 0;
}

bool esl_store_get(esl_store_t *st, uint64_t mac, esl_tag_t *out)
{
    lock(st);
    int idx = find(st, mac);
    if (idx >= 0)
        *out = st->tags[idx];
    unlock(st);
    return idx >= 0;
}

bool esl_store_get_slot(esl_store_t *st, unsigned slot, esl_tag_t *out)
{
    bool ok = false;
    lock(st);
    if (slot < st->cap && st->tags[slot].used) {
        *out = st->tags[slot];
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
        n += st->tags[i].used;
    unlock(st);
    return n;
}

unsigned esl_store_available(esl_store_t *st)
{
    unsigned n = 0;
    lock(st);
    for (unsigned i = 0; i < st->cap; i++)
        n += !st->tags[i].used || !esl_tag_holds_data(&st->tags[i]);
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

uint16_t esl_store_tag_poll_s(esl_store_t *st, uint64_t mac)
{
    lock(st);
    int idx = find(st, mac);
    uint16_t v = idx >= 0 ? tag_poll_s(st, &st->tags[idx]) : st->next_poll_s;
    unlock(st);
    return v;
}
