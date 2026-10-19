#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"

/* AP side of the radio protocol in docs/protokoll.md, part 1, including the
 * layout transfer (LAYOUT / LAYOUT_REQ).
 *
 * The message layout comes from shared/proto.h, which the tag firmware uses
 * too. The AP-side names below are aliases for it. */
#include "proto.h"

#define ESL_POLL_FLAG_BOOT        ESL_POLL_F_BOOTED
#define ESL_POLL_FLAG_DRAW_FAILED ESL_POLL_F_DRAWFAIL
#define ESL_POLL_FLAG_LAYOUT_FAILED ESL_POLL_F_LAYOUTFAIL

typedef struct {
    uint16_t fw_ver;
    uint16_t battery_mv;
    uint32_t content_id;
    uint8_t flags;
    uint8_t model;       /* ESL_MODEL_*; old firmware sends 0 (unknown) */
    bool has_cfg_ver;    /* the tag understands CONFIG (POLL of ESL_POLL_LEN_CFG bytes or more) */
    uint16_t cfg_ver;    /* the settings the tag uses now; 0 = its built-in defaults */
    bool has_layout_ver; /* POLL of 18 bytes or more: the tag can download layout files */
    uint32_t layout_ver; /* its layout file version, 0 = none */
} esl_poll_t;

/* Tag settings carried by CONFIG. 0 in a field = the tag's built-in default. */
typedef struct {
    uint16_t poll_s;     /* normal interval (ESL_CFG_POLL_MIN_S..ESL_CFG_POLL_MAX_S) */
    uint16_t max_poll_s; /* longest interval while the AP does not answer (same range) */
    uint16_t lost_ap_s;  /* "No AP found" after this long without an answer (ESL_CFG_LOST_AP_MIN_S..,
                          * ESL_CFG_LOST_AP_NEVER = never) */
} esl_tag_cfg_t;

/* Version of a set of settings as sent in CONFIG and echoed in POLL: 0 when
 * every field is 0 (what a tag uses after boot), otherwise a nonzero 16-bit
 * hash of the fields. Derived from the values only, so it survives AP restarts
 * and needs no counter in NVS. */
uint16_t esl_tag_cfg_version(const esl_tag_cfg_t *c);
bool esl_tag_cfg_is_default(const esl_tag_cfg_t *c);

/* Accepts payloads longer than 12 bytes so newer tags can append fields:
 * cfg_ver (bytes 12-13) is read from a POLL of at least 14 bytes, the layout
 * version (bytes 14-17) from one of at least 18. */
bool esl_poll_parse(const uint8_t *p, size_t len, esl_poll_t *out);
/* For tests and the fake tag: 18 bytes if has_layout_ver (cfg_ver at 12-13, 0 unless
 * has_cfg_ver), 14 if only has_cfg_ver, else the old 12. */
size_t esl_poll_build(uint8_t *out, const esl_poll_t *poll);

/* LAYOUT_REQ (tag -> AP) */
typedef struct {
    uint8_t model;
    uint8_t max;     /* data bytes the tag wants at most */
    uint32_t layout_ver;
    uint16_t pos;
} esl_layout_req_t;

bool esl_layout_req_parse(const uint8_t *p, size_t len, esl_layout_req_t *out);
size_t esl_layout_req_build(uint8_t *out, const esl_layout_req_t *req); /* for the fake tag */

/* LAYOUT (AP -> tag): data (n bytes) is the block at pos of a file of size
 * bytes with version ver. out must hold ESL_MAX_PAYLOAD bytes. */
size_t esl_layout_msg_build(uint8_t *out, uint16_t next_poll_s, uint32_t ver, uint16_t size, uint16_t pos,
                            const uint8_t *data, size_t n);

/* FW_REQ (tag -> AP, firmware update; docs/tag-ota.md) */
typedef struct {
    uint8_t model;
    uint8_t max;     /* data bytes the tag wants at most */
    uint16_t ver;    /* the image it is downloading */
    uint16_t pos;
    uint32_t crc;    /* CRC-32 of that image */
} esl_fw_req_t;

bool esl_fw_req_parse(const uint8_t *p, size_t len, esl_fw_req_t *out);
size_t esl_fw_req_build(uint8_t *out, const esl_fw_req_t *req); /* for the fake tag */

/* FW (AP -> tag): data (n bytes, at most ESL_FW_CHUNK_MAX) is the block at pos of an
 * image of size bytes for model, version ver, CRC-32 crc. out holds ESL_MAX_PAYLOAD bytes. */
size_t esl_fw_msg_build(uint8_t *out, uint16_t next_poll_s, uint16_t ver, uint16_t size, uint16_t pos,
                        uint32_t crc, uint8_t model, const uint8_t *data, size_t n);

size_t esl_idle_build(uint8_t *out, uint16_t next_poll_s);
/* out must hold ESL_MAX_PAYLOAD bytes */
size_t esl_content_msg_build(uint8_t *out, uint16_t next_poll_s, uint32_t content_id, const esl_content_t *c);
/* CONFIG with c and its version (ESL_CONFIG_LEN bytes). */
size_t esl_config_msg_build(uint8_t *out, uint16_t next_poll_s, const esl_tag_cfg_t *c);

/* An IDLE, CONTENT, LAYOUT, CONFIG or FW message as the tag reads it (for the fake tag and
 * the tests, so what the AP builds is checked by an independent parser). */
typedef struct {
    uint8_t type; /* ESL_MSG_IDLE, ESL_MSG_CONTENT, ESL_MSG_LAYOUT, ESL_MSG_CONFIG or ESL_MSG_FW */
    uint16_t next_poll_s;
    uint32_t content_id;   /* CONTENT only */
    esl_content_t content; /* CONTENT only */
    uint32_t layout_ver;   /* LAYOUT only */
    uint16_t layout_size;  /* LAYOUT and FW: size, position and data of the block */
    uint16_t layout_pos;
    const uint8_t *layout_data; /* points into p */
    size_t layout_len;
    uint16_t fw_ver;       /* FW only */
    uint32_t fw_crc;
    uint8_t fw_model;
    uint16_t cfg_ver;     /* CONFIG only */
    esl_tag_cfg_t cfg;    /* CONFIG only */
} esl_reply_t;

/* Rejects unknown types/versions, truncated messages, bad field lengths,
 * LAYOUT and FW blocks outside their file and trailing bytes (except after CONFIG,
 * where newer APs may append settings). */
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

/* ---- tag model: "2.9" / "1.54" as used in the API ----------------------
 * The models and ESL_MODEL_COUNT come from shared/models.json (shared/models_gen.h). */
/* Name of a known model, NULL for ESL_MODEL_UNKNOWN or anything else. */
const char *esl_model_name(uint8_t model);
/* Screen size of a known model in pixels (the layout coordinates); false otherwise. */
bool esl_model_size(uint8_t model, uint16_t *w, uint16_t *h);
/* True if the model's panel has red (colors in shared/models.json); false for an unknown model. */
bool esl_model_has_red(uint8_t model);
/* ESL_MODEL_UNKNOWN if the name is not a known model. */
uint8_t esl_model_parse(const char *s, size_t len);
