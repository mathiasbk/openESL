#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"
#include "esl_layout.h"
#include "esl_persist.h"
#include "esl_proto.h"

/* Table of tags: MAC -> content + settings + last reported status.
 *
 * Content, per-tag settings (esl_tag_cfg_t) and the AP-wide default interval
 * are persisted through the persist hooks (NVS on the device) when they
 * change; status lives in RAM only. Answering a POLL builds one short
 * message from the table (krav A3: microseconds, well inside the 5 ms).
 *
 * HTTP and radio run in different tasks, so every public function takes the
 * lock. The persist hooks are called after the lock is released. */

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    /* False when it could not be saved (NVS full): esl_store_set then keeps the old
     * content and returns ESL_STORE_NOT_SAVED. */
    bool (*persist_save)(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c);
    void (*persist_erase)(void *ctx, unsigned slot);
    /* Per-tag settings; cfg NULL = back to defaults, erase the stored entry. */
    void (*persist_cfg)(void *ctx, unsigned slot, uint64_t mac, const esl_tag_cfg_t *cfg);
    /* The default interval set at run time; NULL = back to the built-in one
     * from esl_store_init(), erase the stored value. */
    void (*persist_next_poll)(void *ctx, const uint16_t *next_poll_s);
    /* What the AP last heard from a tag (esl_persist.h); s NULL = erase. Written
     * when a tag is new or its model or firmware changes, and for every tag heard
     * since the last save by esl_store_save_seen(). May be NULL. */
    void (*persist_seen)(void *ctx, unsigned slot, uint64_t mac, const esl_seen_t *s);
    /* The wall-clock time in seconds, 0 while unknown (no NTP yet). May be NULL. */
    uint32_t (*unix_now)(void *ctx);
    /* Reads back what persist_save wrote for slot; false if it is missing, bad or of
     * another MAC. With it the store keeps no copy of any content in RAM, without it
     * (NULL) it does. */
    bool (*load_content)(void *ctx, unsigned slot, uint64_t mac, esl_content_t *c);
    /* Zeroed memory for more slots (n bytes), NULL when there is no room for it: the
     * table then stops growing. Freed with free(). NULL = calloc. */
    void *(*alloc)(void *ctx, size_t n);
    /* Held around a change of content or settings and its save (not the table's lock,
     * so a POLL does not wait for NVS): two writers then save in the order they change
     * the table, and NVS ends up with what the table says. May be NULL. */
    void (*save_lock)(void *ctx);
    void (*save_unlock)(void *ctx);
    /* A picture for the tag in slot (PUT /api/tags/{mac}/image), in esl_image's stored
     * form; data NULL = erase. False when it could not be saved. Both NULL: the AP takes
     * no pictures. */
    bool (*persist_image)(void *ctx, unsigned slot, const uint8_t *data, size_t len);
    /* Reads it back into buf (cap bytes): its length, 0 when there is none or it does not fit. */
    size_t (*load_image)(void *ctx, unsigned slot, uint8_t *buf, size_t cap);
    /* The radio channel set at run time and a planned move (esl_store_move_channel):
     * channel 0 = the built-in one, next 0 = none; erase what is 0. May be NULL. */
    void (*persist_channel)(void *ctx, uint8_t channel, uint8_t next);
} esl_store_ops_t;

/* Signal history per tag (#261): the last ESL_LINK_HIST POLLs, in RAM only. 2 bytes
 * each, so a board without PSRAM keeps 8 (16 bytes a slot); the CMakeLists sets more
 * on boards with PSRAM. */
#ifndef ESL_LINK_HIST
#define ESL_LINK_HIST 8
#endif
#define ESL_HIST_LOST     0x7F /* hist_info: rounds the tag lost before this POLL (127 = 127 or more) */
#define ESL_HIST_RETRY    0x80 /* ... and its answer came only on the tag's second try */

/* Radios a store tells apart (esl_tag_t.radio): the one on the AP's channel and 3 more
 * (docs/ap-netradio.md, "Several radios"). */
#define ESL_STORE_RADIOS 4

/* One slot of the table. Kept small, since there is one per tag for the whole run
 * (sizeof is printed in the boot log): the content itself is not here but in NVS
 * (esl_store_get_content), and the flags are bits. */
typedef struct {
    uint64_t mac;
    /* what find() reads next to the MAC, in the same cache line (PSRAM on the C5) */
    bool used : 1;
    bool has_content : 1;
    bool seen : 1;            /* has polled since boot */
    bool cfg_capable : 1;     /* the last POLL carried cfg_ver (tag understands CONFIG) */
    bool channel_capable : 1; /* ... and the link block (0.9.0 and later): it follows a channel move */
    bool fw_taken : 1;        /* the tag started on fw_offered (esl_store_fw_taken), so
                                 ESL_POLL_F_FWFAIL is about it (RAM only) */
    bool image_capable : 1;   /* its last POLL had ESL_POLL_F_IMAGE */
    bool image_rle : 1;       /* ... and ESL_POLL_F_IMG_RLE: it takes coded images */
    bool image_2d : 1;        /* ... and ESL_POLL_F_IMG_2D: it takes 2D coded images */
    bool image_taken : 1;     /* it asked for a block of image_offered */
    bool seen_dirty : 1;      /* polled since its record was last saved (esl_store_save_seen) */
    bool has_image : 1;       /* its content is a picture (ESL_LAYOUT_IMAGE), kept with ops.persist_image */
    bool content_unfit : 1;   /* its content does not fit its model's layout file (esl_store_check_layout,
                                 RAM only): ESL_CERR_UNFIT */
    bool content_check : 1;   /* its content or model changed since that was last checked (esl_store_check_tag) */
    uint8_t radio : 2;        /* several radios over the network: the one that serves it (esl_store_set_tag_radio,
                                 RAM only), 0 = the one on the AP's channel */
    uint8_t model;            /* ESL_MODEL_*, from the last POLL */
    int8_t rssi;
    uint8_t poll_flags;
    uint8_t content_err;      /* ESL_CERR_*: why the content is not shown (from the last POLL) */
    uint16_t battery_mv;
    uint64_t last_seen_ms;
    uint32_t content_id;      /* 0 when no content is set */
    uint32_t layout_hash;     /* esl_content_layout_hash() of the content, 0 = built-in layout */
    uint32_t shown_content_id;
    uint32_t polls;
    /* Signal (#261, RAM only): the AP's RSSI of each of the last POLLs and what the tag's
       link block said with it, oldest first from hist_at - hist_n; the totals since boot */
    int8_t hist_rssi[ESL_LINK_HIST];
    uint8_t hist_info[ESL_LINK_HIST];
    uint8_t hist_n;           /* entries in use */
    uint8_t hist_at;          /* where the next one goes */
    uint8_t rx_rssi;          /* the tag radio's raw RSSI and LQI of the AP's last answer (link block), 0 = none */
    uint8_t rx_lqi;
    uint16_t lost_total;      /* rounds the tag reported without an answer (saturates) */
    uint16_t retries;         /* POLLs answered only at their second try (saturates) */
    /* Images (ESL_POLL_F_IMAGE, esl_image): the tag shows what the AP draws. image_id is
       the id of its content's image once drawn (0 = not drawn since the content was set),
       image_layout_ver the version of the layout file it was drawn with;
       shown_content_id is then the id it shows. The offer bookkeeping as for firmware. */
    uint32_t image_id;
    uint32_t image_layout_ver;
    uint32_t image_offered;   /* id the AP last offered it (RAM only) */
    uint32_t image_failed;    /* an id it refused or could not show (ESL_POLL_F_DRAWFAIL), not offered again */
    uint32_t due_s;           /* scheduled content for it is due at this second since boot (esl_sched), 0 = none */
    uint32_t seen_unix;       /* wall-clock second of its last POLL (also from NVS after a restart), 0 = unknown */
    esl_tag_cfg_t cfg;        /* settings for this tag; 0 fields = default */
    uint16_t fw;
    uint16_t shown_cfg_ver;   /* cfg_ver from the last POLL */
    uint16_t fw_offered;      /* tag firmware version the AP last offered it (RAM only) */
    uint16_t fw_failed;       /* a version it rejected or rolled back (ESL_POLL_F_FWFAIL), not offered again */
    uint16_t fw_got_ver;      /* the image it is fetching (RAM only), 0 = none */
    uint16_t fw_got;          /* bytes of it up to the end of the last FW block it got */
} esl_tag_t;

/* Milliseconds since the tag was last heard. A POLL handled after the caller read
   now_ms leaves last_seen_ms ahead of it; that is 0, not an unsigned wrap. */
static inline uint64_t esl_tag_seen_ms_ago(const esl_tag_t *t, uint64_t now_ms)
{
    return now_ms > t->last_seen_ms ? now_ms - t->last_seen_ms : 0;
}

/* What a tag polls with when the AP sends 0 (tag/src/config.h CFG_POLL_S) */
#define ESL_TAG_BUILTIN_POLL_S 60
/* A tag with scheduled content due before its next poll is told to come back
 * this long after it is due, so the schedule has set it by then. */
#define ESL_DUE_MARGIN_S 2

/* Why a tag's content is not shown */
#define ESL_CERR_NONE         0
#define ESL_CERR_NO_IMAGE     1 /* the tag does not take images (old firmware, or no SPI flash) */
#define ESL_CERR_NOT_IN_FILE  2 /* the layout file for its model has no layout by that name */
#define ESL_CERR_UNFIT        3 /* a barcode, QR code or number of it does not fit its box in that layout */
/* Text for the API, NULL for ESL_CERR_NONE */
const char *esl_content_err_text(uint8_t err);

/* The table grows ESL_STORE_CHUNK slots at a time as tags turn up, up to the max
 * given to esl_store_init (at most ESL_STORE_MAX_TAGS) or until ops.alloc says
 * there is no more room. A slot keeps its number for the life of the table (the
 * NVS keys use it), and memory once taken is kept. */
#define ESL_STORE_CHUNK 32
#define ESL_STORE_MAX_CHUNKS 128
#define ESL_STORE_MAX_TAGS (ESL_STORE_CHUNK * ESL_STORE_MAX_CHUNKS)

typedef struct {
    esl_tag_t *chunk[ESL_STORE_MAX_CHUNKS];
    esl_content_t *content[ESL_STORE_MAX_CHUNKS]; /* RAM copies, only without ops.load_content */
    unsigned cap;             /* slots there is memory for (0..max) */
    unsigned max;
    bool full;                /* a new tag found no slot (none free, and the table could not grow) */
    uint16_t next_poll_s;     /* default interval for tags without their own poll_s (0 = tag keeps its own) */
    uint16_t next_poll_builtin; /* next_poll_s from esl_store_init (menuconfig on the device) */
    /* The radio channel (esl_store_move_channel below) */
    uint8_t channel;          /* the one the radio is on */
    uint8_t channel_builtin;  /* menuconfig's (ESL_DEFAULT_CHANNEL until esl_store_set_builtin_channel) */
    uint8_t channel_next;     /* a planned move announced to the tags, 0 = none */
    uint64_t channel_due_ms;  /* ... which happens at this time at the latest */
    bool channel_hold;        /* the move was restored at boot: tags not heard since count as waiting */
    bool channel_radio_old;   /* the radio does not pass a move on to the tags (esl_store_set_radio_follows) */
    bool channel_force;       /* the planned move goes even with tags that cannot follow */
    uint16_t channel_stopped; /* tags with old firmware that stopped the last move (1 for an old radio), 0 = none */
    uint8_t radio_channel[ESL_STORE_RADIOS]; /* the channel of each extra radio, [0] unused (esl_store_set_radio_channel) */
    esl_store_ops_t ops;
} esl_store_t;

typedef enum {
    ESL_STORE_OK = 0,
    ESL_STORE_FULL,
    ESL_STORE_NOT_SAVED,  /* persist_save failed (NVS full); the tag keeps its old content */
} esl_store_err_t;

typedef struct {
    uint32_t content_id;
    bool changed;
    bool pending;
} esl_set_result_t;

/* An empty table for up to max tags (clamped to ESL_STORE_MAX_TAGS). ops may be NULL
 * (no locking, no persistence, content kept in RAM). Takes no memory yet. st must not
 * hold memory (new, or after esl_store_free): it is not freed here. */
void esl_store_init(esl_store_t *st, unsigned max, const esl_store_ops_t *ops, uint16_t next_poll_s);
/* Frees the table's memory and empties it; st can be initialised again. Safe on a
 * zeroed esl_store_t. */
void esl_store_free(esl_store_t *st);
/* Slot numbers in use are below this (the slots there is memory for). */
unsigned esl_store_slots(esl_store_t *st);
/* The slot of the tag, -1 when it is not in the table. */
int esl_store_slot_of(esl_store_t *st, uint64_t mac);
/* A new tag found no room since a tag last got a slot: none free, and no memory or
 * max reached. Such a tag is not in the table until one frees. */
bool esl_store_full(esl_store_t *st);

/* Loads persisted content at boot. Does not call persist_save. */
bool esl_store_restore(esl_store_t *st, unsigned slot, uint64_t mac, const esl_content_t *c);

/* Loads what the AP last heard from a tag at boot (esl_persist.h): the tag is
 * back in the table, not seen since this boot, with its model, firmware,
 * battery, signal and the time it was last heard. Does not call persist_seen. */
bool esl_store_restore_seen(esl_store_t *st, unsigned slot, uint64_t mac, const esl_seen_t *s);

/* Saves the record of every tag heard since its last save (persist_seen). For
 * a planned restart (OTA) and now and then; returns how many were written. */
unsigned esl_store_save_seen(esl_store_t *st);

/* Loads persisted settings at boot, into the slot's tag if esl_store_restore
 * already put the same MAC there. Does not call persist_cfg. */
bool esl_store_restore_cfg(esl_store_t *st, unsigned slot, uint64_t mac, const esl_tag_cfg_t *cfg);

/* Sets content for a tag, creating it if needed. If the table is full, the
 * longest-unseen tag without content is replaced. ESL_STORE_NOT_SAVED: it could
 * not be persisted, and the tag keeps what it had. */
esl_store_err_t esl_store_set(esl_store_t *st, uint64_t mac, const esl_content_t *c, esl_set_result_t *res);

/* Handles a POLL: records status and writes CONFIG or IDLE into reply
 * (ESL_MAX_PAYLOAD bytes). Unknown tags are added without content if there is
 * room. Returns the reply length.
 *
 * env describes the layout file for the tag's model (esl_layouts_env; NULL =
 * none); content naming a layout it does not have gets ESL_CERR_NOT_IN_FILE, content
 * marked by esl_store_check_layout ESL_CERR_UNFIT.
 * After IDLE the caller offers the content's image (esl_image), then firmware. */
size_t esl_store_on_poll(esl_store_t *st, uint64_t mac, const esl_poll_t *poll, int8_t rssi,
                         uint64_t now_ms, const esl_layout_env_t *env, uint8_t *reply);

typedef struct {
    bool changed;
    bool pending; /* the tag understands CONFIG and has not confirmed these settings yet */
} esl_cfg_result_t;

/* Scheduled content (esl_sched): clear_due forgets every due time, note_due
 * records one for a tag in the table (the earliest one wins). A tag whose
 * content is due before its next poll gets a shorter next_poll_s in the reply
 * (due + ESL_DUE_MARGIN_S), so a long interval does not delay it. */
void esl_store_clear_due(esl_store_t *st);
void esl_store_note_due(esl_store_t *st, uint64_t mac, uint32_t due_s);

/* Replaces a tag's settings (all fields 0 = defaults), creating the tag if
 * needed, like esl_store_set. Values are not range-checked here. */
esl_store_err_t esl_store_set_cfg(esl_store_t *st, uint64_t mac, const esl_tag_cfg_t *cfg, esl_cfg_result_t *res);

/* Pictures (ESL_LAYOUT_IMAGE): the store saves them with ops.persist_image. */
bool esl_store_takes_images(const esl_store_t *st);
/* Sets a picture as the tag's content, like esl_store_set: data (len bytes) is the
 * stored form, c the content naming it (esl_content_init_image). The picture is saved
 * first, then the content; a picture the tag already has is not saved again. When the
 * content cannot be saved after the picture was, the tag keeps its old content, which
 * no longer matches the picture if it was one too: it is not drawn until the next one. */
esl_store_err_t esl_store_set_image(esl_store_t *st, uint64_t mac, const uint8_t *data, size_t len,
                                    const esl_content_t *c, esl_set_result_t *res);
/* The stored picture of the tag into buf (cap bytes, outside the lock): its length, 0
 * when it has none. Content that is not a picture leaves no picture behind (erased). */
size_t esl_store_get_image(esl_store_t *st, uint64_t mac, uint8_t *buf, size_t cap);

/* After a layout file for model was stored (PUT /api/layouts) or loaded at boot: checks
 * the content of every tag drawn with that model's file (the model's own tags, and those of
 * models whose same_as names it: esl_model_layout_model) that names one of the file's layouts against
 * it (esl_layouts_check_content), as PUT /api/tags checks new content. A tag whose
 * content does not fit gets ESL_CERR_UNFIT and its content is held back (it keeps what
 * it shows) until it gets new content or another file fits it; the mark is cleared on
 * the rest. fn (may be NULL) gets each tag that does not fit, with the reason. Returns
 * how many do not fit. Takes the store's and the layouts' locks in turn, never both. */
unsigned esl_store_check_layout(esl_store_t *st, esl_layouts_t *ls, uint8_t model,
                                void (*fn)(void *ctx, uint64_t mac, const char *msg), void *ctx);

/* The same for one tag, when its content or model changed since its last check: content
 * from the schedule (esl_sched_tick, checked when it was queued, maybe against an older
 * file), or set while its model was not known. esl_radio_handle calls it after each POLL
 * (esl_radio_after once the reply is out, with esl_radio_set_draw_later),
 * and esl_image before it draws (with a radio coprocessor, esl_rhost draws before the POLL). True if the content fits (or nothing was to be checked). */
bool esl_store_check_tag(esl_store_t *st, esl_layouts_t *ls, uint64_t mac);

bool esl_store_delete(esl_store_t *st, uint64_t mac);
bool esl_store_get(esl_store_t *st, uint64_t mac, esl_tag_t *out);
/* The tag's content: its RAM copy, or read back with ops.load_content (outside the
 * lock). False when the tag has none, or it could not be read or changed meanwhile. */
bool esl_store_get_content(esl_store_t *st, uint64_t mac, esl_content_t *out);
/* Copies the tag in a slot; false if the slot is unused. */
bool esl_store_get_slot(esl_store_t *st, unsigned slot, esl_tag_t *out);
unsigned esl_store_count(esl_store_t *st);
/* Slots a new MAC could take (free, not in memory yet, or held by a tag without
 * content or settings). */
unsigned esl_store_available(esl_store_t *st);
/* Sets the default interval and persists it (persist_next_poll). */
void esl_store_set_next_poll(esl_store_t *st, uint16_t next_poll_s);
/* Back to the built-in default from esl_store_init(); erases the stored value. */
void esl_store_reset_next_poll(esl_store_t *st);
/* Loads a persisted default interval at boot. Does not call persist_next_poll. */
void esl_store_restore_next_poll(esl_store_t *st, uint16_t next_poll_s);
uint16_t esl_store_get_next_poll(esl_store_t *st);
/* ---- The radio channel (#256, docs/protokoll.md "Channel") ----
 * The AP announces the channel it is on, or the one it is moving to, in every tag's
 * CONFIG (esl_tag_cfg_t.channel, part of cfg_ver). A tag keeps polling where it hears
 * the AP and tries the announced channel first when the AP stops answering there. So a
 * move goes: esl_store_move_channel() announces it; each tag gets CONFIG at its next POLL
 * and confirms it at the one after; esl_store_channel_step() tells the radio to move once
 * every tag heard lately has confirmed, or at the deadline; the tags then miss one POLL,
 * try the new channel and are back. A tag that missed it all keeps its channel. */
/* The channel the AP starts on when none is stored (menuconfig), before any restore. */
void esl_store_set_builtin_channel(esl_store_t *st, uint8_t channel);
/* Loads what persist_channel kept at boot: the radio starts on channel (0 = the built-in
 * one), and a move to next (0 = none) is announced again with a new deadline, since tags
 * may not have it yet. Does not call persist_channel. */
void esl_store_restore_channel(esl_store_t *st, uint8_t channel, uint8_t next, uint64_t now_ms);
/* Moves the AP to channel (ESL_CHANNEL_MIN..MAX; 0 = the built-in one). The radio moves
 * when esl_store_channel_step() says, right away with now; the channel the radio is on
 * cancels a planned move. persist_channel(ctx, radio's channel or 0 for the built-in one,
 * planned move or 0) is called here and when the radio moves. */
void esl_store_move_channel(esl_store_t *st, uint8_t channel, bool now, bool force, uint64_t now_ms);
/* Called by the radio's task now and then (every second): the channel to move the radio
 * to now, 0 = stay. A move off the built-in channel without force is given up (stopped)
 * when a tag heard lately cannot follow it, or the radio cannot pass it on. */
uint8_t esl_store_channel_step(esl_store_t *st, uint64_t now_ms);
typedef struct {
    uint8_t channel;    /* the radio is on it */
    uint8_t builtin;
    uint8_t next;       /* moving there, 0 = no move planned */
    unsigned waiting;   /* tags heard lately that have not confirmed the move */
    uint32_t due_s;     /* the move happens in at most this many seconds */
    unsigned old_tags;  /* tags heard lately whose firmware cannot follow a move (before 0.9.0) */
    unsigned unheard;   /* tags heard before boot, not since, that may still poll: firmware not known now */
    uint32_t unheard_s; /* ... the most seconds until they no longer count */
    bool radio_old;     /* the radio cannot pass a move on to the tags (esl_store_set_radio_follows) */
    unsigned stopped;   /* the last move was given up when tags with old firmware showed up (how many) */
    uint32_t taken;     /* bit c: another radio of this AP is on channel c (esl_store_set_taken_channels) */
} esl_channel_state_t;
void esl_store_channel_state(esl_store_t *st, uint64_t now_ms, esl_channel_state_t *out);
/* For a radio over the network (docs/ap-netradio.md): false when the one connected
 * retunes but does not tell the tags (a C6 from before 0.9.0, HELLO without
 * ESL_NLINK_F_CHANNEL). The radio on the AP itself or over UART always does. */
void esl_store_set_radio_follows(esl_store_t *st, bool follows);
/* ---- Several radios over the network (docs/ap-netradio.md, "Several radios") ----
 * Radio 0 is on the AP's channel and follows its moves. Radios 1..ESL_STORE_RADIOS-1
 * each have a channel of their own, and the tags they serve are told that channel in
 * CONFIG (cfg_ver, config_pending and a channel move count it). The AP's channel cannot
 * move to one of theirs (409 in PUT /api/config). */
/* Radio (1..ESL_STORE_RADIOS-1) is on channel; 0 = it is gone, and its tags count as
 * radio 0's again. */
void esl_store_set_radio_channel(esl_store_t *st, uint8_t radio, uint8_t channel);
/* The tag was last heard through radio (0..ESL_STORE_RADIOS-1). */
void esl_store_set_tag_radio(esl_store_t *st, uint64_t mac, uint8_t radio);
/* The defaults (esl_store_tag_defaults) as they apply to t: the channel of its radio. */
esl_tag_cfg_t esl_store_tag_ap(esl_store_t *st, const esl_tag_t *t, const esl_tag_cfg_t *ap);
/* A tag counts in a move while it was heard within this many of its intervals (and a
 * minute): one that has gone quiet does not hold it up. */
#define ESL_CHANNEL_ACTIVE_POLLS 3
/* persist_channel's next for a forced move (esl_store_move_channel force): the channel | this */
#define ESL_CHANNEL_FORCED 0x80
/* The move waits at most two of the longest interval of a tag (and a minute) for the
 * tags to confirm, and never longer than this. */
#define ESL_CHANNEL_MAX_WAIT_S (2 * ESL_CFG_POLL_MAX_S + 60)

/* What the AP sends every tag unless it has its own (esl_tag_effective_cfg): the default
 * interval and the announced channel. */
void esl_store_tag_defaults(esl_store_t *st, esl_tag_cfg_t *out);

/* The interval a tag is told (next_poll_s): its own poll_s, else the default.
 * The default for a MAC not in the table. Used for FW and IMG blocks. */
uint16_t esl_store_tag_poll_s(esl_store_t *st, uint64_t mac);

/* Tag firmware (esl_tagfw): called after esl_store_on_poll answered IDLE.
 * True when the tag should get the first FW block of image version ver (0 =
 * no image for its model) instead: it takes firmware over the air
 * (ESL_POLL_F_OTA), runs another version, and has not rejected this one. A
 * POLL with ESL_POLL_F_FWFAIL marks the version last offered as rejected. */
bool esl_store_fw_offer(esl_store_t *st, uint64_t mac, const esl_poll_t *poll, uint16_t ver);

/* Images (esl_image): called after esl_store_on_poll answered IDLE to a POLL with
 * ESL_POLL_F_IMAGE, with the id of the image the tag's content gives (0 = none).
 * True when the tag should get the first IMG block: it reports another id and has
 * not refused this one. A POLL with ESL_POLL_F_DRAWFAIL after the tag started on
 * the id last offered marks that id as refused; new content clears that. */
bool esl_store_image_offer(esl_store_t *st, uint64_t mac, const esl_poll_t *poll, uint32_t id);
/* The tag asked for a block of image id: it started on it (as esl_store_fw_taken). */
void esl_store_image_taken(esl_store_t *st, uint64_t mac, uint32_t id);
/* The image of the tag's content content_id was drawn with version layout_ver of the
 * model's layout file and has id (esl_image). Kept only while the tag still has that
 * content. */
void esl_store_image_known(esl_store_t *st, uint64_t mac, uint32_t content_id, uint32_t layout_ver, uint32_t id);

/* A FW block of image ver up to byte end went to the tag (esl_radio). offer: the block
 * offered with a POLL reply, which counts only when nothing of ver went to the tag yet. */
void esl_store_fw_sent(esl_store_t *st, uint64_t mac, uint16_t ver, uint16_t end, bool offer);

/* The tag asked for a block of firmware image ver (esl_radio): it started on that
 * version. A tag keeps its fail flag on until then, so only after it does the flag
 * count against the version offered; before, the flag is about an older one and the
 * offer may just have been lost (both tries of the POLL it answered). */
void esl_store_fw_taken(esl_store_t *st, uint64_t mac, uint16_t ver);

typedef struct {
    unsigned updated;  /* tags of the model that run the version */
    unsigned pending;  /* tags that take firmware over the air and run another one */
    unsigned failed;   /* tags that rejected it, or went back to their old firmware */
    unsigned no_ota;   /* tags of the model without OTA (old firmware or no SPI flash) */
} esl_fw_counts_t;
/* The state of the update to version ver for the seen tags of a model. */
void esl_store_fw_counts(esl_store_t *st, uint8_t model, uint16_t ver, esl_fw_counts_t *out);

/* Content is set and the tag has not reported showing its image yet (or it has not
 * been drawn). */
bool esl_tag_pending(const esl_tag_t *t);
/* Why its content is held back (ESL_CERR_*, for the API and health), NONE when it is not:
 * also while the tag still shows an image of it drawn before (a layout file it no longer fits,
 * or that lacks its layout). content_err is worked out again at each POLL and upload. */
uint8_t esl_tag_content_err(const esl_tag_t *t);
/* Its signal history, oldest first, into rssi and info (ESL_LINK_HIST each): how many. */
unsigned esl_tag_signal(const esl_tag_t *t, int8_t *rssi, uint8_t *info);
/* The tag has content or its own settings (so it is kept when the table is full). */
bool esl_tag_holds_data(const esl_tag_t *t);
/* The settings the AP sends this tag: its own, with poll_s from the default
 * when the tag has none, and the channel from ap (esl_store_tag_defaults). next_poll_s
 * in IDLE/CONTENT is out->poll_s. */
void esl_tag_effective_cfg(const esl_tag_t *t, const esl_tag_cfg_t *ap, esl_tag_cfg_t *out);
/* The tag understands CONFIG and has not reported the current settings yet. */
bool esl_tag_cfg_pending(const esl_tag_t *t, const esl_tag_cfg_t *ap);
