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
    void (*persist_save)(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c);
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
} esl_store_ops_t;

typedef struct {
    bool used;
    uint64_t mac;

    bool has_content;
    esl_content_t content;
    uint32_t content_id;      /* 0 when no content is set */
    uint8_t content_err;      /* ESL_CERR_*: why named content is not sent (from the last POLL) */

    esl_tag_cfg_t cfg;        /* settings for this tag; 0 fields = default */

    bool seen;                /* has polled since boot */
    uint64_t last_seen_ms;
    uint32_t shown_content_id;
    uint16_t battery_mv;
    uint16_t fw;
    uint8_t model;            /* ESL_MODEL_*, from the last POLL */
    int8_t rssi;
    uint8_t poll_flags;
    bool cfg_capable;         /* the last POLL carried cfg_ver (tag understands CONFIG) */
    uint16_t shown_cfg_ver;   /* cfg_ver from the last POLL */
    uint32_t polls;
    bool layout_capable;      /* its POLL has the layout version (18 bytes) */
    uint32_t layout_ver;      /* layout file version from the last POLL */
    uint32_t layout_offered;  /* version the AP last offered it */
    uint32_t layout_failed;   /* version it rejected (ESL_POLL_F_LAYOUTFAIL), not offered again */
    bool layout_taken;        /* the tag started on layout_offered (esl_store_offer_taken), so
                                 ESL_POLL_F_LAYOUTFAIL is about it */
    uint16_t fw_offered;      /* tag firmware version the AP last offered it (RAM only) */
    bool fw_taken;            /* the tag started on fw_offered, as layout_taken (RAM only) */
    uint16_t fw_failed;       /* a version it rejected or rolled back (ESL_POLL_F_FWFAIL), not offered again */
    uint16_t fw_got_ver;      /* the image it is fetching (RAM only), 0 = none */
    uint16_t fw_got;          /* bytes of it up to the end of the last FW block it got */
    /* Images (ESL_POLL_F_IMAGE, esl_image): the tag shows what the AP draws. image_id is
       the id of its content's image once drawn (0 = not drawn since the content was set);
       shown_content_id is then the id it shows. The offer bookkeeping as for firmware. */
    bool image_capable;       /* its last POLL had ESL_POLL_F_IMAGE */
    uint32_t image_id;
    uint32_t image_offered;   /* id the AP last offered it (RAM only) */
    bool image_taken;         /* it asked for a block of image_offered */
    uint32_t image_failed;    /* an id it refused or could not show (ESL_POLL_F_DRAWFAIL), not offered again */
    uint32_t due_s;           /* scheduled content for it is due at this second since boot (esl_sched), 0 = none */
    uint32_t seen_unix;       /* wall-clock second of its last POLL (also from NVS after a restart), 0 = unknown */
    bool seen_dirty;          /* polled since its record was last saved (esl_store_save_seen) */
} esl_tag_t;

/* What a tag polls with when the AP sends 0 (tag/src/config.h CFG_POLL_S) */
#define ESL_TAG_BUILTIN_POLL_S 60
/* A tag with scheduled content due before its next poll is told to come back
 * this long after it is due, so the schedule has set it by then. */
#define ESL_DUE_MARGIN_S 2

/* Why content with a named layout is not sent to a tag */
#define ESL_CERR_NONE         0
#define ESL_CERR_NO_LAYOUTS   1 /* the tag's firmware cannot download layout files */
#define ESL_CERR_NOT_IN_FILE  2 /* the layout file for its model has no layout by that name */
#define ESL_CERR_REJECTED     3 /* the tag rejected the current layout file */
/* (While the tag is still downloading the file, the AP answers with LAYOUT
 * blocks, not IDLE, so that needs no error.) */
/* Text for the API, NULL for ESL_CERR_NONE */
const char *esl_content_err_text(uint8_t err);

typedef struct {
    esl_tag_t *tags;
    unsigned cap;
    uint16_t next_poll_s;     /* default interval for tags without their own poll_s (0 = tag keeps its own) */
    uint16_t next_poll_builtin; /* next_poll_s from esl_store_init (menuconfig on the device) */
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
 * longest-unseen tag without content is replaced. */
esl_store_err_t esl_store_set(esl_store_t *st, uint64_t mac, const esl_content_t *c, esl_set_result_t *res);

/* Handles a POLL: records status and writes IDLE, CONTENT or CONFIG into reply
 * (ESL_MAX_PAYLOAD bytes). Unknown tags are added without content if there is
 * room. Returns the reply length.
 *
 * env describes the layout file for the tag's model (esl_layouts_env; NULL =
 * none). Returns 0 and writes nothing when the tag must get the layout file
 * first (its POLL reports another version, and it has not rejected this one):
 * the caller then answers with the first LAYOUT block (esl_layouts_block).
 * Priority: LAYOUT, then CONTENT, then CONFIG, then IDLE (one per POLL). */
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

bool esl_store_delete(esl_store_t *st, uint64_t mac);
bool esl_store_get(esl_store_t *st, uint64_t mac, esl_tag_t *out);
/* Copies the tag in a slot; false if the slot is unused. */
bool esl_store_get_slot(esl_store_t *st, unsigned slot, esl_tag_t *out);
unsigned esl_store_count(esl_store_t *st);
/* Slots a new MAC could take (free, or held by a tag without content or settings). */
unsigned esl_store_available(esl_store_t *st);
/* Sets the default interval and persists it (persist_next_poll). */
void esl_store_set_next_poll(esl_store_t *st, uint16_t next_poll_s);
/* Back to the built-in default from esl_store_init(); erases the stored value. */
void esl_store_reset_next_poll(esl_store_t *st);
/* Loads a persisted default interval at boot. Does not call persist_next_poll. */
void esl_store_restore_next_poll(esl_store_t *st, uint16_t next_poll_s);
uint16_t esl_store_get_next_poll(esl_store_t *st);
/* The interval a tag is told (next_poll_s): its own poll_s, else the default.
 * The default for a MAC not in the table. Used for LAYOUT blocks. */
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
/* The tag asked for a block of image id: it started on it (as esl_store_offer_taken). */
void esl_store_image_taken(esl_store_t *st, uint64_t mac, uint32_t id);
/* The image of the tag's content content_id was drawn and has id (esl_image). Kept
 * only while the tag still has that content. */
void esl_store_image_known(esl_store_t *st, uint64_t mac, uint32_t content_id, uint32_t id);

/* A FW block of image ver up to byte end went to the tag (esl_radio). */
void esl_store_fw_sent(esl_store_t *st, uint64_t mac, uint16_t ver, uint16_t end);

/* The tag asked for a block of firmware image ver (fw) or layout file ver, or got the
 * whole layout file in the offer (esl_radio): it started on that version. A tag keeps
 * its fail flag on until then, so only after it does the flag count against the
 * version offered; before, the flag is about an older one and the offer may just have
 * been lost (both tries of the POLL it answered). */
void esl_store_offer_taken(esl_store_t *st, uint64_t mac, bool fw, uint32_t ver);

typedef struct {
    unsigned updated;  /* tags of the model that run the version */
    unsigned pending;  /* tags that take firmware over the air and run another one */
    unsigned failed;   /* tags that rejected it, or went back to their old firmware */
    unsigned no_ota;   /* tags of the model without OTA (old firmware or no SPI flash) */
} esl_fw_counts_t;
/* The state of the update to version ver for the seen tags of a model. */
void esl_store_fw_counts(esl_store_t *st, uint8_t model, uint16_t ver, esl_fw_counts_t *out);

/* Content is set and the tag has not reported showing it yet (for a tag that shows
 * images: the image of the content, once drawn). */
bool esl_tag_pending(const esl_tag_t *t);
/* The tag has content or its own settings (so it is kept when the table is full). */
bool esl_tag_holds_data(const esl_tag_t *t);
/* The settings the AP sends this tag: its own, with poll_s from the default
 * when the tag has none. next_poll_s in IDLE/CONTENT is out->poll_s. */
void esl_tag_effective_cfg(const esl_tag_t *t, uint16_t default_poll_s, esl_tag_cfg_t *out);
/* The tag understands CONFIG and has not reported the current settings yet. */
bool esl_tag_cfg_pending(const esl_tag_t *t, uint16_t default_poll_s);
