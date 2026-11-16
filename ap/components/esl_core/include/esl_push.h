#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_image.h"
#include "esl_layout.h"
#include "esl_store.h"

/* Tags the AP sends pictures to itself: Bluetooth tags with their maker's firmware
 * (docs/ble-tags.md). Such a tag never polls. The AP takes turns instead: a tag whose
 * content gives another image than the one it was last given gets the new one through
 * the transport (esl_push_out_t: the maker's protocol over BLE on the device, a fake in
 * the tests).
 *
 * The store keeps such a tag as it keeps any other. After every attempt that reached it,
 * and whenever the scan hears it (esl_push_seen), the store is told as if the tag had
 * polled (esl_store_on_poll with a POLL made here: its model, battery, and the id of
 * the image it was given as what it shows). The dashboard, the API, "last seen", the
 * battery and "shows its content" then work for it unchanged.
 *
 * A tag that cannot be reached is tried again after ESL_PUSH_RETRY_MS, doubling up to
 * ESL_PUSH_RETRY_MAX_MS; a tag that answers but refuses an image (ESL_PUSH_REFUSED) is
 * not sent that image again, until its content changes. Images are drawn only for a tag
 * that needs one (esl_image_draw into this module's own buffer), so a round over tags
 * that show their content costs a store lookup each.
 *
 * One task runs it: the send call blocks for as long as the transfer takes (seconds over
 * BLE). The store and the images take their own locks, but this table has none, so a scan
 * that reports from another task (the BLE host's callback) hands its reports to that task,
 * which calls esl_push_seen(). */

#ifndef ESL_PUSH_TAGS
#define ESL_PUSH_TAGS 64
#endif
#define ESL_PUSH_RETRY_MS     (30 * 1000ULL)
#define ESL_PUSH_RETRY_MAX_MS (30 * 60 * 1000ULL)
/* A tag advertises about once a second: the store is told it was heard at most this often */
#define ESL_PUSH_SEEN_MS      (60 * 1000ULL)

typedef enum {
    ESL_PUSH_SENT = 0, /* the tag took the image and shows it */
    ESL_PUSH_AWAY,     /* not reached: not found, no connection, or the link broke */
    ESL_PUSH_REFUSED,  /* reached, but it said no (a size or format it does not take) */
} esl_push_res_t;

/* What the transport learnt from the tag; 0 = not known */
typedef struct {
    uint16_t battery_mv;
    int8_t rssi;
} esl_push_info_t;

typedef struct {
    void *ctx;
    /* Sends the image (len bytes as esl_render draws it for model: the black plane, then
       the red plane on a model with red) to the tag. Blocks until done. info is zeroed
       before the call. */
    esl_push_res_t (*send)(void *ctx, uint64_t mac, uint8_t model, const uint8_t *img, size_t len,
                           esl_push_info_t *info);
} esl_push_out_t;

typedef struct {
    uint64_t mac;      /* 0 = free */
    uint8_t model;     /* ESL_MODEL_* */
    uint8_t fails;     /* attempts in a row that did not reach it */
    bool woken;        /* the scan pulled its next try forward once in this run of misses */
    bool dirty;        /* shown changed since esl_push_saved(): to be kept in NVS */
    int8_t rssi;       /* last known, 0 = none yet */
    uint16_t battery_mv;
    uint32_t shown;    /* the image id it was last given, 0 = none since it was added */
    uint32_t refused;  /* an image id it refused: not sent again */
    uint64_t next_ms;  /* not tried before this */
    uint64_t noted_ms; /* when the store was last told the scan heard it */
} esl_push_tag_t;

typedef struct {
    uint32_t sent, away, refused, drawn;
} esl_push_stats_t;

typedef struct {
    esl_push_tag_t tags[ESL_PUSH_TAGS];
    esl_push_out_t out;
    uint8_t *buf;      /* cap bytes: an image of any model */
    size_t cap;
    unsigned cursor;   /* where the next round starts, so a low max_sends reaches every tag */
    esl_push_stats_t stats;
} esl_push_t;

/* buf holds cap bytes (esl_image_mem_size() / ESL_IMAGE_BUFS, as esl_rhost). */
void esl_push_init(esl_push_t *p, const esl_push_out_t *out, uint8_t *buf, size_t cap);

/* A tag the AP sends to (paired in the web page, or restored from NVS with the id it
   was last given, 0 when not known). False when the table is full or mac is 0; a tag
   already in it gets the new model. */
bool esl_push_add(esl_push_t *p, uint64_t mac, uint8_t model, uint32_t shown);
bool esl_push_remove(esl_push_t *p, uint64_t mac);
/* The tag's entry, NULL when it is not one the AP sends to */
const esl_push_tag_t *esl_push_get(const esl_push_t *p, uint64_t mac);
unsigned esl_push_count(const esl_push_t *p);

/* The scan heard the tag (rssi; battery_mv 0 = not known): the store records it as seen
   (at most every ESL_PUSH_SEEN_MS), and a tag that could not be reached is tried at the
   next round, once per run of misses: a tag that is heard but cannot be connected to keeps
   its back-off. False when it is not one the AP sends to. */
bool esl_push_seen(esl_push_t *p, esl_store_t *st, esl_layouts_t *ls, uint64_t mac, int8_t rssi,
                   uint16_t battery_mv, uint64_t now_ms);

/* One turn over the tags: each that is due and needs another image gets it, at most
   max_sends of them, starting after the last tag the previous round sent to. Returns how
   many were sent to (whatever the result). */
/* The tag whose shown id changed since it was last saved (dirty), NULL when none: the
   caller keeps mac and shown in NVS, then calls esl_push_saved(). */
const esl_push_tag_t *esl_push_unsaved(const esl_push_t *p);
void esl_push_saved(esl_push_t *p, uint64_t mac);

unsigned esl_push_round(esl_push_t *p, esl_store_t *st, esl_layouts_t *ls, esl_image_t *im, uint64_t now_ms,
                        unsigned max_sends);
