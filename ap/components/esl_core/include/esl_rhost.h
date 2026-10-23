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
 *   1. replays every logged frame through esl_radio_handle() (esl_rhost_replay), at
 *      the time it came on the host's clock, so the store records the tags as if the
 *      host had heard them itself: last seen, battery, what they show, what they took
 *      and refused. The replies go nowhere: the radio has answered already.
 *   2. answers with what has changed (esl_rhost_answer): the state of every tag that
 *      differs from what the radio was last told (what esl_radio_handle() would answer
 *      it with), the images it lacks for them, the tag firmware per model, the default
 *      interval, and the tags deleted since.
 * It never sends anything on its own: the radio decides when WiFi is used.
 *
 * Drawing is the slow part, so a sync draws at most max_draws images; a tag whose
 * image is not drawn yet keeps its old state at the radio until a later sync. Likewise
 * at most max_images images go to the radio per sync (each costs it tag replies).
 *
 * What goes to the radio goes through esl_rhost_out_t, so the transport (TCP on the
 * S3, direct calls in the tests) is not this module's. One task runs it; the store,
 * the images and the tag firmware take their own locks. */

#define ESL_RHOST_DRAWS  8   /* images drawn per sync */
#define ESL_RHOST_IMAGES 4   /* images sent per sync */

typedef struct {
    void *ctx;
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
} esl_rhost_out_t;

typedef struct {
    uint32_t syncs, replayed;
    uint32_t tags, drops, images, fws;   /* sent to the radio */
    uint32_t drawn;
    uint32_t refused;                    /* states or images the radio had no room for */
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
    esl_rhost_stats_t stats;
} esl_rhost_t;

/* sent holds max_sent entries (the store's capacity); img img_cap bytes, at least
 * esl_image_mem_size() / ESL_IMAGE_BUFS. im (the AP's images, so ids match what it
 * draws) and fw may be NULL. */
void esl_rhost_init(esl_rhost_t *h, esl_store_t *st, esl_layouts_t *ls, esl_image_t *im, esl_tagfw_t *fw,
                    uint16_t pan, esl_rtag_state_t *sent, unsigned max_sent, uint8_t *img, size_t img_cap);

/* The radio is new to the host (connected, or restarted): it is told everything again. */
void esl_rhost_reset(esl_rhost_t *h);

/* One entry of the radio's log, replayed at its time on the host's clock (now_ms, as
 * the radio's req->now_ms). */
void esl_rhost_replay(esl_rhost_t *h, const esl_rsync_t *req, const esl_rlog_t *e, uint64_t now_ms);

/* After the log: what the radio needs, through out. True when there is more than one
 * sync could carry (images not drawn or sent yet): the radio should sync again soon
 * (esl_rcache_want_more). */
bool esl_rhost_answer(esl_rhost_t *h, const esl_rsync_t *req, uint64_t now_ms, const esl_rhost_out_t *out);
