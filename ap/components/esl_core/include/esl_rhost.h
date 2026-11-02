#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_image.h"
#include "esl_layout.h"
#include "esl_rcache.h"
#include "esl_store.h"
#include "esl_tagfw.h"

/* The host's side of a network radio (docs/ap-netradio.md): the S3 that a wireless C6
 * radio (esl_rcache) syncs with.
 *
 * At a sync the radio sends what it got from the tags (its log) and what it holds
 * (esl_rsync_t). The host
 *   0. checks who it talks to (esl_rhost_begin): a radio that restarted, a host that
 *      restarted, or an answer the radio did not get whole means the radio is told
 *      everything again, after it forgets the tags it has;
 *   1. replays every logged frame (esl_rhost_replay, through esl_radio_replay()) at the
 *      time it came on the host's clock, so the store records the tags as if the host
 *      had heard them itself: last seen, battery, what they show, what they took and
 *      refused. Nothing is drawn, and a frame it replayed already (a log sent again
 *      after a lost answer) is skipped;
 *   2. answers with what has changed (esl_rhost_answer): the state of every tag that
 *      differs from what the radio was last told (what esl_radio_handle() would answer
 *      it with), the images it lacks for tags that do not show them yet, the tag
 *      firmware per model, the default interval and the tags deleted since; then the
 *      answer's seq, which the radio acknowledges in its next sync.
 * It never sends anything on its own: the radio decides when WiFi is used.
 *
 * Images: first those of the tags the radio lacked something for (req->wants), which
 * are polling now; then, while the radio has slots to spare (req->free_images), those
 * of other tags, so they are there when the tags wake. A sync draws at most max_draws
 * and sends at most max_images (each costs the radio tag replies); a tag whose image is
 * not drawn yet keeps its old state at the radio until a later sync, and when more is
 * waiting the radio is told to come back soon.
 *
 * What goes to the radio goes through esl_rhost_out_t, so the transport (TCP on the
 * S3, direct calls in the tests) is not this module's. One task runs it; the store,
 * the images and the tag firmware take their own locks. A sync looks at every slot of
 * the store and its interval (esl_store_tag_poll_s, a search per tag): fine for the
 * 1000 tags of an S3, every 10 s. */

#define ESL_RHOST_DRAWS  8   /* images drawn per sync */
#define ESL_RHOST_NONE   0xFF /* esl_rowner_t.radio: no radio */

/* Several radios on one host (docs/ap-netradio.md, "Several radios"), each on its own
 * channel with its own esl_rhost_t. A tag talks to one radio at a time: the one on the
 * channel it found an answer on. The radio that last heard a tag owns it: only that
 * radio is told its state and gets its images, and a radio that loses a tag to another
 * is told to drop it. A tag no radio has heard since the host started is told to every
 * radio, as with one radio, but its image goes ahead only when one radio is connected.
 * A tag that turns up at a radio that dropped it is a tag that radio lacks: it asks,
 * the log shows the tag there, and it owns the tag from then on. */
typedef struct {
    uint64_t mac;      /* the tag in that slot when it was heard, 0 = none */
    uint64_t at_ms;    /* when, on the host's clock */
    uint8_t radio;     /* esl_rhost_t.index of the radio that heard it */
} esl_rowner_t;

typedef struct {
    esl_rowner_t *by_slot;   /* by store slot, n entries */
    unsigned n;
    uint8_t radios;          /* radios connected now (set by the transport) */
} esl_rowners_t;

/* by_slot holds n entries (the store's max). */
void esl_rowners_init(esl_rowners_t *o, esl_rowner_t *by_slot, unsigned n);
/* The radio that last heard the tag (esl_rhost_t.index), ESL_RHOST_NONE when none has. */
uint8_t esl_rowners_get(const esl_rowners_t *o, esl_store_t *st, uint64_t mac);
/* Tags the radio owns now. */
unsigned esl_rowners_count(const esl_rowners_t *o, uint8_t radio);
/* The radio is gone for good (forgotten on the host): its tags are nobody's. */
void esl_rowners_forget(esl_rowners_t *o, uint8_t radio);

/* The channel for a new radio with a channel of its own: the first that taken (bit c for
 * channel c: the AP's, a move it plans, the other radios') leaves free, in the order a
 * tag that lost its radio looks (CFG_SCAN_CHANNELS in tag/src/config.h: 15, 20, 26, 11
 * first, so it finds the radio within its first rounds, then 12..25). 0 when all are taken. */
uint8_t esl_rhost_pick_channel(uint32_t taken);
#define ESL_RHOST_IMAGES 4   /* images sent per sync */

typedef struct {
    void *ctx;
    /* Forget every tag (esl_rcache_clear_tags): their states follow. */
    void (*clear)(void *ctx);
    /* The state of a tag; false when the radio has no room for it (sent again next time). */
    bool (*tag)(void *ctx, const esl_rtag_state_t *s);
    /* A tag deleted on the host. */
    void (*drop)(void *ctx, uint64_t mac);
    /* An image (as esl_image draws it); false when the radio cannot take it. */
    bool (*image)(void *ctx, uint32_t id, uint8_t model, uint8_t fmt, const uint8_t *data, size_t size);
    /* The tag firmware for model is now img (ver 0: none); the transport sends its bytes. */
    void (*fw)(void *ctx, uint8_t model, const esl_tagfw_img_t *img);
    /* The default interval, for tags the radio has no state for. */
    void (*default_poll)(void *ctx, uint16_t poll_s);
    /* The end of the answer (esl_rcache_sync_end): its seq, and whether more is waiting. */
    void (*end)(void *ctx, uint32_t seq, bool more);
} esl_rhost_out_t;

typedef struct {
    uint32_t syncs, replayed, skipped;   /* skipped: log entries replayed already */
    uint32_t resets;                     /* times the radio was told everything again */
    uint32_t tags, drops, images, fws;   /* sent to the radio */
    uint32_t drawn;
    uint32_t refused;                    /* states or images the radio had no room for */
    uint32_t radio_log_dropped;          /* frames the radio's log lost (req->log_dropped) */
} esl_rhost_stats_t;

typedef struct {
    esl_store_t *st;
    esl_layouts_t *ls;
    esl_image_t *im;
    esl_tagfw_t *fw;
    uint16_t pan;
    esl_rtag_state_t *sent;   /* what the radio was told, by store slot (mac 0 = nothing) */
    unsigned max_sent;
    uint8_t *img;             /* for drawing: img_cap bytes */
    size_t img_cap;
    uint32_t img_id;          /* the image in img, 0 = none */
    uint8_t img_model, img_fmt;
    uint16_t img_size;
    uint16_t default_sent;    /* 0 = not sent */
    unsigned max_draws, max_images;
    uint8_t fw_models;        /* the radio holds tag firmware for models 1..fw_models only (0: any) */
    esl_rowners_t *owners;    /* several radios: who owns which tag (NULL: one radio, it serves every tag) */
    uint8_t index;            /* this radio's number in owners and the store (esl_tag_t.radio): 0 is on the
                                 AP's channel, 1.. on esl_store_set_radio_channel()'s */

    bool have_radio;          /* radio_boot is the radio's */
    uint32_t radio_boot;
    uint32_t seq;             /* of the last answer */
    bool clear;               /* the radio is to forget its tags first (esl_rhost_begin) */
    bool replayed_any;
    uint32_t replay_next;     /* the log seq after the last frame replayed */
    esl_rhost_stats_t stats;
} esl_rhost_t;

/* sent holds max_sent entries: the store's max (it takes no more tags), not its slots
 * so far; img img_cap bytes, at least esl_image_mem_size() / ESL_IMAGE_BUFS. im (the
 * AP's images, so ids match what it draws) and fw may be NULL. */
void esl_rhost_init(esl_rhost_t *h, esl_store_t *st, esl_layouts_t *ls, esl_image_t *im, esl_tagfw_t *fw,
                    uint16_t pan, esl_rtag_state_t *sent, unsigned max_sent, uint8_t *img, size_t img_cap);

/* With several radios: this one is radio index (below ESL_STORE_RADIOS) in owners and
 * the store. Radio 0 is on the AP's channel; the channel of another is set with
 * esl_store_set_radio_channel(), and its tags are told it in CONFIG. */
void esl_rhost_share(esl_rhost_t *h, esl_rowners_t *owners, uint8_t index);

/* The start of a sync, before its log: whether the radio is to be told everything again. */
void esl_rhost_begin(esl_rhost_t *h, const esl_rsync_t *req);

/* One entry of the radio's log, replayed at its time on the host's clock (now_ms, as
 * the radio's req->now_ms). */
void esl_rhost_replay(esl_rhost_t *h, const esl_rsync_t *req, const esl_rlog_t *e, uint64_t now_ms);

/* After the log: what the radio needs, through out, ending with out->end. */
void esl_rhost_answer(esl_rhost_t *h, const esl_rsync_t *req, const esl_rhost_out_t *out);
