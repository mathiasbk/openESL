#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What one tag should show: layout, flags and up to four Latin-1 text fields.
 * See docs/protokoll.md, CONTENT message. */

#include "proto.h" /* shared/proto.h */

/* ESL_LAYOUT_PRICE: [name, price, subtitle]
 * ESL_LAYOUT_TEXT:  [line1, line2, line3, line4] */

#define ESL_CFLAG_PRICE_HIGHLIGHT ESL_CF_PRICE_HIGHLIGHT
#define ESL_CFLAG_FRAME ESL_CF_FRAME

/* Total text bytes (after Latin-1 conversion): what fits in the 110-byte
 * radio payload with all four length bytes and the reserved 4-byte MIC. */
#define ESL_MAX_TEXT ESL_MAX_TEXT_BYTES
/* layout + flags + (len + text) per field */
#define ESL_CONTENT_BODY_MAX (2 + ESL_MAX_FIELDS + ESL_MAX_TEXT)

typedef enum {
    ESL_CONTENT_OK = 0,
    ESL_CONTENT_ERR_LAYOUT,
    ESL_CONTENT_ERR_TOO_MANY_FIELDS,
    ESL_CONTENT_ERR_TOO_LONG,
} esl_content_err_t;

typedef struct {
    uint8_t layout;
    uint8_t flags;
    uint8_t nfields;
    uint8_t len[ESL_MAX_FIELDS];
    uint8_t text[ESL_MAX_TEXT]; /* fields stored back to back */
} esl_content_t;

/* Max number of fields for a layout, 0 for an unknown layout. */
int esl_content_max_fields(uint8_t layout);

esl_content_err_t esl_content_init(esl_content_t *c, uint8_t layout, uint8_t flags);
esl_content_err_t esl_content_add_field(esl_content_t *c, const uint8_t *latin1, size_t len);

/* Drops trailing empty fields, so ["a", ""] and ["a"] get the same content_id. */
void esl_content_trim(esl_content_t *c);

const uint8_t *esl_content_field(const esl_content_t *c, unsigned idx, uint8_t *len);
size_t esl_content_text_len(const esl_content_t *c);

/* Serialises layout, flags and the fields exactly as they appear in the
 * CONTENT message (from the layout byte on). out must hold
 * ESL_CONTENT_BODY_MAX bytes. Returns the length. */
size_t esl_content_body(const esl_content_t *c, uint8_t *out);

/* CRC32 of the body. Never 0, since 0 means "no content" on the radio. */
uint32_t esl_content_id(const esl_content_t *c);

bool esl_content_equal(const esl_content_t *a, const esl_content_t *b);
