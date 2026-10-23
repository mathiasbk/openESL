#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_proto.h"

/* The reply cache of a network radio: an ESP32-C6 that reaches its AP host (an
 * ESP32-S3) over WiFi instead of a cable (docs/ap-netradio.md).
 *
 * Over WiFi a reply cannot come from the host in time: a tag listens 20 ms after its
 * request, and a round trip to the C6 takes 20-100 ms. So the radio answers every tag
 * itself, from what the host told it beforehand, and the host learns afterwards what
 * happened: every frame the radio got goes into a log the host fetches and runs
 * through esl_radio_handle(), so its table of tags ends up as if it had heard them
 * itself, only later.
 *
 * What the host tells it, per tag (esl_rtag_state_t): the interval to send, the
 * settings (CONFIG when the tag reports others), the image it should show and the
 * tag firmware version to offer. Images and firmware come separately, by id and by
 * model. The radio picks the reply as esl_radio_handle() does, in the same order:
 *   POLL     CONFIG when the tag's cfg_ver is not the settings' version, else the
 *            first IMG block when it shows another image than its own (and has not
 *            refused that one), else the first FW block when it runs another version
 *            (and has not refused that one), else IDLE
 *   IMG_REQ  the block asked for of the tag's image, block 0 when the tag asks for
 *            another id (its image changed meanwhile), IDLE when there is none
 *   FW_REQ   the block asked for of its model's firmware, likewise
 * When it lacks something (a tag it has no state for, or an image or firmware it has
 * not got yet) the reply is IDLE with a short interval, ESL_RCACHE_WAIT_S, and the
 * radio asks the host for it at its next sync; after ESL_RCACHE_WAIT_TRIES such replies
 * in a row the tag gets its own interval again (the host's default for a tag it knows
 * nothing of: a tag keeps the last interval it got, so it has to be sent), so a host that cannot deliver (a full table, an image too large)
 * does not keep it awake.
 *
 * WiFi on the C6 takes the radio from 802.15.4 for 80-95 ms per exchange
 * (docs/ap-netradio.md, "Limits"), so the radio syncs with the host only when no
 * tag is fetching blocks, and not more often than it has to: esl_rcache_sync_due().
 *
 * No ESP-IDF headers and no locking: one task owns it (on the C6 the radio task, which
 * also runs the sync between tags), and the host tests build it. */

#define ESL_RCACHE_WAIT_S        5      /* next_poll_s while the radio waits for the host */
#define ESL_RCACHE_WAIT_TRIES    3      /* ... at most this often in a row per tag, then its own interval */
#define ESL_RCACHE_UNKNOWN       8      /* unknown tags whose waits it counts */
#define ESL_RCACHE_DEFAULT_POLL_S 60    /* next_poll_s for a tag it has no state for, until the host says */
#define ESL_RCACHE_QUIET_MS      60     /* no tag frame for this long: no tag is mid-exchange */
#define ESL_RCACHE_BLOCKS_MS     300    /* ... after a block that was not the image's last */
#define ESL_RCACHE_SYNC_MS       10000  /* sync this often when there is nothing to ask for */
#define ESL_RCACHE_SYNC_MAX_MS   30000  /* and at the latest this late, quiet or not */
#define ESL_RCACHE_SOON_MS       1000   /* ... or this soon when the radio lacks something */

/* What the host says about one tag. */
typedef struct {
    uint64_t mac;
    uint16_t next_poll_s;    /* sent in every reply, as esl_store_tag_poll_s() */
    bool has_cfg;            /* cfg is the tag's settings (effective, as in CONFIG) */
    esl_tag_cfg_t cfg;
    uint32_t image_id;       /* the image it should show, 0 = none */
    uint16_t fw_ver;         /* the firmware version to offer it, 0 = none */
    uint32_t image_failed;   /* the image it refused (ESL_POLL_F_DRAWFAIL), as the host noted it: not offered */
    uint16_t fw_failed;      /* the version it rolled back (ESL_POLL_F_FWFAIL), likewise */
} esl_rtag_state_t;

/* One tag in the cache: the host's state and what the radio saw itself. */
typedef struct {
    esl_rtag_state_t s;
    uint32_t image_taken;    /* it asked for a block of this image */
    uint32_t image_failed;   /* it reported ESL_POLL_F_DRAWFAIL after taking it: not offered again */
    uint16_t fw_taken;
    uint16_t fw_failed;      /* ESL_POLL_F_FWFAIL likewise */
    uint8_t waits;           /* ESL_RCACHE_WAIT_S replies in a row */
} esl_rtag_t;

/* An image, as the host drew it (esl_image): id, the bytes as sent (coded or raw). */
typedef struct {
    uint32_t id;             /* 0 = free slot */
    uint16_t size;
    uint8_t model;
    uint8_t fmt;             /* ESL_IMG_FMT_* */
    uint32_t used;           /* esl_rcache_t.clock at the last use, for replacing */
    uint8_t *data;           /* image_cap bytes */
} esl_rimage_t;

/* The tag firmware for one model. The bytes stay where the platform keeps them
 * (flash on the C6) and are read through ops.read_fw. */
typedef struct {
    uint16_t ver;            /* 0 = none */
    uint16_t size;
    uint32_t crc;
} esl_rfw_t;

/* One entry of the log the host fetches: a frame the radio got, with when and how
 * strong. t_ms is the radio's own clock (the low 32 bits, 49 days): the host turns it
 * into its own time with the radio's clock at the sync (esl_rhost). */
typedef struct {
    uint32_t t_ms;
    int8_t rssi;
    uint8_t len;             /* PSDU length including the FCS */
    uint8_t psdu[ESL_PSDU_MAX];
} esl_rlog_t;

typedef struct {
    void *ctx;
    /* n bytes at off of the firmware for model into buf; false when it cannot be read. */
    bool (*read_fw)(void *ctx, uint8_t model, size_t off, uint8_t *buf, size_t n);
} esl_rcache_ops_t;

typedef struct {
    uint32_t polls, blocks;  /* POLLs and IMG_REQ/FW_REQ answered */
    uint32_t waits;          /* replies of ESL_RCACHE_WAIT_S: something was missing */
    uint32_t log_dropped;    /* frames the log had no room for */
} esl_rcache_stats_t;

#define ESL_RCACHE_MODELS 32

typedef struct {
    esl_rtag_t *tags;
    unsigned max_tags, n_tags;
    esl_rimage_t *images;
    unsigned n_images;
    size_t image_cap;
    esl_rfw_t fw[ESL_RCACHE_MODELS];      /* by model - 1 */
    esl_rlog_t *log;
    unsigned log_cap, log_head, log_len;
    esl_rcache_ops_t ops;
    uint16_t pan;
    uint16_t default_poll_s; /* the host's default interval (esl_store_get_next_poll), for unknown tags */
    uint32_t clock;          /* counts uses, for replacing images */

    /* unknown tags (no state from the host): their waits, so they too go back to their own interval */
    struct { uint64_t mac; uint8_t waits; } unknown[ESL_RCACHE_UNKNOWN];
    unsigned unknown_next;

    /* for esl_rcache_sync_due() */
    uint64_t last_rx_ms;     /* the last tag frame */
    bool mid_blocks;         /* the last block to mid_mac was not the end of its image or firmware */
    uint64_t mid_mac;
    uint32_t mid_image;      /* ... of this image (not replaced while it is fetched), 0 = firmware */
    bool want;               /* the radio lacks something the host has to send */
    uint64_t want_mac;       /* the last tag it lacked state, an image or firmware for */
    uint32_t want_image;
    uint8_t want_fw_model;
    esl_rcache_stats_t stats;
} esl_rcache_t;

/* Memory comes from the caller: tags (max_tags), images (n_images, each with data of
 * image_cap bytes: the largest image it is to hold) and the log (log_cap entries).
 * pan is the network's PAN ID; frames to another PAN or address are not answered. */
void esl_rcache_init(esl_rcache_t *rc, uint16_t pan, esl_rtag_t *tags, unsigned max_tags, esl_rimage_t *images,
                     unsigned n_images, size_t image_cap, esl_rlog_t *log, unsigned log_cap,
                     const esl_rcache_ops_t *ops);

/* ---- from the host ---- */

/* Sets or replaces the state of a tag. False when the table is full. A state that
 * differs from the last lets the tag wait (ESL_RCACHE_WAIT_S) again. */
bool esl_rcache_set_tag(esl_rcache_t *rc, const esl_rtag_state_t *s);
/* The interval for tags it has no state for (the host's default; ESL_RCACHE_DEFAULT_POLL_S
 * until set). */
void esl_rcache_set_default_poll(esl_rcache_t *rc, uint16_t poll_s);
/* Forgets a tag (deleted on the host). */
void esl_rcache_drop_tag(esl_rcache_t *rc, uint64_t mac);
/* Stores an image (size bytes, at most image_cap), replacing the one used the longest
 * ago that no tag's state names, else the oldest, but never the one a tag is fetching
 * (unless it is the only slot). False when it does not fit. */
bool esl_rcache_put_image(esl_rcache_t *rc, uint32_t id, uint8_t model, uint8_t fmt, const uint8_t *data,
                          size_t size);
bool esl_rcache_has_image(const esl_rcache_t *rc, uint32_t id);
/* The firmware for model is now ver (size bytes, CRC-32 crc), readable through
 * ops.read_fw; ver 0 = none. */
void esl_rcache_set_fw(esl_rcache_t *rc, uint8_t model, uint16_t ver, uint16_t size, uint32_t crc);

/* ---- the radio ---- */

/* Handles one received PSDU (psdu_len includes the FCS) as esl_radio_handle() does,
 * from the cache: if it is a POLL, IMG_REQ or FW_REQ to this AP, writes the reply
 * frame into tx (ESL_PSDU_MAX bytes) and returns its PSDU length including the FCS,
 * else 0. Every frame to this AP goes into the log, answered or not. */
size_t esl_rcache_handle(esl_rcache_t *rc, const uint8_t *psdu, size_t psdu_len, int8_t rssi, uint64_t now_ms,
                         uint8_t *tx);

/* Whether the radio should sync with the host now: when no tag is mid-exchange, and
 * it lacks something (after ESL_RCACHE_SOON_MS) or it last synced ESL_RCACHE_SYNC_MS
 * ago; at ESL_RCACHE_SYNC_MAX_MS it is due even while tags are busy, so the host is
 * never far behind. last_sync_ms is when the last sync ended (one later than now_ms
 * makes it due at once). */
bool esl_rcache_sync_due(const esl_rcache_t *rc, uint64_t now_ms, uint64_t last_sync_ms);

/* ---- the log ---- */

unsigned esl_rcache_log_count(const esl_rcache_t *rc);
/* The i-th oldest entry, NULL when there is none. */
const esl_rlog_t *esl_rcache_log_at(const esl_rcache_t *rc, unsigned i);
/* The host has the n oldest entries. */
void esl_rcache_log_drop(esl_rcache_t *rc, unsigned n);
/* What the radio lacks (want, want_*: the last tag, image and firmware it missed; the
 * host also sends the state of every tag in the log it fetches): cleared once a sync
 * has asked for it. */
void esl_rcache_want_clear(esl_rcache_t *rc);
