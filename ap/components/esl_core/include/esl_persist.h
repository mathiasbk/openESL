#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"
#include "esl_proto.h"

/* Blob format for one tag's content in NVS:
 *   ver 1: ver(1) mac(8, LE) layout(1) flags(1) nfields(1) len[nfields] text
 *   ver 2: the same, with name_len(1) name after layout (= ESL_LAYOUT_NAMED);
 *          only written for a layout from the layout file, so content with a
 *          built-in layout is stored exactly as before. */
#define ESL_PERSIST_VER 1
#define ESL_PERSIST_VER_NAMED 2
#define ESL_PERSIST_BLOB_MAX (1 + 8 + 3 + 1 + ESL_LAYOUT_NAME_MAX + ESL_MAX_FIELDS + ESL_MAX_TEXT)

size_t esl_persist_encode(uint64_t mac, const esl_content_t *c, uint8_t *out);
/* Rejects unknown versions and anything esl_content_add_field would reject. */
bool esl_persist_decode(const uint8_t *in, size_t len, uint64_t *mac, esl_content_t *c);

/* Blob format for one tag's settings (esl_tag_cfg_t) in NVS:
 *   ver(1) mac(8, LE) poll_s(2) max_poll_s(2) lost_ap_s(2), all LE */
#define ESL_PERSIST_CFG_VER 1
#define ESL_PERSIST_CFG_LEN (1 + 8 + 6)

size_t esl_persist_cfg_encode(uint64_t mac, const esl_tag_cfg_t *cfg, uint8_t *out);
bool esl_persist_cfg_decode(const uint8_t *in, size_t len, uint64_t *mac, esl_tag_cfg_t *cfg);

/* Blob format for what the AP last heard from a tag, so a tag that only polls
 * (no content, no settings) is not forgotten across a restart or an OTA update,
 * and a tag that has gone quiet still shows when it was last heard:
 *   ver(1) mac(8, LE) model(1) fw(2) battery_mv(2) rssi(1) unix_s(4), all LE
 * unix_s is the wall-clock second of its last POLL as last saved, 0 = unknown. */
#define ESL_PERSIST_SEEN_VER 1
#define ESL_PERSIST_SEEN_LEN (1 + 8 + 1 + 2 + 2 + 1 + 4)

typedef struct {
    uint8_t model;
    uint16_t fw;
    uint16_t battery_mv;
    int8_t rssi;
    uint32_t unix_s;
} esl_seen_t;

size_t esl_persist_seen_encode(uint64_t mac, const esl_seen_t *s, uint8_t *out);
bool esl_persist_seen_decode(const uint8_t *in, size_t len, uint64_t *mac, esl_seen_t *s);
