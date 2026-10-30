/*
 * proto_tag.h - the tag's side of the radio protocol (docs/protokoll.md part 1).
 * Builds POLL, IMG_REQ, FW_REQ and the surrounding 802.15.4 frame, and parses
 * IDLE/CONFIG/IMG/FW from the AP.
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
#define PROTO_E_FIELD      -4   /* a data block goes outside its image */
#define PROTO_E_FRAME      -6   /* wrong frame control */
#define PROTO_E_PAN        -7   /* different PAN ID */
#define PROTO_E_ADDR       -8   /* not for us, or not from the AP */
#define PROTO_E_LONG       -9   /* longer than an 802.15.4 frame */

/*
 * Parsed reply from the AP. IMG and FW carry one block of an image: size, pos and the
 * data at data_off (an offset into the buffer that was parsed), data_len bytes long.
 */
typedef struct {
    uint8_t type;            /* ESL_MSG_IDLE, ESL_MSG_CONFIG, ESL_MSG_IMG, ESL_MSG_FW or ESL_MSG_PAIR */
    uint16_t next_poll_s;    /* 0 = keep the interval */
    uint8_t data_off;        /* IMG and FW: the block; PAIR: the key */
    uint8_t data_len;
    uint16_t size;           /* IMG and FW: size of the whole image */
    uint16_t pos;            /* IMG and FW: where in it the block starts */
    uint16_t fw_ver;         /* FW only: version, CRC-32 and model of the image */
    uint32_t fw_crc;
    uint8_t fw_model;        /* FW and IMG: model the image is for */
    uint32_t img_id;         /* IMG only: image_id (CRC-32 of the image) and format */
    uint8_t img_fmt;
    uint16_t cfg_ver;        /* CONFIG only, the rest of the settings as in shared/proto.h */
    uint16_t cfg_poll_s;
    uint16_t cfg_max_poll_s;
    uint16_t cfg_lost_ap_s;
    uint16_t cfg_long_poll_s;
    uint16_t cfg_long_after_min;
} proto_reply_t;

/* Writes POLL (ESL_POLL_LEN_LINK bytes: cfg_ver at 12, then the link block) to out, with
   the image_id the tag shows in the content_id field. rx_rssi and rx_lqi are what the
   radio measured on the AP's last answer, lost the rounds without an answer before this
   one, link_flags ESL_POLL_LF_RETRY when the last answered round needed its retry.
   Returns the length. */
uint8_t proto_build_poll(uint8_t *out, uint16_t fw_ver, uint16_t battery_mv,
                         uint32_t shown_id, uint8_t flags, uint16_t cfg_ver,
                         uint8_t rx_rssi, uint8_t rx_lqi, uint8_t lost, uint8_t link_flags);

/* Writes FW_REQ (ESL_FREQ_LEN bytes) to out. Returns the length. */
uint8_t proto_build_fw_req(uint8_t *out, uint16_t fw_ver, uint16_t pos, uint32_t crc, uint8_t max);

/* Writes IMG_REQ (ESL_IREQ_LEN bytes) to out. Returns the length. */
uint8_t proto_build_img_req(uint8_t *out, uint32_t img_id, uint16_t pos, uint8_t max);

/*
 * Parses an IDLE, CONFIG, IMG, FW or PAIR payload (the trailer taken off, sec.h); any other type is PROTO_E_TYPE. Bytes after
 * the settings in CONFIG are ignored (settings a newer AP may add). An IMG or FW block
 * must lie inside the image it belongs to.
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

/* CRC-32 (IEEE, reflected, as esl_crc32 on the AP), continued from crc (start with 0). */
uint32_t proto_crc32(uint32_t crc, const uint8_t *p, uint16_t len);

/* 802.15.4 FCS (CRC-16, polynomial 0x1021, reflected, initial value 0). Used if the radio does not do it. */
uint16_t proto_fcs(const uint8_t *p, uint8_t len);

#endif /* PROTO_TAG_H */
