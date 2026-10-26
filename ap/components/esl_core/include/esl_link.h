#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_proto.h"
#include "esl_radio.h"

/* The radio link between the AP and its tags, independent of what carries the
 * frames. A link receives PSDUs from tags and sends the replies; everything
 * between (esl_radio_handle) is the same for every link. ap/main/radio.c runs
 * the task around it, and each driver fills in an esl_link_t:
 *
 *   main/radio_ieee802154.c  the 802.15.4 radio of the ESP32-C6/C5 itself
 *   main/radio_uart.c        an ESP32-C6 running ap/radio/ as a radio coprocessor
 *                            on UART (docs/ap-radio-link.md)
 * Frames are always whole 802.15.4 PSDUs
 * (docs/protokoll.md): the link carries them, it does not change them. The
 * protocol (what a POLL is, what the reply holds) is esl_radio_handle()'s and
 * never a driver's; a tag that speaks another protocol gets another handler,
 * not another link. A radio that is not 802.15.4 (a tag maker's own 2.4 GHz
 * radio) would need start() and the frame format widened first. One link per
 * build (radio_link in ap/main/radio.h). */

typedef struct esl_link esl_link_t;

/* One received frame, as a driver hands it to radio.c */
typedef struct {
    uint8_t len;                /* PSDU length including the FCS */
    uint8_t psdu[ESL_PSDU_MAX];
    int8_t rssi;
    uint8_t channel;
    int64_t rx_us;              /* when it arrived, on the AP's clock (esp_timer) */
    uint32_t ref;               /* the driver's own (a coprocessor's id for the frame); the rest leave it alone */
} esl_link_rx_t;

typedef struct {
    uint32_t rx;        /* frames received */
    uint32_t polls;     /* POLLs answered */
    uint32_t blocks;    /* FW_REQ and IMG_REQ answered */
    uint32_t replies;   /* replies send() took */
    uint32_t tx_failed; /* replies that did not go out (each reply once, however many tries) */
    uint32_t tx_resent; /* resends the driver needed (it counts them) */
    uint32_t dropped;   /* frames lost before the task saw them (it counts them) */
    int64_t last_us;    /* the last answered frame: from its arrival until its reply was on its way
                           (esl_link_sending(), or the return of send()) */
    int64_t worst_us;   /* the longest last_us */
    uint32_t slow;      /* replies with last_us over ESL_LINK_SLOW_US */
} esl_link_stats_t;

#define ESL_LINK_SLOW_US 5000 /* krav A3: a reply should be on its way within 5 ms */

struct esl_link {
    const char *name;   /* for the log: "802.15.4" */
    /* Starts receiving on channel with PAN pan and short address short_addr. With
     * promiscuous, every frame on the channel is received (the sniffer). Returns 0
     * or the driver's error. */
    int (*start)(esl_link_t *l, uint8_t channel, uint16_t pan, uint16_t short_addr, bool promiscuous);
    /* Sends one reply to req (len is the PSDU length including the FCS; the driver
     * fills in the FCS if its radio does not). The tag listens for CFG_RX_TIMEOUT_MS
     * (20 ms) after its request, so a driver that resends stops in time: by
     * req->rx_us, or by req->ref on a radio that keeps its own time. Returns 0 if
     * the reply went out, or, on a radio that reports later, was taken (such a
     * driver reports a reply that then did not go out with esl_link_lost());
     * else the driver's error. A driver calls esl_link_sending() when the reply
     * is handed to the radio, so the time it waited for the radio counts. */
    int (*send)(esl_link_t *l, const uint8_t *psdu, size_t len, const esl_link_rx_t *req);
    /* Changes channel while running (the sniffer's hopping); NULL if not possible */
    int (*set_channel)(esl_link_t *l, uint8_t channel);
    void *ctx;          /* the driver's own */
    esl_link_stats_t stats;
};

/* One received frame through esl_radio_handle(): if it needs a reply, the reply is
 * built in tx (ESL_PSDU_MAX bytes) and sent with l->send. Counts it in l->stats;
 * clock_us (the clock of rx_us) times it for last_us and worst_us (requirement
 * A3: under 5 ms from the request to the reply on its way), NULL = not timed.
 * Returns the reply's length (0 = the frame needed none) and fills ev; *sent says
 * whether it went out. ls, ev and sent may be NULL. */
size_t esl_link_handle(esl_link_t *l, esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const esl_link_rx_t *f,
                       uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev, int64_t (*clock_us)(void), bool *sent);

/* From the driver's send(): the reply is handed to the radio now (now_us on the
 * clock of req->rx_us). Sets last_us; without it esl_link_handle() takes the time
 * send() returned, which includes the time on the air. */
void esl_link_sending(esl_link_t *l, const esl_link_rx_t *req, int64_t now_us);

/* From a driver whose send() returned 0 before the radio said: that reply did not
 * go out after all. Call it from the radio task (radio_rx() a frame for it, say),
 * since the radio task writes the same counters. */
void esl_link_lost(esl_link_t *l);
