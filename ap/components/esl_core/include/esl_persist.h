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
