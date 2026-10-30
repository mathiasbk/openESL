#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_layout.h"
#include "esl_proto.h"
#include "esl_store.h"
#include "esl_tagfw.h"
#include "esl_image.h"

/* The AP's receive path, independent of the radio driver: the 802.15.4 glue
 * (main/radio_ieee802154.c) hands every received PSDU to esl_radio_handle()
 * and transmits what comes back. The simulated tags in main/radio_stub.c and
 * the host tests go through the same function, so only the driver itself and
 * the timing are left for the hardware. */

typedef struct {
    uint64_t mac;
    uint8_t msg_type;   /* ESL_MSG_POLL, ESL_MSG_FW_REQ or ESL_MSG_IMG_REQ */
    esl_poll_t poll;    /* POLL only */
    uint8_t reply_type; /* ESL_MSG_IDLE, ESL_MSG_CONFIG, ESL_MSG_FW or ESL_MSG_IMG */
} esl_radio_event_t;

/* Handles one received PSDU (psdu_len includes the FCS). If it is a POLL, FW_REQ
 * or IMG_REQ to this AP (PAN pan, short address ESL_AP_SHORT_ADDR), writes the
 * reply frame into tx (ESL_PSDU_MAX bytes) and returns its PSDU length
 * including the FCS. Returns 0 for anything else. ls (layout files, which the
 * AP draws with) and ev may be NULL.
 *   POLL       -> the store records the status and answers with CONFIG when the
 *                 tag's settings are out of date, else (a tag with ESL_POLL_F_IMAGE)
 *                 the first IMG block of its content's image when it shows another
 *                 (esl_image), else the first FW block when there is tag firmware
 *                 for it (esl_store_fw_offer), else IDLE (esl_store_on_poll)
 *   FW_REQ     -> the FW block the tag asks for (docs/tag-ota.md), IDLE when
 *                 the AP no longer has firmware for its model
 *   IMG_REQ    -> the IMG block the tag asks for (esl_image_block), IDLE when it
 *                 has no content
 * next_poll_s in every reply is the tag's own interval (esl_store_tag_poll_s). */
size_t esl_radio_handle(esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const uint8_t *psdu, size_t psdu_len,
                        int8_t rssi, uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev);

/* A frame a network radio answered itself (esl_rcache), at now_ms on the AP's clock:
 * the store learns from it what esl_radio_handle() would note (status, what the tag
 * shows, took and refused), but nothing is drawn: an image the tag shows or is offered
 * is the one the store knows for its content (esl_rhost has drawn it). */
void esl_radio_replay(esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const uint8_t *psdu, size_t psdu_len,
                      int8_t rssi, uint64_t now_ms);

/* The tag firmware images the radio path offers (one per AP; NULL = none,
 * the default). Set once at start-up, before the radio runs. */
void esl_radio_set_tagfw(esl_tagfw_t *fw);
/* The image buffers for tags that show what the AP draws (NULL = none, the
 * default: such tags get IDLE). Set once at start-up. */
void esl_radio_set_image(esl_image_t *im);
/* With on, esl_radio_handle() draws no image while the tag waits for its reply: a tag
 * whose image is not in a buffer is told to come back in ESL_IMAGE_BUSY_S
 * (esl_image_prepare_later, and esl_image_block_later for an IMG_REQ), and
 * esl_radio_after() draws it once the reply has gone out, together with the check of
 * new content against the layout file. Off (the default) draws at the POLL. Set once
 * at start-up. */
void esl_radio_set_draw_later(bool on);
/* After the reply to ev (from esl_radio_handle) has gone out: the work set aside by
 * esl_radio_set_draw_later(). Nothing when that is off or ev is not a POLL or IMG_REQ. */
void esl_radio_after(esl_store_t *st, esl_layouts_t *ls, const esl_radio_event_t *ev, uint64_t now_ms);
/* What was set (for a radio driver that answers the tags elsewhere: esl_rhost). */
esl_tagfw_t *esl_radio_tagfw(void);
esl_image_t *esl_radio_image(void);

/* ---- fake tag ------------------------------------------------------------
 * Behaves like the tag firmware on the air (tag/src/app.c): sends POLL with the
 * image id it shows and its cfg_ver, accepts only replies addressed to it with
 * its own sequence number, and applies CONFIG like tag/src/app.c (reporting
 * cfg_ver in the next POLL).
 * With ota it reports ESL_POLL_F_OTA and downloads firmware the AP offers
 * (FW / FW_REQ, keeping only a CRC); a whole image with the right CRC becomes
 * its version, as after the restart, unless fw_bad (it then "rolls back" and
 * reports ESL_POLL_F_FWFAIL, as the bootloader would).
 * With images (the default, as a tag with SPI flash) it reports ESL_POLL_F_IMAGE
 * ESL_POLL_F_IMG_RLE and ESL_POLL_F_IMG_2D and takes the image the AP offers (IMG / IMG_REQ,
 * keeping a CRC as the tag checks it); a whole image with the right id is what it
 * "shows" (the next POLL reports the id), unless img_bad (it then reports
 * ESL_POLL_F_DRAWFAIL and keeps what it showed). images = false is a tag without
 * SPI flash, which cannot show content. legacy sends the old 12-byte POLL and
 * drops CONFIG. */

#define ESL_FAKETAG_POLL_S 60 /* built-in interval, = CFG_POLL_S in tag/src/config.h */

typedef struct {
    uint64_t mac;
    uint16_t fw;
    uint16_t battery_mv;
    uint8_t model;
    uint8_t seq;
    uint8_t flags;        /* sent in the next POLL; starts as ESL_POLL_F_BOOTED */
    uint32_t shown;       /* image_id on the "screen" */
    uint16_t next_poll_s; /* last value from the AP (0 = keep) */
    uint32_t idles, configs, rejected;

    bool legacy;          /* firmware without CONFIG: 12-byte POLL */
    bool no_link;         /* firmware before 0.9.0: 14-byte POLL, no link block (no channel moves) */
    uint16_t cfg_ver;     /* from the last CONFIG, sent in POLL */
    esl_tag_cfg_t cfg;    /* settings from the last CONFIG */
    uint16_t interval_s;  /* the interval the tag would sleep now */

    /* firmware over the air */
    bool ota;                  /* has the SPI flash: reports ESL_POLL_F_OTA */
    bool fw_bad;               /* every image it installs "rolls back" */
    uint16_t fw_dl_ver, fw_dl_size, fw_dl_got;
    uint32_t fw_dl_crc, fw_dl_sum;
    uint16_t fw_failed;        /* reported with ESL_POLL_F_FWFAIL */
    uint32_t fw_blocks, fw_installs;

    /* images drawn by the AP */
    bool images;               /* reports ESL_POLL_F_IMAGE: content_id in POLL is the image shown */
    bool img_rle;              /* reports ESL_POLL_F_IMG_RLE: takes coded images too */
    bool img_2d;               /* reports ESL_POLL_F_IMG_2D: takes 2D coded images too */
    bool img_bad;              /* every image it gets "fails to draw" */
    uint32_t img_dl_id, img_dl_sum;
    uint16_t img_dl_size, img_dl_got;
    uint32_t img_failed;       /* reported with ESL_POLL_F_DRAWFAIL */
    uint32_t img_blocks, img_shown;
    uint8_t img_size_sent;     /* the last block's data length */
} esl_faketag_t;

#define ESL_FAKETAG_FW_REQS 128  /* FW_REQ per wake-up, = CFG_FW_MAX_REQ in tag/src/config.h */
#define ESL_FAKETAG_IMG_REQS 64  /* IMG_REQ per wake-up, = CFG_IMG_MAX_REQ in tag/src/config.h */

void esl_faketag_init(esl_faketag_t *t, uint64_t mac, uint16_t fw, uint16_t battery_mv, uint8_t model);
/* Builds the next POLL frame into psdu (ESL_PSDU_MAX bytes). Returns its length. */
size_t esl_faketag_poll(esl_faketag_t *t, uint16_t pan, uint8_t *psdu);
/* The FW_REQ for the next block of a firmware download, 0 if none. */
size_t esl_faketag_fw_request(esl_faketag_t *t, uint16_t pan, uint8_t *psdu);
/* The IMG_REQ for the next block of an image download, 0 if none. */
size_t esl_faketag_img_request(esl_faketag_t *t, uint16_t pan, uint8_t *psdu);
/* Handles a received frame. Returns ESL_MSG_IDLE, ESL_MSG_CONFIG, ESL_MSG_FW or
 * ESL_MSG_IMG if it was a valid reply for this tag, 0 if it was ignored. */
uint8_t esl_faketag_receive(esl_faketag_t *t, const uint8_t *psdu, size_t psdu_len);

/* One wake-up of the tag through esl_radio_handle(), as the firmware does it:
 * POLL, then after a FW block, FW_REQ until the image is complete (at most
 * ESL_FAKETAG_FW_REQS), or after an IMG block, IMG_REQ until the image is
 * complete (at most ESL_FAKETAG_IMG_REQS). Returns the type of the
 * last reply it accepted (0 = none). */
uint8_t esl_faketag_round(esl_faketag_t *t, esl_store_t *st, esl_layouts_t *ls, uint16_t pan, int8_t rssi,
                          uint64_t now_ms);
