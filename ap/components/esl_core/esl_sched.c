#include "esl_sched.h"
#include <string.h>

static void lock(esl_sched_t *s)
{
    if (s->ops.lock)
        s->ops.lock(s->ops.ctx);
}

static void unlock(esl_sched_t *s)
{
    if (s->ops.unlock)
        s->ops.unlock(s->ops.ctx);
}

static int find(const esl_sched_t *s, uint64_t mac, uint32_t at)
{
    for (unsigned i = 0; i < s->cap; i++)
        if (s->e[i].used && s->e[i].mac == mac && s->e[i].at == at)
            return (int)i;
    return -1;
}

static void drop(esl_sched_t *s, unsigned i)
{
    s->e[i].used = false;
    s->due_dirty = true;
    s->ops.erase(s->ops.ctx, i);
}

/* The content of slot i; false if the stored blob is missing or bad. */
static bool load(esl_sched_t *s, unsigned i, esl_content_t *c)
{
    uint8_t blob[ESL_SCHED_BLOB_MAX];
    size_t len = sizeof(blob);
    uint64_t mac;
    return s->ops.load(s->ops.ctx, i, blob, &len) && len > 4 && esl_persist_decode(&blob[4], len - 4, &mac, c) &&
           mac == s->e[i].mac;
}

void esl_sched_init(esl_sched_t *s, esl_sched_entry_t *e, unsigned cap, const esl_sched_ops_t *ops)
{
    memset(s, 0, sizeof(*s));
    memset(e, 0, sizeof(*e) * cap);
    s->e = e;
    s->cap = cap;
    s->ops = *ops;
}

bool esl_sched_restore(esl_sched_t *s, unsigned slot, const uint8_t *blob, size_t len)
{
    uint64_t mac;
    esl_content_t c;
    if (slot >= s->cap || s->e[slot].used || len <= 4 || !esl_persist_decode(&blob[4], len - 4, &mac, &c))
        return false;
    uint32_t at = (uint32_t)blob[0] | (uint32_t)blob[1] << 8 | (uint32_t)blob[2] << 16 | (uint32_t)blob[3] << 24;
    if (at < ESL_SCHED_MIN_UNIX || find(s, mac, at) >= 0)
        return false;
    s->e[slot] = (esl_sched_entry_t){true, at, mac};
    s->due_dirty = true;
    return true;
}

void esl_sched_set_time(esl_sched_t *s, uint32_t unix_s, uint64_t now_ms, uint8_t src)
{
    lock(s);
    s->base_unix = unix_s;
    s->base_ms = now_ms;
    s->time_src = src;
    s->due_dirty = true;
    unlock(s);
}

static uint32_t now_locked(const esl_sched_t *s, uint64_t now_ms)
{
    if (s->time_src == ESL_TIME_UNKNOWN)
        return 0;
    return s->base_unix + (uint32_t)((now_ms - s->base_ms) / 1000);
}

uint32_t esl_sched_now(esl_sched_t *s, uint64_t now_ms, uint8_t *src)
{
    lock(s);
    uint32_t t = now_locked(s, now_ms);
    if (src)
        *src = s->time_src;
    unlock(s);
    return t;
}

const char *esl_time_src_name(uint8_t src)
{
    return src == ESL_TIME_NTP ? "ntp" : src == ESL_TIME_API ? "api" : NULL;
}

esl_sched_err_t esl_sched_add(esl_sched_t *s, uint64_t mac, uint32_t at, const esl_content_t *c)
{
    uint8_t blob[ESL_SCHED_BLOB_MAX];
    for (int i = 0; i < 4; i++)
        blob[i] = (uint8_t)(at >> (8 * i));
    size_t len = 4 + esl_persist_encode(mac, c, &blob[4]);

    lock(s);
    int slot = find(s, mac, at);
    for (unsigned i = 0; slot < 0 && i < s->cap; i++)
        if (!s->e[i].used)
            slot = (int)i;
    if (slot < 0) {
        unlock(s);
        return ESL_SCHED_FULL;
    }
    s->e[slot] = (esl_sched_entry_t){true, at, mac};
    s->due_dirty = true;
    s->ops.save(s->ops.ctx, (unsigned)slot, blob, len);
    unlock(s);
    return ESL_SCHED_OK;
}

bool esl_sched_has(esl_sched_t *s, uint64_t mac, uint32_t at)
{
    lock(s);
    bool r = find(s, mac, at) >= 0;
    unlock(s);
    return r;
}

unsigned esl_sched_count(esl_sched_t *s)
{
    unsigned n = 0;
    lock(s);
    for (unsigned i = 0; i < s->cap; i++)
        n += s->e[i].used;
    unlock(s);
    return n;
}

unsigned esl_sched_free(esl_sched_t *s)
{
    return s->cap - esl_sched_count(s);
}

unsigned esl_sched_count_mac(esl_sched_t *s, uint64_t mac)
{
    unsigned n = 0;
    lock(s);
    for (unsigned i = 0; i < s->cap; i++)
        n += s->e[i].used && s->e[i].mac == mac;
    unlock(s);
    return n;
}

unsigned esl_sched_remove(esl_sched_t *s, uint64_t mac, bool all)
{
    unsigned n = 0;
    lock(s);
    for (unsigned i = 0; i < s->cap; i++)
        if (s->e[i].used && (all || s->e[i].mac == mac)) {
            drop(s, i);
            n++;
        }
    unlock(s);
    return n;
}

bool esl_sched_get_slot(esl_sched_t *s, unsigned slot, uint64_t *mac, uint32_t *at, esl_content_t *c)
{
    bool ok = false;
    lock(s);
    if (slot < s->cap && s->e[slot].used && (!c || load(s, slot, c))) {
        *mac = s->e[slot].mac;
        *at = s->e[slot].at;
        ok = true;
    }
    unlock(s);
    return ok;
}

unsigned esl_sched_tick(esl_sched_t *s, esl_store_t *st, uint64_t now_ms)
{
    unsigned applied = 0;
    lock(s);
    uint32_t now = now_locked(s, now_ms);
    for (unsigned i = 0; now && i < s->cap; i++) {
        esl_sched_entry_t *e = &s->e[i];
        if (!e->used || e->at > now)
            continue;
        /* A later entry for the same tag that is also due wins */
        bool superseded = false;
        for (unsigned j = 0; j < s->cap && !superseded; j++)
            superseded = s->e[j].used && s->e[j].mac == e->mac && s->e[j].at <= now && s->e[j].at > e->at;
        esl_content_t c;
        esl_set_result_t r;
        if (superseded)
            continue; /* dropped with the one that wins */
        bool loaded = load(s, i, &c);
        uint64_t mac = e->mac;
        /* Dropped first: that frees their NVS, which the content may need */
        for (unsigned j = 0; j < s->cap; j++)
            if (s->e[j].used && s->e[j].mac == mac && s->e[j].at <= now)
                drop(s, j); /* this one, and the earlier ones it replaces */
        if (loaded && esl_store_set(st, mac, &c, &r) == ESL_STORE_OK)
            applied++;
    }
    if (now && (s->due_dirty || now - s->due_sent >= 60)) {
        /* due times in seconds since boot, as the store counts */
        uint32_t boot_s = (uint32_t)(now_ms / 1000);
        esl_store_clear_due(st);
        for (unsigned i = 0; i < s->cap; i++)
            if (s->e[i].used)
                esl_store_note_due(st, s->e[i].mac, boot_s + (s->e[i].at - now));
        s->due_dirty = false;
        s->due_sent = now;
    }
    unlock(s);
    return applied;
}
