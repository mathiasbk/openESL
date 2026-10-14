#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"

/* AP side of the radio protocol in docs/protokoll.md, part 1.
 *
 * The message layout constants belong in shared/proto.h (owned by the tag
 * firmware). Until that file exists they are mirrored here; when it lands,
 * these defines should come from it instead. */

#define ESL_PROTO_VER    1
#define ESL_MSG_POLL     0x01
#define ESL_MSG_IDLE     0x02
#define ESL_MSG_CONTENT  0x03

#define ESL_POLL_LEN        12
#define ESL_IDLE_LEN        4
#define ESL_CONTENT_HDR_LEN 8  /* type, ver, next_poll_s, content_id */
#define ESL_MAX_PAYLOAD     110

#define ESL_POLL_FLAG_BOOT        0x01
#define ESL_POLL_FLAG_DRAW_FAILED 0x02

typedef struct {
    uint16_t fw_ver;
    uint16_t battery_mv;
    uint32_t content_id;
    uint8_t flags;
} esl_poll_t;

/* Accepts payloads longer than 12 bytes so newer tags can append fields. */
bool esl_poll_parse(const uint8_t *p, size_t len, esl_poll_t *out);
size_t esl_poll_build(uint8_t *out, const esl_poll_t *poll); /* for tests and the fake tag */

size_t esl_idle_build(uint8_t *out, uint16_t next_poll_s);
/* out must hold ESL_MAX_PAYLOAD bytes */
size_t esl_content_msg_build(uint8_t *out, uint16_t next_poll_s, uint32_t content_id, const esl_content_t *c);

/* ---- IEEE 802.15.4 MAC header -------------------------------------------
 * Data frame, PAN ID compression, no security, no ACK request.
 *   tag -> AP: dst = short 0x0000, src = 64-bit tag MAC   (FCF 0xC841)
 *   AP -> tag: dst = 64-bit tag MAC, src = short 0x0000   (FCF 0x8C41)
 * Both headers are 15 bytes; FCS (2 bytes) is added by the radio hardware.
 * All address fields are little-endian on air, so MAC 00000123456789A8 is
 * sent as A8 89 67 45 23 01 00 00. */

#define ESL_MHR_LEN    15
#define ESL_FCS_LEN    2
#define ESL_PSDU_MAX   127
#define ESL_FCF_TAG_TO_AP 0xC841
#define ESL_FCF_AP_TO_TAG 0x8C41

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

/* ---- MAC as text: 16 hex digits, e.g. "00000123456789A8" ---------------- */
bool esl_mac_parse(const char *s, size_t len, uint64_t *out);
void esl_mac_format(uint64_t mac, char out[17]);
