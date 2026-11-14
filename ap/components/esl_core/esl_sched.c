#include "esl_sched.h"
#include "esl_clock.h"
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

const char *esl_sched_repeat_name(uint8_t repeat)
{
    return repeat == ESL_SCHED_DAILY ? "daily" : repeat == ESL_SCHED_WEEKLY ? "weekly" : NULL;
}

int esl_sched_repeat_parse(const char *str, size_t len)
{
    if (len == 4 && memcmp(str, "once", 4) == 0)
        return ESL_SCHED_ONCE;
    if (len == 5 && memcmp(str, "daily", 5) == 0)
        return ESL_SCHED_DAILY;
    if (len == 6 && memcmp(str, "weekly", 6) == 0)
        return ESL_SCHED_WEEKLY;
    return -1;
}

static uint32_t period(uint8_t repeat)
{
    return repeat == ESL_SCHED_WEEKLY ? 7u * 86400u : 86400u;
}

static int32_t offset_at(const esl_sched_t *s, uint32_t t)
{
    return s->ops.utc_offset_at ? s->ops.utc_offset_at(s->ops.ctx, t) : esl_clock_utc_offset();
}

/* Where t falls in the local day or week, in seconds */
static uint32_t local_in(const esl_sched_t *s, uint32_t t, uint32_t p)
{
    int64_t l = ((int64_t)t + offset_at(s, t)) % (int64_t)p;
    return (uint32_t)(l < 0 ? l + p : l);
}

/* The time near t (within half a period) that is at local in the day or week: after a change
   of the UTC offset (daylight saving time) the same local time is an hour off in UTC */
static uint32_t align(const esl_sched_t *s, uint32_t t, uint32_t local, uint32_t p)
{
    for (int k = 0; k < 2; k++) { /* again with the offset at the new time */
        int64_t d = ((int64_t)local_in(s, t, p) - local) % (int64_t)p;
        if (d < 0)
            d += p;
        if (d >= p / 2)
            d -= p;
        t = (uint32_t)((int64_t)t - d);
    }
    return t;
}

/* The first time after now of an entry at "at" that repeats at local */
static uint32_t next_time(const esl_sched_t *s, uint32_t at, uint8_t repeat, uint32_t local, uint32_t now)
{
    uint32_t p = period(repeat);
    if (at > now)
        return at;
    uint32_t t = align(s, at + ((now - at) / p + 1) * p, local, p);
    while (t <= now)
        t = align(s, t + p, local, p);
    return t;
}

/* An entry's blob: at, mac and content, and for a recurring one repeat and its local time */
static size_t encode(uint8_t *blob, uint32_t at, uint64_t mac, const esl_content_t *c, uint8_t repeat, uint32_t local)
{
    for (int i = 0; i < 4; i++)
        blob[i] = (uint8_t)(at >> (8 * i));
    size_t len = 4 + esl_persist_encode(mac, c, &blob[4]);
    if (repeat != ESL_SCHED_ONCE) {
        blob[len++] = repeat;
        for (int i = 0; i < 4; i++)
            blob[len++] = (uint8_t)(local >> (8 * i));
        blob[len++] = ESL_SCHED_REPEAT_MAGIC;
    }
    return len;
}

static bool decode(const uint8_t *blob, size_t len, uint32_t *at, uint64_t *mac, esl_content_t *c, uint8_t *repeat,
                   uint32_t *local)
{
    if (len <= 4)
        return false;
    *at = (uint32_t)blob[0] | (uint32_t)blob[1] << 8 | (uint32_t)blob[2] << 16 | (uint32_t)blob[3] << 24;
    *repeat = ESL_SCHED_ONCE;
    *local = 0;
    if (len > 10 && blob[len - 1] == ESL_SCHED_REPEAT_MAGIC &&
        (blob[len - 6] == ESL_SCHED_DAILY || blob[len - 6] == ESL_SCHED_WEEKLY) &&
        esl_persist_decode(&blob[4], len - 10, mac, c)) {
        *repeat = blob[len - 6];
        *local = (uint32_t)blob[len - 5] | (uint32_t)blob[len - 4] << 8 | (uint32_t)blob[len - 3] << 16 |
                 (uint32_t)blob[len - 2] << 24;
        return *local < period(*repeat);
    }
    return esl_persist_decode(&blob[4], len - 4, mac, c);
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
    uint8_t blob[ESL_SCHED_BLOB_MAX], repeat;
    size_t len = sizeof(blob);
    uint64_t mac;
    uint32_t at, local;
    return s->ops.load(s->ops.ctx, i, blob, &len) && decode(blob, len, &at, &mac, c, &repeat, &local) &&
           mac == s->e[i].mac;
}

/* A recurring entry that was due moves on to its next time; dropped if its blob is gone */
static void move_on(esl_sched_t *s, unsigned i, uint32_t now)
{
    uint8_t blob[ESL_SCHED_BLOB_MAX], repeat;
    size_t len = sizeof(blob);
    uint64_t mac;
    uint32_t at, local;
    esl_content_t c;
    if (!s->ops.load(s->ops.ctx, i, blob, &len) || !decode(blob, len, &at, &mac, &c, &repeat, &local) ||
        repeat == ESL_SCHED_ONCE || mac != s->e[i].mac) {
        drop(s, i);
        return;
    }
    at = next_time(s, s->e[i].at, repeat, local, now);
    for (int k = 0; k < 4; k++)
        blob[k] = (uint8_t)(at >> (8 * k));
    s->e[i].at = at;
    s->due_dirty = true;
    s->ops.save(s->ops.ctx, i, blob, len);
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
    uint32_t at, local;
    uint8_t repeat;
    if (slot >= s->cap || s->e[slot].used || !decode(blob, len, &at, &mac, &c, &repeat, &local))
        return false;
    if (at < ESL_SCHED_MIN_UNIX || find(s, mac, at) >= 0)
        return false;
    s->e[slot] = (esl_sched_entry_t){true, repeat, at, mac};
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
    return esl_sched_add_repeat(s, mac, &at, ESL_SCHED_ONCE, 0, c);
}

esl_sched_err_t esl_sched_add_repeat(esl_sched_t *s, uint64_t mac, uint32_t *at_io, uint8_t repeat, uint32_t now,
                                     const esl_content_t *c)
{
    uint8_t blob[ESL_SCHED_BLOB_MAX];
    if (repeat > ESL_SCHED_WEEKLY)
        repeat = ESL_SCHED_ONCE;
    lock(s);
    uint32_t at = *at_io, local = 0;
    if (repeat != ESL_SCHED_ONCE) {
        local = local_in(s, at, period(repeat));
        if (now)
            at = next_time(s, at, repeat, local, now);
    }
    size_t len = encode(blob, at, mac, c, repeat, local);
    int slot = find(s, mac, at);
    for (unsigned i = 0; slot < 0 && i < s->cap; i++)
        if (!s->e[i].used)
            slot = (int)i;
    if (slot < 0) {
        unlock(s);
        return ESL_SCHED_FULL;
    }
    s->e[slot] = (esl_sched_entry_t){true, repeat, at, mac};
    s->due_dirty = true;
    s->ops.save(s->ops.ctx, (unsigned)slot, blob, len);
    unlock(s);
    *at_io = at;
    return ESL_SCHED_OK;
}

uint8_t esl_sched_slot_repeat(esl_sched_t *s, unsigned slot)
{
    lock(s);
    uint8_t r = slot < s->cap && s->e[slot].used ? s->e[slot].repeat : ESL_SCHED_ONCE;
    unlock(s);
    return r;
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
        /* Of the tag's entries that are due, the one due last wins: a recurring entry by
           the last of its times that has passed (one period apart; near enough to compare) */
        uint64_t mac = e->mac;
        int win = -1;
        uint32_t best = 0;
        for (unsigned j = 0; j < s->cap; j++) {
            const esl_sched_entry_t *o = &s->e[j];
            if (!o->used || o->mac != mac || o->at > now)
                continue;
            uint32_t last = o->at;
            if (o->repeat != ESL_SCHED_ONCE)
                last += (now - o->at) / period(o->repeat) * period(o->repeat);
            if (win < 0 || last > best) {
                win = (int)j;
                best = last;
            }
        }
        esl_content_t c;
        esl_set_result_t r;
        bool loaded = load(s, (unsigned)win, &c);
        /* Dropped first: that frees their NVS, which the content may need */
        for (unsigned j = 0; j < s->cap; j++)
            if (s->e[j].used && s->e[j].mac == mac && s->e[j].at <= now) {
                if (s->e[j].repeat != ESL_SCHED_ONCE)
                    move_on(s, j, now);
                else
                    drop(s, j); /* the winner, and the earlier ones it replaces */
            }
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
