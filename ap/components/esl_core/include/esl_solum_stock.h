#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_layout.h"
#include "esl_proto.h"
#include "esl_store.h"

/* SoluM's stock firmware: the vendor's own tag protocol, next to the openESL protocol
 * (esl_radio). This is one vendor's factory firmware, not a general "stock" driver: the
 * frames, the model bytes and the image format are all SoluM's. A tag that speaks another
 * protocol gets another handler, not another link (esl_link.h), so another vendor's stock
 * firmware is a sibling of this file, not a change to it.
 *
 * The two labels are the same panels openESL already supports (2.9" = ESL_MODEL_29,
 * 1.54" = ESL_MODEL_154); SoluM's firmware numbers them 0x87 and 0x85 on the air
 * (esl_solum_stock_model_to_esl). Such a tag never polls in the openESL format: it scans
 * for its gateway, activates, then asks for an image job and fetches it as a run of
 * fragments. The AP keeps the tag's content as for any tag (esl_store), draws the screen
 * with esl_render and sends the result as one job (esl_solum_stock_stream).
 *
 * The image the tag wants is SoluM's own: a 1-bit BMP file, zlib (RFC 1950) compressed,
 * behind a small page block. esl_solum_stock_job() turns esl_render's column format into
 * exactly that, including the panel's own rotation and mirroring, and compresses it
 * (esl_solum_stock_deflate, fixed Huffman, written from RFC 1951/1950). Nothing is
 * stored: the image is built when a job goes out.
 *
 * Everything here is platform-independent (built and tested on the host), so it may
 * not include ESP-IDF headers. The AP's radio glue (main/radio*.c) recognises the
 * messages with esl_solum_stock_parse() and answers with the builders, or runs the whole
 * per-tag state machine through esl_solum_stock_handle(). */

/* ---- the on-air messages (payload byte 0) ---- */
#define ESL_SOLUM_STOCK_MSG_SCAN   0xE0 /* tag -> AP, broadcast: looking for its gateway */
#define ESL_SOLUM_STOCK_MSG_SCAN_R 0xE1 /* AP -> tag */
#define ESL_SOLUM_STOCK_MSG_JOIN   0xD7 /* tag -> AP: activation request */
#define ESL_SOLUM_STOCK_MSG_JOIN_R 0xD8 /* AP -> tag */
#define ESL_SOLUM_STOCK_MSG_POLL   0xAA /* tag -> AP: inquiry; carries the job it holds */
#define ESL_SOLUM_STOCK_MSG_IMG    0xAD /* AP -> tag: one fragment of an image job */
#define ESL_SOLUM_STOCK_MSG_NONE   0xDE /* AP -> tag: nothing new */
#define ESL_SOLUM_STOCK_MSG_STATUS 0xF4 /* tag -> AP: result of a job (the msg, then the code) */

/* SoluM's on-air model byte (SCAN, POLL, STATUS). */
#define ESL_SOLUM_STOCK_MODEL_154 0x85 /* the 1.54" panel */
#define ESL_SOLUM_STOCK_MODEL_29  0x87 /* the 2.9" panel */

/* SoluM's model byte <-> the openESL model (ESL_MODEL_*). 0 for one openESL does
 * not know. */
uint8_t esl_solum_stock_model_to_esl(uint8_t stock_model);
uint8_t esl_solum_stock_model_from_esl(uint8_t model);

/* ---- sizes ---- */
/* The 1-bit BMP SoluM's firmware renders from, worst case: a 62-byte header and
 * 200 rows of 28 bytes (the 1.54"; the 2.9" is 296 rows of 16). */
#define ESL_SOLUM_STOCK_BMP_MAX    (62 + 200 * 28)
/* The whole job data: the 17-byte page block, then the zlib stream. Fixed Huffman
 * can expand by up to 9 bits per byte (the literals 144..255), so leave room for that
 * plus the stream's own headers, or a screen that does not compress would give none. */
#define ESL_SOLUM_STOCK_JOB_MAX    (17 + (ESL_SOLUM_STOCK_BMP_MAX * 9 / 8) + 16)
/* The full stream a job goes out as: the 11-byte head, then the job data. */
#define ESL_SOLUM_STOCK_STREAM_MAX (11 + ESL_SOLUM_STOCK_JOB_MAX)
/* The largest esl_render image of either model: the 1.54" is 200 columns of 25 bytes. */
#define ESL_SOLUM_STOCK_RENDER_MAX 5000

/* Scratch for the job builder, the compressor and esl_solum_stock_handle(). Not
 * reentrant while one call runs; allocate once (esl_solum_stock_mem_size()). */
size_t esl_solum_stock_mem_size(void);

/* ---- the image ---- */
/* zlib (RFC 1950, wbits 10, a 1 KB window) of n bytes into out (cap bytes): its
 * length, 0 when it does not fit or n is over ESL_SOLUM_STOCK_BMP_MAX. Fixed Huffman
 * codes, so it inflates with any decoder. scratch is esl_solum_stock_mem_size() bytes. */
size_t esl_solum_stock_deflate(void *scratch, const uint8_t *in, size_t n, uint8_t *out, size_t cap);

/* The job data for a rendered screen of model: the 17-byte page block, then the zlib
 * stream of SoluM's 1-bit BMP. render is esl_render_size(model) bytes in
 * esl_render's format (bit 1 = ink). out holds ESL_SOLUM_STOCK_JOB_MAX bytes; the length is
 * returned, 0 for an unknown model or when it does not fit. The 2.9" panel is mounted
 * portrait, so its landscape image is turned a quarter clockwise (which, with the
 * panel's own mirroring, is a transpose); the 1.54" is only mirrored. scratch is
 * esl_solum_stock_mem_size() bytes. */
size_t esl_solum_stock_job(void *scratch, uint8_t model, const uint8_t *render, uint8_t *out, size_t cap);

/* The full stream a job goes out as: the 11-byte head (job number, channel byte), then
 * the job data (esl_solum_stock_job). out holds ESL_SOLUM_STOCK_STREAM_MAX bytes. channel
 * is the 802.15.4 channel (the head's channel byte was checked on channel 20). */
size_t esl_solum_stock_stream(void *scratch, uint8_t model, const uint8_t *render, uint32_t job, uint8_t channel,
                        uint8_t *out, size_t cap);

/* ---- frames ---- */
/* The stream head is 11 bytes and a fragment header is 7; both are public so the
 * handler and the tests can slice a stream the same way. */
#define ESL_SOLUM_STOCK_STREAM_HEAD 11
#define ESL_SOLUM_STOCK_FRAG_HDR    7
#define ESL_SOLUM_STOCK_FRAG_DATA   95 /* data bytes per fragment, the largest that fits a PSDU */
#define ESL_SOLUM_STOCK_FRAGS_MAX   ((ESL_SOLUM_STOCK_STREAM_MAX + ESL_SOLUM_STOCK_FRAG_DATA - 1) / ESL_SOLUM_STOCK_FRAG_DATA)

typedef struct {
    uint8_t msg;         /* ESL_SOLUM_STOCK_MSG_* */
    uint8_t seq;         /* the frame's sequence number */
    uint8_t model;       /* SoluM's model byte (SCAN, JOIN, POLL, STATUS); else 0 */
    uint64_t mac;        /* the tag's EUI-64 (frames with a long address); else 0 */
    uint16_t short_addr; /* the tag's short address (SCAN); else 0 */
    uint16_t pan;        /* the frame's PAN ID */
    uint8_t dst[8];      /* the destination address, 8 bytes (a short one in dst[0..1]) */
    uint32_t job;        /* the job the tag holds (POLL) or reports on (STATUS) */
    uint8_t code;        /* STATUS: the result code (0 = the job was taken) */
    uint8_t status_msg;  /* STATUS: the message type the code is about */
    const uint8_t *payload; /* the payload, without the MAC header */
    size_t payload_len;
} esl_solum_stock_frame_t;

/* Reads one tag -> AP frame (psdu = MAC header + payload, no FCS). False when it is
 * not one of SoluM's frames. */
bool esl_solum_stock_parse(const uint8_t *psdu, size_t psdu_len, esl_solum_stock_frame_t *out);

/* The AP's answers; each builds a whole frame (MAC header + payload, no FCS) into out
 * (ESL_PSDU_MAX bytes) and returns its length. gw is the AP's 8-byte EUI-64 in on-air
 * order, as the tag reads it from the scan reply and then addresses its own frames to. */

/* SCAN_R to a tag that broadcast SCAN from short_addr. */
size_t esl_solum_stock_build_scan_reply(uint8_t *out, uint16_t pan, uint16_t short_addr, const uint8_t gw[8]);
/* JOIN_R to a tag that sent JOIN (mac, seq). */
size_t esl_solum_stock_build_join_reply(uint8_t *out, uint16_t pan, uint64_t mac, const uint8_t gw[8], uint8_t seq);
/* NONE to a tag that polled and needs no image (mac, the job it holds, seq). */
size_t esl_solum_stock_build_none(uint8_t *out, uint16_t pan, uint64_t mac, const uint8_t gw[8], uint32_t job, uint8_t seq);
/* One IMG fragment: the header (total, idx, ts) and n data bytes (at most
 * ESL_SOLUM_STOCK_FRAG_DATA). */
size_t esl_solum_stock_build_img(uint8_t *out, uint16_t pan, uint64_t mac, const uint8_t gw[8], uint8_t seq,
                           uint16_t total, uint16_t idx, uint16_t ts, const uint8_t *data, size_t n);

/* ---- the per-tag state machine ---- */
#define ESL_SOLUM_STOCK_TAGS 8

typedef struct {
    bool used;
    uint64_t mac;
    uint8_t model;         /* ESL_MODEL_* */
    uint32_t content_id;   /* the content the last job was built from */
    uint32_t job;          /* the job number of the last image sent (0 = none) */
    uint64_t sent_ms;
} esl_solum_stock_tag_t;

typedef struct {
    uint8_t gw[8];                  /* the AP's EUI-64, on-air order */
    uint8_t channel;                /* for the stream head's channel byte */
    uint8_t seq;                    /* the next frame's sequence number */
    esl_solum_stock_tag_t tag[ESL_SOLUM_STOCK_TAGS];
    uint32_t next_job;              /* the job numbers handed out */
    uint32_t jobs;                  /* jobs sent (for the log and the tests) */
    uint32_t dropped;               /* frames a full tag table or a full buffer dropped */
    void *scratch;                  /* esl_solum_stock_mem_size() bytes */
} esl_solum_stock_t;

/* mem is esl_solum_stock_mem_size() bytes. gw is the AP's 8-byte EUI-64, on-air order. */
void esl_solum_stock_init(esl_solum_stock_t *sk, void *mem, const uint8_t gw[8], uint8_t channel);

/* Called for each reply frame the AP must send, in order; returns false to stop the
 * run (the rest of a job's fragments are then dropped). */
typedef bool (*esl_solum_stock_send_fn)(void *ctx, const uint8_t *psdu, size_t len);

/* Handles one received stock frame: sends every reply it calls for (a POLL that needs a
 * new image sends one IMG frame per fragment) through send, in order, and returns how
 * many frames went out (0 = not a stock frame, or nothing to send). st and ls are the
 * store and the layout files, for the tag's content; a frame from a tag is noted in the
 * store (esl_store_note_seen) so it is listed like any other; rssi is the AP's signal
 * for it, now_ms the AP's clock. */
unsigned esl_solum_stock_handle(esl_solum_stock_t *sk, esl_store_t *st, esl_layouts_t *ls, uint16_t pan,
                          const uint8_t *psdu, size_t psdu_len, int8_t rssi, uint64_t now_ms,
                          esl_solum_stock_send_fn send, void *ctx);
