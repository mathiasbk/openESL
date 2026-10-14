#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_proto.h"
#include "esl_store.h"

/* The AP's receive path, independent of the radio driver: the 802.15.4 glue
 * (main/radio_ieee802154.c) hands every received PSDU to esl_radio_handle()
 * and transmits what comes back. The simulated tags in main/radio_stub.c and
 * the host tests go through the same function, so only the driver itself and
 * the timing are left for the hardware. */

typedef struct {
    uint64_t mac;
    esl_poll_t poll;
    uint8_t reply_type; /* ESL_MSG_IDLE or ESL_MSG_CONTENT */
} esl_radio_event_t;

/* Handles one received PSDU (psdu_len includes the FCS). If it is a POLL to
 * this AP (PAN pan, short address ESL_AP_SHORT_ADDR), updates the store,
 * writes the reply frame into tx (ESL_PSDU_MAX bytes) and returns its PSDU
 * length including the FCS. Returns 0 for anything else. ev may be NULL. */
size_t esl_radio_handle(esl_store_t *st, uint16_t pan, const uint8_t *psdu, size_t psdu_len, int8_t rssi,
                        uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev);

/* ---- fake tag ------------------------------------------------------------
 * Behaves like the tag firmware on the air: sends POLL with the content_id it
 * shows, accepts only replies addressed to it with its own sequence number,
 * and "draws" every CONTENT it gets (the next POLL reports the new id). */

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
} esl_faketag_t;

void esl_faketag_init(esl_faketag_t *t, uint64_t mac, uint16_t fw, uint16_t battery_mv, uint8_t model);
/* Builds the next POLL frame into psdu (ESL_PSDU_MAX bytes). Returns its length. */
size_t esl_faketag_poll(esl_faketag_t *t, uint16_t pan, uint8_t *psdu);
/* Handles a received frame. Returns ESL_MSG_IDLE or ESL_MSG_CONTENT if it was a
 * valid reply for this tag, 0 if it was ignored. */
uint8_t esl_faketag_receive(esl_faketag_t *t, const uint8_t *psdu, size_t psdu_len);
