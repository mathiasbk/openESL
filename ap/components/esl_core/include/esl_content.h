#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What one tag should show: layout, flags and up to ESL_MAX_FIELDS Latin-1 text fields.
 * The HTTP API takes and gives UTF-8; esl_text converts it to the fonts' Latin-1.
 * The AP draws it into the tag's image (esl_render, esl_image). */

#include "proto.h" /* shared/proto.h */

/* ESL_LAYOUT_PRICE: [name, price, subtitle]
 * ESL_LAYOUT_TEXT:  [line1, line2, line3, line4]
 * ESL_LAYOUT_FILE + n: layout n of the model's layout file, up to ESL_MAX_FIELDS fields
 * ESL_LAYOUT_NAMED: what the API stores for a layout from the layout file. It
 *   names the layout; esl_render looks it up in the file for the tag's model
 *   (docs/protokoll.md part 3). */
#define ESL_LAYOUT_NAMED 0xFF
#define ESL_LAYOUT_NAME_MAX 31
/* ESL_LAYOUT_IMAGE: a picture a client sent (PUT /api/tags/{mac}/image), shown as it is.
 * The picture itself is kept apart (esl_store ops.persist_image, esl_image's stored
 * form); the content's one field is the model it is for and the CRC-32 of the stored
 * picture, so a new picture gives a new content_id. "image" in the API. */
#define ESL_LAYOUT_IMAGE 0xFE

#define ESL_CFLAG_PRICE_HIGHLIGHT ESL_CF_PRICE_HIGHLIGHT
#define ESL_CFLAG_FRAME ESL_CF_FRAME

/* Text and one length byte per field share ESL_CONTENT_FIELD_SPACE (256) bytes: 255
 * bytes of text with one field, 250 with six, 240 with sixteen. Up to 0.9.2 it was 96 bytes
 * and 6 fields (the CONTENT message the tags used to get); content within those has the
 * same body and content_id as then (#182). ESL_MAX_TEXT is the most text any content can
 * hold, and the most one field can (a length byte). */
#define ESL_MAX_TEXT ESL_MAX_TEXT_FOR(1)
/* layout + name length + name + flags + (len + text) per field */
#define ESL_CONTENT_BODY_MAX (2 + 1 + ESL_LAYOUT_NAME_MAX + ESL_MAX_FIELDS + ESL_MAX_TEXT)

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
    char name[ESL_LAYOUT_NAME_MAX + 1]; /* ESL_LAYOUT_NAMED: the layout name, NUL-terminated */
} esl_content_t;

/* Max number of fields for a layout, 0 for an unknown layout. */
int esl_content_max_fields(uint8_t layout);

/* For ESL_LAYOUT_PRICE, ESL_LAYOUT_TEXT and ESL_LAYOUT_FILE + n. */
esl_content_err_t esl_content_init(esl_content_t *c, uint8_t layout, uint8_t flags);
/* For a layout from the layout file, by name (see esl_layout_name_valid). */
esl_content_err_t esl_content_init_named(esl_content_t *c, const char *name, size_t len, uint8_t flags);
/* For a picture (ESL_LAYOUT_IMAGE) for a tag of model; crc is the CRC-32 of its stored form. */
void esl_content_init_image(esl_content_t *c, uint8_t model, uint32_t crc);
/* True for a picture, with its model and CRC. */
bool esl_content_image(const esl_content_t *c, uint8_t *model, uint32_t *crc);

/* A layout name in a layout file: 1..31 of a-z 0-9 . _ -, starting with a
 * letter or digit, and not "price", "text" (the built-in layouts) or "image" (a picture). */
bool esl_layout_name_valid(const char *s, size_t len);

esl_content_err_t esl_content_add_field(esl_content_t *c, const uint8_t *latin1, size_t len);

/* Drops trailing empty fields, so ["a", ""] and ["a"] get the same content_id. */
void esl_content_trim(esl_content_t *c);

const uint8_t *esl_content_field(const esl_content_t *c, unsigned idx, uint8_t *len);
size_t esl_content_text_len(const esl_content_t *c);

/* Serialises layout, flags and the fields as the CONTENT message had them
 * (from the layout byte on), so content_id is the same as before. A named
 * layout gives ESL_LAYOUT_NAMED, the name length and the name in place of the
 * layout byte. out must hold ESL_CONTENT_BODY_MAX bytes. Returns the length. */
size_t esl_content_body(const esl_content_t *c, uint8_t *out);

/* CRC32 of the body. Never 0, since 0 means "no content". For a
 * named layout it covers the name, not the index, so it is the same for every
 * model and does not change when the layout file does. */
uint32_t esl_content_id(const esl_content_t *c);

bool esl_content_equal(const esl_content_t *a, const esl_content_t *b);

/* A short stand-in for a layout name (never 0), so the tag table can tell whether
 * the layout file has a content's layout without keeping the name. */
uint32_t esl_layout_name_hash(const char *name);
/* esl_layout_name_hash of c's layout name, 0 for a built-in layout. */
uint32_t esl_content_layout_hash(const esl_content_t *c);
