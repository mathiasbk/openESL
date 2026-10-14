#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"

/* AP side of the radio protocol in docs/protokoll.md, part 1.
 *
 * The message layout comes from shared/proto.h, which the tag firmware uses
 * too. The AP-side names below are aliases for it. */
#include "proto.h"

#define ESL_POLL_FLAG_BOOT        ESL_POLL_F_BOOTED
#define ESL_POLL_FLAG_DRAW_FAILED ESL_POLL_F_DRAWFAIL

typedef struct {
    uint16_t fw_ver;
    uint16_t battery_mv;
    uint32_t content_id;
    uint8_t flags;
    uint8_t model; /* ESL_MODEL_*; old firmware sends 0 (unknown) */
} esl_poll_t;

/* Accepts payloads longer than 12 bytes so newer tags can append fields. */
bool esl_poll_parse(const uint8_t *p, size_t len, esl_poll_t *out);
size_t esl_poll_build(uint8_t *out, const esl_poll_t *poll); /* for tests and the fake tag */

size_t esl_idle_build(uint8_t *out, uint16_t next_poll_s);
/* out must hold ESL_MAX_PAYLOAD bytes */
size_t esl_content_msg_build(uint8_t *out, uint16_t next_poll_s, uint32_t content_id, const esl_content_t *c);

/* An IDLE or CONTENT message as the tag reads it (for the fake tag and the
 * tests, so what the AP builds is checked by an independent parser). */
typedef struct {
    uint8_t type; /* ESL_MSG_IDLE or ESL_MSG_CONTENT */
    uint16_t next_poll_s;
    uint32_t content_id;  /* CONTENT only */
    esl_content_t content; /* CONTENT only */
} esl_reply_t;

/* Rejects unknown types/versions, truncated messages, bad field lengths and
 * trailing bytes. */
bool esl_reply_parse(const uint8_t *p, size_t len, esl_reply_t *out);

/* ---- IEEE 802.15.4 MAC header -------------------------------------------
 * Data frame, PAN ID compression, no security, no ACK request.
 *   tag -> AP: dst = short 0x0000, src = 64-bit tag MAC   (FCF 0xC841)
 *   AP -> tag: dst = 64-bit tag MAC, src = short 0x0000   (FCF 0x8C41)
 * Both headers are 15 bytes; FCS (2 bytes) is added by the radio hardware.
 * All address fields are little-endian on air, so MAC 00000123456789A8 is
 * sent as A8 89 67 45 23 01 00 00. */

#define ESL_MHR_LEN       ESL_MAC_HDR_LEN
#define ESL_FCS_LEN       ESL_MAC_FCS_LEN
#define ESL_PSDU_MAX      ESL_MAC_MAX_FRAME
#define ESL_FCF_TAG_TO_AP ESL_FC_TAG_TO_AP
#define ESL_FCF_AP_TO_TAG ESL_FC_AP_TO_TAG

typedef struct {
    uint8_t seq;
    uint16_t dst_pan;
    uint16_t dst_short;
    uint64_t src_mac;
    const uint8_t *payload;
    size_t payload_len;
} esl_rx_frame_t;

/* psdu = MAC header + payload + FCS, psdu_len includes the FCS. Returns false
 * if the frame is not a tag->AP data frame. Does not check PAN/address. */
bool esl_frame_parse_from_tag(const uint8_t *psdu, size_t psdu_len, esl_rx_frame_t *out);

/* Builds header + payload into out (ESL_PSDU_MAX bytes) and leaves room for
 * the FCS. Returns the PSDU length including FCS, or 0 if it does not fit.
 * The tag's sequence number is echoed so it can match the reply. */
size_t esl_frame_build_to_tag(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t src_short,
                              uint64_t dst_mac, const uint8_t *payload, size_t payload_len);

/* Builds a tag->AP frame (for tests and the fake tag). */
size_t esl_frame_build_from_tag(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t dst_short,
                                uint64_t src_mac, const uint8_t *payload, size_t payload_len);

typedef struct {
    uint8_t seq;
    uint16_t dst_pan;
    uint64_t dst_mac;
    uint16_t src_short;
    const uint8_t *payload;
    size_t payload_len;
} esl_ap_frame_t;

/* Parses an AP->tag frame (for tests and the fake tag). Same rules as
 * esl_frame_parse_from_tag. */
bool esl_frame_parse_to_tag(const uint8_t *psdu, size_t psdu_len, esl_ap_frame_t *out);

/* ---- MAC as text: 16 hex digits, e.g. "00000123456789A8" ---------------- */
bool esl_mac_parse(const char *s, size_t len, uint64_t *out);
void esl_mac_format(uint64_t mac, char out[17]);

/* ---- tag model: "2.9" / "1.54" as used in the API ---------------------- */
#define ESL_MODEL_COUNT 3 /* ESL_MODEL_UNKNOWN .. ESL_MODEL_154 */
/* Name of a known model, NULL for ESL_MODEL_UNKNOWN or anything else. */
const char *esl_model_name(uint8_t model);
/* ESL_MODEL_UNKNOWN if the name is not a known model. */
uint8_t esl_model_parse(const char *s, size_t len);
