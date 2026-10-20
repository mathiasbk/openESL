#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The UART link between an AP host (ESP32-S3, main/radio_uart.c) and an
 * 802.15.4 radio coprocessor (ESP32-C6, ap/radio/). The host does everything
 * the AP does; the radio only forwards received frames and transmits the
 * replies (docs/ap-radio-link.md).
 *
 * Every message is one frame on the wire:
 *
 *   0xA5  type  len  payload[len]  crc_lo  crc_hi
 *
 * crc is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type, len and the
 * payload. The receiver hunts for 0xA5, so garbage on the line (the C6's ROM
 * boot messages, a half-sent frame after a reset) costs at most the frames it
 * overlaps. All multi-byte fields are little-endian.
 *
 * No ESP-IDF headers here: the host tests (ap/test) build this file too. */

#define ESL_LINK_SYNC        0xA5
#define ESL_LINK_VERSION     1
#define ESL_LINK_BAUD        921600
#define ESL_LINK_PAYLOAD_MAX 160
#define ESL_LINK_OVERHEAD    5   /* sync, type, len, crc */
#define ESL_LINK_FRAME_MAX   (ESL_LINK_PAYLOAD_MAX + ESL_LINK_OVERHEAD)
#define ESL_LINK_PSDU_MAX    127 /* = ESL_MAC_MAX_FRAME in shared/proto.h */

/* Message types. radio -> host have the high bit set. */
enum {
    ESL_LINK_PING    = 0x01, /* host -> radio, no payload; the radio answers HELLO */
    ESL_LINK_CONFIG  = 0x02, /* host -> radio, esl_link_config_t; the radio answers HELLO */
    ESL_LINK_TX      = 0x03, /* host -> radio, ref + PSDU: send this frame */
    ESL_LINK_STATS_Q = 0x04, /* host -> radio, no payload; the radio answers STATS */

    ESL_LINK_HELLO   = 0x81, /* radio -> host, esl_link_hello_t: at boot and on PING/CONFIG */
    ESL_LINK_RX      = 0x82, /* radio -> host, esl_link_rx_t: a received frame */
    ESL_LINK_TX_DONE = 0x83, /* radio -> host, esl_link_tx_done_t */
    ESL_LINK_STATS   = 0x84, /* radio -> host, esl_link_stats_t */
};

/* CONFIG flags */
#define ESL_LINK_CFG_SNIFFER 0x01 /* promiscuous: forward every frame on the channel */

typedef struct {
    uint8_t channel;    /* 11-26 */
    uint16_t pan;
    uint16_t short_addr;
    int8_t tx_power;    /* dBm */
    uint8_t flags;      /* ESL_LINK_CFG_* */
} esl_link_config_t;

typedef struct {
    uint8_t version;    /* ESL_LINK_VERSION of the radio firmware */
    uint8_t configured; /* 1 once a CONFIG was applied since boot; 0 = send CONFIG */
    uint16_t fw;        /* radio firmware version, major << 8 | minor */
    uint64_t ieee_addr; /* the radio's own 64-bit address, for the log */
    esl_link_config_t cfg; /* what it runs with now (when configured) */
} esl_link_hello_t;

/* A received frame. ref numbers the frames (wraps at 256): a TX that answers
 * it carries the same ref, so the radio can time its resends from when this
 * frame arrived. */
typedef struct {
    uint8_t ref;
    int8_t rssi;
    uint8_t channel;
    uint8_t len;        /* PSDU length incl. FCS */
    uint8_t psdu[ESL_LINK_PSDU_MAX];
} esl_link_rx_t;

#define ESL_LINK_REF_NONE 0xFF /* a TX that answers no frame; RX never uses this ref */

/* TX_DONE status */
enum {
    ESL_LINK_TX_OK = 0,      /* sent (possibly after resends) */
    ESL_LINK_TX_FAILED = 1,  /* the radio refused it until the tag stopped listening */
    ESL_LINK_TX_LATE = 2,    /* arrived after the tag stopped listening: not sent */
    ESL_LINK_TX_BUSY = 3,    /* another transmit was still going on */
};

typedef struct {
    uint8_t ref;
    uint8_t status;     /* ESL_LINK_TX_* */
    uint8_t resends;
    uint16_t rx_to_tx_us; /* from the RX frame's arrival to the first transmit (0xFFFF = more) */
} esl_link_tx_done_t;

typedef struct {
    uint32_t rx, tx, tx_failed, tx_resent, tx_late, dropped, link_errors;
    uint32_t uptime_s;
} esl_link_stats_t;

/* ---- encoding: each returns the frame length written to out
 * (ESL_LINK_FRAME_MAX bytes), 0 if it does not fit. ---- */

size_t esl_link_frame(uint8_t *out, uint8_t type, const void *payload, size_t len);
size_t esl_link_encode_config(uint8_t *out, const esl_link_config_t *c);
size_t esl_link_encode_hello(uint8_t *out, const esl_link_hello_t *h);
size_t esl_link_encode_rx(uint8_t *out, const esl_link_rx_t *rx);
size_t esl_link_encode_tx(uint8_t *out, uint8_t ref, const uint8_t *psdu, size_t len);
size_t esl_link_encode_tx_done(uint8_t *out, const esl_link_tx_done_t *d);
size_t esl_link_encode_stats(uint8_t *out, const esl_link_stats_t *s);

/* ---- decoding the payload of a received message: false if it is malformed ---- */

bool esl_link_decode_config(const uint8_t *p, size_t len, esl_link_config_t *c);
bool esl_link_decode_hello(const uint8_t *p, size_t len, esl_link_hello_t *h);
bool esl_link_decode_rx(const uint8_t *p, size_t len, esl_link_rx_t *rx);
/* TX: ref and the PSDU (points into p). */
bool esl_link_decode_tx(const uint8_t *p, size_t len, uint8_t *ref, const uint8_t **psdu, size_t *psdu_len);
bool esl_link_decode_tx_done(const uint8_t *p, size_t len, esl_link_tx_done_t *d);
bool esl_link_decode_stats(const uint8_t *p, size_t len, esl_link_stats_t *s);

/* ---- receiving: feed bytes as they come ---- */

typedef struct {
    uint8_t state;
    uint8_t type;
    uint8_t len;
    uint8_t got;
    uint16_t crc;
    uint8_t payload[ESL_LINK_PAYLOAD_MAX];
    uint32_t bad_crc, too_long, skipped; /* skipped = bytes outside a frame */
} esl_link_parser_t;

void esl_link_parser_init(esl_link_parser_t *p);
/* Takes one byte. Returns true when it completed a frame with a good CRC:
 * p->type, p->len and p->payload hold it until the next call. */
bool esl_link_parser_feed(esl_link_parser_t *p, uint8_t b);

uint16_t esl_link_crc16(uint16_t crc, const uint8_t *data, size_t len);
