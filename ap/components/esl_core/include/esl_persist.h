#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"

/* Blob format for one tag's content in NVS:
 *   ver(1) mac(8, LE) layout(1) flags(1) nfields(1) len[nfields] text */
#define ESL_PERSIST_VER 1
#define ESL_PERSIST_BLOB_MAX (1 + 8 + 3 + ESL_MAX_FIELDS + ESL_MAX_TEXT)

size_t esl_persist_encode(uint64_t mac, const esl_content_t *c, uint8_t *out);
/* Rejects unknown versions and anything esl_content_add_field would reject. */
bool esl_persist_decode(const uint8_t *in, size_t len, uint64_t *mac, esl_content_t *c);
