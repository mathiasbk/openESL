#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_rcache.h"
#include "esl_rhost.h"

/* The link between a network radio (esl_rcache, on a C6) and its host (esl_rhost, on
 * the S3) over a byte stream: TCP with TLS on the devices, a buffer in the tests
 * (docs/ap-netradio.md, "Link protocol").
 *
 * Every message is a frame: its length (2 bytes, LE, the type byte included), its type
 * (1 byte) and its payload; all numbers little-endian. TCP and TLS make sure the bytes
 * arrive whole and in order, so frames carry no CRC. The radio connects and starts:
 *
 *   radio -> host   HELLO   once per connection: who it is and how much it holds
 *   host -> radio   CONFIG  the channel and PAN to serve (also when they change), and
 *                           the transmit power once the AP has one to give
 *   radio -> host   SYNC    esl_rsync_t, then its log_n LOG entries
 *   host -> radio   the answer: CLEAR, DEFAULT_POLL, TAG, DROP, IMAGE, FW + FW_DATA, then END
 *
 * Only the radio starts a sync (the host answers, it never sends on its own, docs:
 * "Limits"), and one sync is open at a time. A connection that breaks mid-answer is
 * no harm: the next SYNC carries the last END the radio got (ack), and the host tells
 * it everything again (esl_rhost_begin).
 *
 * Tag firmware goes as FW (model, version, size, CRC) and then FW_DATA parts, written
 * by the radio through ops.write_fw (flash on the C6). The radio offers it only once
 * every byte has arrived. */

#define ESL_NLINK_VERSION 1
#define ESL_NLINK_HDR     3      /* length (2) + type (1) */
#define ESL_NLINK_FW_PART 1024   /* firmware bytes per FW_DATA */

/* radio -> host */
#define ESL_NLINK_HELLO   0x01
#define ESL_NLINK_SYNC    0x02
#define ESL_NLINK_LOG     0x03
/* host -> radio */
#define ESL_NLINK_CONFIG       0x81
#define ESL_NLINK_CLEAR        0x82
#define ESL_NLINK_TAG          0x83
#define ESL_NLINK_DROP         0x84
#define ESL_NLINK_IMAGE        0x85
#define ESL_NLINK_FW           0x86
#define ESL_NLINK_FW_DATA      0x87
#define ESL_NLINK_DEFAULT_POLL 0x88
#define ESL_NLINK_END          0x89

/* Writes n bytes to the stream; false when it broke. */
typedef bool (*esl_nlink_write_t)(void *ctx, const uint8_t *data, size_t n);

/* ---- frames ---- */

/* Collects frames from the stream. buf (cap bytes) holds the largest frame expected:
 * an IMAGE of the largest image plus 16. */
typedef struct {
    uint8_t *buf;
    size_t cap, got;
} esl_nlink_rx_t;

void esl_nlink_rx_init(esl_nlink_rx_t *rx, uint8_t *buf, size_t cap);
/* Feeds n bytes; for each whole frame calls on(ctx, type, payload, len), which returns
 * false to stop. False when a frame is too large or empty, or on() said stop: the
 * connection should then be closed. */
bool esl_nlink_feed(esl_nlink_rx_t *rx, const uint8_t *data, size_t n,
                    bool (*on)(void *ctx, uint8_t type, const uint8_t *p, size_t len), void *ctx);

/* ---- the radio's side ---- */

typedef struct {
    uint32_t boot;
    uint16_t radio_ver;      /* its firmware version, as the AP's (major * 10000 + ...) */
    uint64_t id;             /* its own MAC */
    uint16_t max_tags, n_images;
    uint32_t image_cap;
    uint8_t fw_models;       /* tag firmware it can hold: models 1..fw_models */
    uint8_t features;        /* ESL_NLINK_F_*; 0 from a radio before 0.9.0, which leaves it out */
} esl_nlink_hello_t;

/* HELLO features */
#define ESL_NLINK_F_CHANNEL 0x01 /* passes a planned channel move to the tags (TAG carries it, #256) */

typedef struct {
    void *ctx;
    /* the channel and PAN the host serves; the radio retunes */
    void (*config)(void *ctx, uint8_t channel, uint16_t pan);
    /* n bytes of the tag firmware for model at off (ESL_NLINK_FW_PART at most); off 0
       starts it over. False when it cannot be written. May be NULL (no firmware). */
    bool (*write_fw)(void *ctx, uint8_t model, uint32_t off, const uint8_t *data, size_t n);
    /* the transmit power in dBm (a CONFIG with it; an older AP's has none). May be NULL. */
    void (*tx_power)(void *ctx, int8_t dbm);
} esl_nlink_radio_ops_t;

/* Tag firmware arriving for one model */
typedef struct {
    uint16_t ver, size;
    uint32_t crc, got;
} esl_nlink_fw_rx_t;

typedef struct {
    esl_rcache_t *rc;
    esl_nlink_radio_ops_t ops;
    esl_nlink_write_t write;
    void *wctx;
    esl_nlink_fw_rx_t fw[ESL_RCACHE_MODELS];
    bool in_sync;            /* a SYNC is out, its END not in yet */
    uint32_t syncs, ends;
} esl_nlink_radio_t;

void esl_nlink_radio_init(esl_nlink_radio_t *r, esl_rcache_t *rc, const esl_nlink_radio_ops_t *ops,
                          esl_nlink_write_t write, void *wctx);
/* On a new connection: HELLO (and a sync open on the old one is given up). */
bool esl_nlink_radio_hello(esl_nlink_radio_t *r, const esl_nlink_hello_t *h);
/* Starts a sync (now_ms on the radio's clock): SYNC and the log. */
bool esl_nlink_radio_sync(esl_nlink_radio_t *r, uint64_t now_ms);
/* A frame from the host, applied to the cache. False on a frame it cannot take. */
bool esl_nlink_radio_on(esl_nlink_radio_t *r, uint8_t type, const uint8_t *p, size_t len);

/* ---- the host's side ---- */

typedef struct {
    esl_rhost_t *h;
    esl_tagfw_t *fw;         /* for the firmware bytes, may be NULL */
    esl_nlink_write_t write;
    void *wctx;
    uint8_t channel;
    uint16_t pan;
    int8_t tx_power;         /* goes with CONFIG when tx_set */
    bool tx_set;
    bool hello;              /* the radio said HELLO on this connection */
    esl_nlink_hello_t radio;
    esl_rsync_t req;         /* the sync being received */
    bool in_sync;
    unsigned log_left;
    bool broken;             /* a write failed: the connection is to be closed */
    bool config_due;         /* CONFIG goes with the next answer */
    uint64_t (*now_ms)(void);
} esl_nlink_host_t;

/* now_ms gives the host's clock (ms since boot). */
void esl_nlink_host_init(esl_nlink_host_t *n, esl_rhost_t *h, esl_tagfw_t *fw, uint8_t channel, uint16_t pan,
                         esl_nlink_write_t write, void *wctx, uint64_t (*now_ms)(void));
/* A frame from the radio: HELLO is answered with CONFIG, SYNC and its log with the
 * answer. False when the connection is to be closed. */
bool esl_nlink_host_on(esl_nlink_host_t *n, uint8_t type, const uint8_t *p, size_t len);
/* The channel or PAN changed: CONFIG goes with the next answer. */
void esl_nlink_host_set_config(esl_nlink_host_t *n, uint8_t channel, uint16_t pan);
/* The transmit power (dBm) for the radio: in every CONFIG from now on, the next with
 * the next answer. Before it is called, CONFIG has none and the radio keeps its own. */
void esl_nlink_host_set_tx_power(esl_nlink_host_t *n, int8_t dbm);
