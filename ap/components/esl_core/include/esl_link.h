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
 *
 * A link to a radio on another chip (a second ESP32 over UART, say) is another
 * driver with the same three calls. Frames are always whole 802.15.4 PSDUs
 * (docs/protokoll.md): the link carries them, it does not change them. */

typedef struct esl_link esl_link_t;

/* One received frame, as a driver hands it to radio.c */
typedef struct {
    uint8_t len;                /* PSDU length including the FCS */
    uint8_t psdu[ESL_PSDU_MAX];
    int8_t rssi;
    uint8_t channel;
    int64_t rx_us;              /* when it arrived, on the AP's clock (esp_timer) */
} esl_link_rx_t;

typedef struct {
    uint32_t rx;        /* frames received */
    uint32_t polls;     /* POLLs answered */
    uint32_t blocks;    /* FW_REQ and IMG_REQ answered */
    uint32_t replies;   /* replies that went out */
    uint32_t tx_failed; /* replies that did not */
    uint32_t tx_resent; /* resends the driver needed (it counts them) */
    uint32_t dropped;   /* frames lost before the task saw them (it counts them) */
    int64_t last_us;    /* time from the last answered frame's arrival until its reply went to send() */
    int64_t worst_us;   /* the longest last_us */
} esl_link_stats_t;

struct esl_link {
    const char *name;   /* for the log: "802.15.4" */
    /* Starts receiving on channel with PAN pan and short address short_addr. With
     * promiscuous, every frame on the channel is received (the sniffer). Returns 0
     * or the driver's error. */
    int (*start)(esl_link_t *l, uint8_t channel, uint16_t pan, uint16_t short_addr, bool promiscuous);
    /* Sends one reply (len is the PSDU length including the FCS; the driver fills
     * in the FCS if its radio does not). rx_us is when the request arrived: the tag
     * listens for CFG_RX_TIMEOUT_MS (20 ms) after it, so a driver that resends
     * stops in time. Returns 0 if it went out, else the driver's error. */
    int (*send)(esl_link_t *l, const uint8_t *psdu, size_t len, int64_t rx_us);
    /* Changes channel while running (the sniffer's hopping); NULL if not possible */
    int (*set_channel)(esl_link_t *l, uint8_t channel);
    void *ctx;          /* the driver's own */
    esl_link_stats_t stats;
};

/* One received frame through esl_radio_handle(): if it needs a reply, the reply is
 * built in tx (ESL_PSDU_MAX bytes) and sent with l->send. Counts it in l->stats;
 * clock_us (the clock of rx_us) times how long the reply took to build, for
 * last_us and worst_us (requirement A3: under 5 ms), NULL = not timed.
 * Returns the reply's length (0 = the frame needed none) and fills ev; *sent says
 * whether it went out. ls, ev and sent may be NULL. */
size_t esl_link_handle(esl_link_t *l, esl_store_t *st, esl_layouts_t *ls, uint16_t pan, const esl_link_rx_t *f,
                       uint64_t now_ms, uint8_t *tx, esl_radio_event_t *ev, int64_t (*clock_us)(void), bool *sent);
