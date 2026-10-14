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

/* Tag without content that was seen longest ago. */
static int find_evictable(const esl_store_t *st)
{
    int best = -1;
    for (unsigned i = 0; i < st->cap; i++) {
        const esl_tag_t *t = &st->tags[i];
        if (!t->used || t->has_content)
            continue;
        if (best < 0 || t->last_seen_ms < st->tags[best].last_seen_ms)
            best = (int)i;
    }
    return best;
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

/* Under the lock: the answer to a POLL for a known tag. 0 = send the layout file first. */
static size_t poll_answer(esl_store_t *st, esl_tag_t *t, const esl_poll_t *poll, const esl_layout_env_t *env,
                          uint8_t *reply)
{
    uint32_t v = env ? env->version : 0;

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
    if (!t->has_content || poll->content_id == t->content_id)
        return esl_idle_build(reply, st->next_poll_s);
    if (t->content.layout != ESL_LAYOUT_NAMED)
        return esl_content_msg_build(reply, st->next_poll_s, t->content_id, &t->content);

    int idx = env_find(env, t->content.name);
    if (!poll->has_layout_ver)
        t->content_err = ESL_CERR_NO_LAYOUTS;
    else if (idx < 0)
        t->content_err = ESL_CERR_NOT_IN_FILE;
    else if (poll->layout_ver != v)
        t->content_err = ESL_CERR_REJECTED; /* see the top: only reached when it rejected v */
    if (t->content_err)
        return esl_idle_build(reply, st->next_poll_s);
    esl_content_t sent;
    esl_content_resolve(&t->content, (uint8_t)idx, &sent);
    return esl_content_msg_build(reply, st->next_poll_s, t->content_id, &sent);
}

bool esl_tag_pending(const esl_tag_t *t)
{
    return t->has_content && (!t->seen || t->shown_content_id != t->content_id);
}

void esl_store_init(esl_store_t *st, esl_tag_t *tags, unsigned cap, const esl_store_ops_t *ops, uint16_t next_poll_s)
{
    memset(st, 0, sizeof(*st));
    memset(tags, 0, sizeof(*tags) * cap);
    st->tags = tags;
    st->cap = cap;
    st->next_poll_s = next_poll_s;
    if (ops)
        st->ops = *ops;
}

bool esl_store_restore(esl_store_t *st, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    bool ok = false;
    lock(st);
    if (slot < st->cap && !st->tags[slot].used && find(st, mac) < 0) {
        esl_tag_t *t = &st->tags[slot];
        memset(t, 0, sizeof(*t));
        t->used = true;
        t->mac = mac;
        set_content(t, c);
        ok = true;
    }
    unlock(st);
    return ok;
}

esl_store_err_t esl_store_set(esl_store_t *st, uint64_t mac, const esl_content_t *c, esl_set_result_t *res)
{
    bool persist = false;
    lock(st);
    int idx = find(st, mac);
    if (idx < 0) {
        idx = find_free(st);
        if (idx < 0)
            idx = find_evictable(st);
        if (idx < 0) {
            unlock(st);
            return ESL_STORE_FULL;
        }
        memset(&st->tags[idx], 0, sizeof(esl_tag_t));
        st->tags[idx].used = true;
        st->tags[idx].mac = mac;
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
        t->polls++;
        t->layout_capable = poll->has_layout_ver;
        t->layout_ver = poll->layout_ver;
        n = poll_answer(st, t, poll, env, reply);
    }
    unlock(st);
    return n;
}

bool esl_store_delete(esl_store_t *st, uint64_t mac)
{
    lock(st);
    int idx = find(st, mac);
    bool had_content = false;
    if (idx >= 0) {
        had_content = st->tags[idx].has_content;
        memset(&st->tags[idx], 0, sizeof(esl_tag_t));
    }
    unlock(st);
    if (idx >= 0 && had_content && st->ops.persist_erase)
        st->ops.persist_erase(st->ops.ctx, (unsigned)idx);
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
        n += !st->tags[i].used || !st->tags[i].has_content;
    unlock(st);
    return n;
}

void esl_store_set_next_poll(esl_store_t *st, uint16_t next_poll_s)
{
    lock(st);
    st->next_poll_s = next_poll_s;
    unlock(st);
}

uint16_t esl_store_next_poll(esl_store_t *st)
{
    lock(st);
    uint16_t s = st->next_poll_s;
    unlock(st);
    return s;
}
