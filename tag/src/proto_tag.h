/*
 * proto_tag.h - the tag's side of the radio protocol (docs/protokoll.md part 1).
 * Builds POLL and the surrounding 802.15.4 frame, and parses IDLE/CONTENT from the AP.
 * The packet definitions are in shared/proto.h.
 */
#ifndef PROTO_TAG_H
#define PROTO_TAG_H

#include "compat.h"
#include "proto.h"

#define MAC_ADDR_LEN 8

/* Error codes from the parsers (negative) */
#define PROTO_OK            0
#define PROTO_E_SHORT      -1   /* too short */
#define PROTO_E_VERSION    -2   /* unknown proto_ver */
#define PROTO_E_TYPE       -3   /* unknown or unexpected message type */
#define PROTO_E_FIELD      -4   /* a text field goes outside the packet */
#define PROTO_E_LAYOUT     -5   /* unknown layout */
#define PROTO_E_FRAME      -6   /* wrong frame control */
#define PROTO_E_PAN        -7   /* different PAN ID */
#define PROTO_E_ADDR       -8   /* not for us, or not from the AP */
#define PROTO_E_LONG       -9   /* longer than an 802.15.4 frame */

/* Parsed reply from the AP. Field offsets point into the buffer that was parsed. */
typedef struct {
    uint8_t type;            /* ESL_MSG_IDLE or ESL_MSG_CONTENT */
    uint16_t next_poll_s;    /* 0 = keep the interval */
    uint32_t content_id;     /* CONTENT only */
    uint8_t layout;          /* CONTENT only */
    uint8_t flags;           /* CONTENT only */
    uint8_t nfields;
    uint8_t field_off[ESL_MAX_FIELDS];
    uint8_t field_len[ESL_MAX_FIELDS];
} proto_reply_t;

/* Writes POLL (ESL_POLL_LEN bytes) to out. Returns the length. */
uint8_t proto_build_poll(uint8_t *out, uint16_t fw_ver, uint16_t battery_mv,
                         uint32_t content_id, uint8_t flags);

/*
 * Parses an IDLE or CONTENT payload. Fields that are missing get length 0.
 * Bytes after the fourth field are ignored (room for a future MIC).
 */
int8_t proto_parse_reply(const uint8_t *p, uint8_t len, proto_reply_t *r);

/*
 * Builds an 802.15.4 data frame tag -> AP (without FCS) around payload.
 * mac is the tag's 64-bit address in the order it is sent over the air
 * (802.15.4: least significant byte first). Returns the total length, 0 if too long.
 */
uint8_t proto_build_frame(uint8_t *out, uint8_t seq, uint16_t pan, const uint8_t *mac,
                          const uint8_t *payload, uint8_t plen);

/*
 * Checks a received frame (without FCS) from the AP addressed to us.
 * Returns the payload length (>= 0) and sets *payload_off, or an error code.
 */
int8_t proto_check_frame(const uint8_t *f, uint8_t len, uint16_t pan, const uint8_t *mac,
                         uint8_t *payload_off);

/* 802.15.4 FCS (CRC-16, polynomial 0x1021, reflected, initial value 0). Used if the radio does not do it. */
uint16_t proto_fcs(const uint8_t *p, uint8_t len);

#endif /* PROTO_TAG_H */
