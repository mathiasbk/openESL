#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_layout.h"
#include "esl_proto.h"
#include "esl_store.h"
#include "esl_tagfw.h"

/* The AP's receive path, independent of the radio driver: the 802.15.4 glue
 * (main/radio_ieee802154.c) hands every received PSDU to esl_radio_handle()
 * and transmits what comes back. The simulated tags in main/radio_stub.c and
 * the host tests go through the same function, so only the driver itself and
 * the timing are left for the hardware. */

typedef struct {
    uint64_t mac;
    uint8_t msg_type;   /* ESL_MSG_POLL, ESL_MSG_LAYOUT_REQ or ESL_MSG_FW_REQ */
    esl_poll_t poll;    /* POLL only */
    esl_layout_req_t req; /* LAYOUT_REQ only */
    uint8_t reply_type; /* ESL_MSG_IDLE, ESL_MSG_CONTENT, ESL_MSG_LAYOUT, ESL_MSG_CONFIG or ESL_MSG_FW */
} esl_radio_event_t;

/* Handles one received PSDU (psdu_len includes the FCS). If it is a POLL or a
 * LAYOUT_REQ to this AP (PAN pan, short address ESL_AP_SHORT_ADDR), writes the
 * reply frame into tx (ESL_PSDU_MAX bytes) and returns its PSDU length
 * including the FCS. Returns 0 for anything else. ls (layout files) and ev may
 * be NULL.
 *   POLL       -> the store records the status and answers with the first LAYOUT
 *                 block when the tag's layout file is out of date, else CONTENT,
 *                 else CONFIG, else the first FW block when there is tag firmware
 *                 for it (esl_store_fw_offer), else IDLE (esl_store_on_poll)
 *   LAYOUT_REQ -> the LAYOUT block the tag asks for (docs/protokoll.md part 1)
 *   FW_REQ     -> the FW block the tag asks for (docs/tag-ota.md), IDLE when
 *                 the AP no longer has firmware for its model
 * next_poll_s in every reply is the tag's own interval (esl_store_tag_poll_s). */
size_t esl_radio_handle(esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const uint8_t *psdu, size_t psdu_len,
                        int8_t rssi, uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev);

/* The tag firmware images the radio path offers (one per AP; NULL = none,
 * the default). Set once at start-up, before the radio runs. */
void esl_radio_set_tagfw(esl_tagfw_t *fw);

/* ---- fake tag ------------------------------------------------------------
 * Behaves like the tag firmware on the air (tag/src/app.c): sends POLL with the
 * content_id it shows, its cfg_ver and its layout file version, accepts only
 * replies addressed to it with its own sequence number, downloads the layout
 * file block by block when the AP offers a new one, and "draws" every CONTENT
 * it gets (the next POLL reports the new id). Drawing with a file layout looks
 * the layout up in the downloaded file, as the tag does. It applies CONFIG like
 * tag/src/app.c and reports cfg_ver in the next POLL.
 * With ota it reports ESL_POLL_F_OTA and downloads firmware the AP offers
 * (FW / FW_REQ, keeping only a CRC); a whole image with the right CRC becomes
 * its version, as after the restart, unless fw_bad (it then "rolls back" and
 * reports ESL_POLL_F_FWFAIL, as the bootloader would).
 * Older firmware: layouts = false sends the 14-byte POLL (cfg_ver, no layout
 * version) and drops LAYOUT; legacy sends the old 12-byte POLL and drops both
 * LAYOUT and CONFIG. */

#define ESL_FAKETAG_POLL_S 60 /* built-in interval, = CFG_POLL_S in tag/src/config.h */

typedef struct {
    uint64_t mac;
    uint16_t fw;
    uint16_t battery_mv;
    uint8_t model;
    uint8_t seq;
    uint8_t flags;        /* sent in the next POLL; starts as ESL_POLL_F_BOOTED */
    uint32_t shown;       /* content_id on the "screen" */
    esl_content_t content;
    uint16_t next_poll_s; /* last value from the AP (0 = keep) */
    uint32_t idles, contents, configs, rejected;

    bool legacy;          /* firmware without CONFIG or layouts: 12-byte POLL */
    uint16_t cfg_ver;     /* from the last CONFIG, sent in POLL */
    esl_tag_cfg_t cfg;    /* settings from the last CONFIG */
    uint16_t interval_s;  /* the interval the tag would sleep now */

    /* layout files; layouts = false behaves like firmware without them */
    bool layouts;
    uint32_t layout_ver;       /* installed file, 0 = none */
    uint16_t layout_size;
    uint8_t layout[ESL_LBIN_MAX];
    uint32_t dl_ver;           /* download in progress */
    uint16_t dl_size, dl_got;
    uint32_t layout_rejected;  /* a version it could not use */
    bool poll_again;           /* the file changed: poll once more in this round */
    uint8_t drawn_elements;    /* elements of the file layout the last CONTENT was drawn with */
    uint32_t layout_blocks, layout_installs, missing_layouts;

    /* firmware over the air */
    bool ota;                  /* has the SPI flash: reports ESL_POLL_F_OTA */
    bool fw_bad;               /* every image it installs "rolls back" */
    uint16_t fw_dl_ver, fw_dl_size, fw_dl_got;
    uint32_t fw_dl_crc, fw_dl_sum;
    uint16_t fw_failed;        /* reported with ESL_POLL_F_FWFAIL */
    uint32_t fw_blocks, fw_installs;
} esl_faketag_t;

#define ESL_FAKETAG_FW_REQS 128  /* FW_REQ per wake-up, = CFG_FW_MAX_REQ in tag/src/config.h */

void esl_faketag_init(esl_faketag_t *t, uint64_t mac, uint16_t fw, uint16_t battery_mv, uint8_t model);
/* Builds the next POLL frame into psdu (ESL_PSDU_MAX bytes). Returns its length. */
size_t esl_faketag_poll(esl_faketag_t *t, uint16_t pan, uint8_t *psdu);
/* Builds the LAYOUT_REQ for the next missing block of a download in progress
 * into psdu. Returns its length, 0 if there is nothing to ask for. */
size_t esl_faketag_request(esl_faketag_t *t, uint16_t pan, uint8_t *psdu);
/* The FW_REQ for the next block of a firmware download, 0 if none. */
size_t esl_faketag_fw_request(esl_faketag_t *t, uint16_t pan, uint8_t *psdu);
/* Handles a received frame. Returns ESL_MSG_IDLE, ESL_MSG_CONTENT, ESL_MSG_LAYOUT,
 * ESL_MSG_CONFIG or ESL_MSG_FW if it was a valid reply for this tag, 0 if it was ignored. */
uint8_t esl_faketag_receive(esl_faketag_t *t, const uint8_t *psdu, size_t psdu_len);
/* Elements of layout n of the installed file, or NULL. */
const uint8_t *esl_faketag_layout(const esl_faketag_t *t, uint8_t n, uint8_t *nelem);

/* One wake-up of the tag through esl_radio_handle(), as the firmware does it:
 * POLL, then LAYOUT_REQ until the file is complete (at most 16), then one more
 * POLL if the file changed; or after a FW block, FW_REQ until the image is
 * complete (at most ESL_FAKETAG_FW_REQS). Returns the type of the last reply it accepted
 * (0 = none). */
uint8_t esl_faketag_round(esl_faketag_t *t, esl_store_t *st, esl_layouts_t *ls, uint16_t pan, int8_t rssi,
                          uint64_t now_ms);
