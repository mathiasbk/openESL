#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"
#include "esl_proto.h"

/* Table of tags: MAC -> content + last reported status.
 *
 * Content is persisted through the persist hooks (NVS on the device) when it
 * changes; status lives in RAM only. The ready-made radio reply for each tag
 * is kept in the table so answering a POLL is a copy (krav A3).
 *
 * HTTP and radio run in different tasks, so every public function takes the
 * lock. The persist hooks are called after the lock is released. */

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void (*persist_save)(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c);
    void (*persist_erase)(void *ctx, unsigned slot);
} esl_store_ops_t;

typedef struct {
    bool used;
    uint64_t mac;

    bool has_content;
    esl_content_t content;
    uint32_t content_id;      /* 0 when no content is set */
    uint8_t reply[ESL_MAX_PAYLOAD];
    uint8_t reply_len;

    bool seen;                /* has polled since boot */
    uint64_t last_seen_ms;
    uint32_t shown_content_id;
    uint16_t battery_mv;
    uint16_t fw;
    uint8_t model;            /* ESL_MODEL_*, from the last POLL */
    int8_t rssi;
    uint8_t poll_flags;
    uint32_t polls;
} esl_tag_t;

typedef struct {
    esl_tag_t *tags;
    unsigned cap;
    uint16_t next_poll_s;
    esl_store_ops_t ops;
} esl_store_t;

typedef enum {
    ESL_STORE_OK = 0,
    ESL_STORE_FULL,
} esl_store_err_t;

typedef struct {
    uint32_t content_id;
    bool changed;
    bool pending;
} esl_set_result_t;

/* tags must hold cap entries. ops may be NULL (no locking, no persistence). */
void esl_store_init(esl_store_t *st, esl_tag_t *tags, unsigned cap, const esl_store_ops_t *ops, uint16_t next_poll_s);

/* Loads persisted content at boot. Does not call persist_save. */
bool esl_store_restore(esl_store_t *st, unsigned slot, uint64_t mac, const esl_content_t *c);

/* Sets content for a tag, creating it if needed. If the table is full, the
 * longest-unseen tag without content is replaced. */
esl_store_err_t esl_store_set(esl_store_t *st, uint64_t mac, const esl_content_t *c, esl_set_result_t *res);

/* Handles a POLL: records status and writes IDLE or CONTENT into reply
 * (ESL_MAX_PAYLOAD bytes). Unknown tags are added without content if there is
 * room. Returns the reply length. */
size_t esl_store_on_poll(esl_store_t *st, uint64_t mac, const esl_poll_t *poll, int8_t rssi,
                         uint64_t now_ms, uint8_t *reply);

bool esl_store_delete(esl_store_t *st, uint64_t mac);
bool esl_store_get(esl_store_t *st, uint64_t mac, esl_tag_t *out);
/* Copies the tag in a slot; false if the slot is unused. */
bool esl_store_get_slot(esl_store_t *st, unsigned slot, esl_tag_t *out);
unsigned esl_store_count(esl_store_t *st);
/* Slots a new MAC could take (free, or held by a tag without content). */
unsigned esl_store_available(esl_store_t *st);
void esl_store_set_next_poll(esl_store_t *st, uint16_t next_poll_s);

/* Content is set and the tag has not reported showing it yet. */
bool esl_tag_pending(const esl_tag_t *t);
