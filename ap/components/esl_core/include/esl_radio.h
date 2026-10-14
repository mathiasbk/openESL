#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_layout.h"
#include "esl_proto.h"
#include "esl_store.h"

/* The AP's receive path, independent of the radio driver: the 802.15.4 glue
 * (main/radio_ieee802154.c) hands every received PSDU to esl_radio_handle()
 * and transmits what comes back. The simulated tags in main/radio_stub.c and
 * the host tests go through the same function, so only the driver itself and
 * the timing are left for the hardware. */

typedef struct {
    uint64_t mac;
    uint8_t msg_type;   /* ESL_MSG_POLL or ESL_MSG_LAYOUT_REQ */
    esl_poll_t poll;    /* POLL only */
    esl_layout_req_t req; /* LAYOUT_REQ only */
    uint8_t reply_type; /* ESL_MSG_IDLE, ESL_MSG_CONTENT or ESL_MSG_LAYOUT */
} esl_radio_event_t;

/* Handles one received PSDU (psdu_len includes the FCS). If it is a POLL or a
 * LAYOUT_REQ to this AP (PAN pan, short address ESL_AP_SHORT_ADDR), writes the
 * reply frame into tx (ESL_PSDU_MAX bytes) and returns its PSDU length
 * including the FCS. Returns 0 for anything else. ls (layout files) and ev may
 * be NULL.
 *   POLL       -> the store records the status and answers IDLE or CONTENT, or
 *                 the first LAYOUT block when the tag's layout file is out of date
 *   LAYOUT_REQ -> the LAYOUT block the tag asks for (docs/protokoll.md part 1) */
size_t esl_radio_handle(esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const uint8_t *psdu, size_t psdu_len,
                        int8_t rssi, uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev);

/* ---- fake tag ------------------------------------------------------------
 * Behaves like the tag firmware on the air (tag/src/app.c): sends POLL with the
 * content_id it shows and its layout file version, accepts only replies
 * addressed to it with its own sequence number, downloads the layout file
 * block by block when the AP offers a new one, and "draws" every CONTENT it
 * gets (the next POLL reports the new id). Drawing with a file layout looks
 * the layout up in the downloaded file, as the tag does. */

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
    uint32_t idles, contents, rejected;

    /* layout files; layouts = false behaves like firmware without them (12-byte POLL) */
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
} esl_faketag_t;

void esl_faketag_init(esl_faketag_t *t, uint64_t mac, uint16_t fw, uint16_t battery_mv, uint8_t model);
/* Builds the next POLL frame into psdu (ESL_PSDU_MAX bytes). Returns its length. */
size_t esl_faketag_poll(esl_faketag_t *t, uint16_t pan, uint8_t *psdu);
/* Builds the LAYOUT_REQ for the next missing block of a download in progress
 * into psdu. Returns its length, 0 if there is nothing to ask for. */
size_t esl_faketag_request(esl_faketag_t *t, uint16_t pan, uint8_t *psdu);
/* Handles a received frame. Returns ESL_MSG_IDLE, ESL_MSG_CONTENT or
 * ESL_MSG_LAYOUT if it was a valid reply for this tag, 0 if it was ignored. */
uint8_t esl_faketag_receive(esl_faketag_t *t, const uint8_t *psdu, size_t psdu_len);
/* Elements of layout n of the installed file, or NULL. */
const uint8_t *esl_faketag_layout(const esl_faketag_t *t, uint8_t n, uint8_t *nelem);

/* One wake-up of the tag through esl_radio_handle(), as the firmware does it:
 * POLL, then LAYOUT_REQ until the file is complete (at most 16), then one more
 * POLL if the file changed. Returns the type of the last reply it accepted
 * (0 = none). */
uint8_t esl_faketag_round(esl_faketag_t *t, esl_store_t *st, esl_layouts_t *ls, uint16_t pan, int8_t rssi,
                          uint64_t now_ms);
