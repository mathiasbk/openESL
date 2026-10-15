#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"
#include "esl_persist.h"
#include "esl_store.h"

/* Scheduled content (prices from a given time) and the AP's wall clock.
 *
 * An entry is a tag, a time (Unix seconds, UTC) and content. When the time
 * has come, the content is set as if PUT /api/tags had sent it then. Only
 * MAC and time are kept in RAM (16 bytes per entry); the content is a blob
 * behind the ops (NVS on the device), loaded when the entry is due or listed.
 * A tag can have several entries (a sale that starts and one that ends it);
 * the same tag and time replaces the earlier entry.
 *
 * The AP has no clock of its own: the time comes from NTP while it is on WiFi,
 * or from PUT /api/time. Until then (also after every restart without NTP)
 * nothing is applied; entries that came due meanwhile are applied as soon as
 * the time is known, the latest one per tag.
 *
 * ops.lock is held around the save/load/erase hooks, and around
 * esl_store_set() in esl_sched_tick(), so it must not be the store's lock. */

#define ESL_TIME_UNKNOWN 0
#define ESL_TIME_API     1 /* set by PUT /api/time */
#define ESL_TIME_NTP     2

/* Times before this are refused (2020-09-13), so a value in milliseconds or
 * a small number by mistake gives an error instead of "now". */
#define ESL_SCHED_MIN_UNIX 1600000000u

/* Blob: at(4, LE) followed by an esl_persist blob */
#define ESL_SCHED_BLOB_MAX (4 + ESL_PERSIST_BLOB_MAX)

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void (*save)(void *ctx, unsigned slot, const uint8_t *blob, size_t len);
    /* *len is the room in blob on entry, the blob's length on return */
    bool (*load)(void *ctx, unsigned slot, uint8_t *blob, size_t *len);
    void (*erase)(void *ctx, unsigned slot);
} esl_sched_ops_t;

typedef struct {
    bool used;
    uint32_t at;
    uint64_t mac;
} esl_sched_entry_t;

typedef struct {
    esl_sched_entry_t *e;
    unsigned cap;
    esl_sched_ops_t ops;
    uint8_t time_src;  /* ESL_TIME_* */
    uint32_t base_unix; /* the time was base_unix at base_ms since boot */
    uint64_t base_ms;
    bool due_dirty;     /* entries or the time changed since the store last got the due times */
    uint32_t due_sent;  /* Unix seconds when it last did */
} esl_sched_t;

typedef enum {
    ESL_SCHED_OK = 0,
    ESL_SCHED_FULL,
} esl_sched_err_t;

/* e must hold cap entries; ops needs save, load and erase. */
void esl_sched_init(esl_sched_t *s, esl_sched_entry_t *e, unsigned cap, const esl_sched_ops_t *ops);
/* Loads a stored entry at boot. Does not call save. */
bool esl_sched_restore(esl_sched_t *s, unsigned slot, const uint8_t *blob, size_t len);

void esl_sched_set_time(esl_sched_t *s, uint32_t unix_s, uint64_t now_ms, uint8_t src);
/* Unix seconds now, 0 while the time is not known. src may be NULL. */
uint32_t esl_sched_now(esl_sched_t *s, uint64_t now_ms, uint8_t *src);
const char *esl_time_src_name(uint8_t src); /* "ntp", "api", NULL when unknown */

/* Adds or replaces the entry for (mac, at). */
esl_sched_err_t esl_sched_add(esl_sched_t *s, uint64_t mac, uint32_t at, const esl_content_t *c);
/* An entry for (mac, at) exists (adding it again takes no room). */
bool esl_sched_has(esl_sched_t *s, uint64_t mac, uint32_t at);
unsigned esl_sched_free(esl_sched_t *s);
unsigned esl_sched_count(esl_sched_t *s);
/* Entries for one tag. */
unsigned esl_sched_count_mac(esl_sched_t *s, uint64_t mac);
/* Removes every entry for mac (all entries when all is true); returns how many. */
unsigned esl_sched_remove(esl_sched_t *s, uint64_t mac, bool all);
/* Copies the entry in a slot, with its content (c may be NULL). False if unused. */
bool esl_sched_get_slot(esl_sched_t *s, unsigned slot, uint64_t *mac, uint32_t *at, esl_content_t *c);

/* Applies every entry that is due: the latest due entry per tag is set
 * (esl_store_set) and every due entry is removed. Then, when the entries or
 * the time have changed (and once a minute, for tags that joined the table
 * since), tells the store when each tag's next entry is due
 * (esl_store_note_due), so those tags poll just after it instead of after
 * their full interval. Call about once a second. Returns how many were applied. */
unsigned esl_sched_tick(esl_sched_t *s, esl_store_t *st, uint64_t now_ms);
