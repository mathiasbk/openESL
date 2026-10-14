#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Small JSON tokenizer and writer, enough for the AP's HTTP API.
 *
 * The parser fills a flat token array (no allocation of its own). Each token
 * records where it is in the input, how many children it has (key/value pairs
 * for objects, items for arrays) and the index of the token after its whole
 * subtree, so callers can skip over values. */

typedef enum {
    ESL_JSON_OBJECT = 1,
    ESL_JSON_ARRAY,
    ESL_JSON_STRING,
    ESL_JSON_NUMBER,
    ESL_JSON_TRUE,
    ESL_JSON_FALSE,
    ESL_JSON_NULL,
} esl_json_type_t;

typedef struct {
    uint8_t type;
    uint16_t start; /* strings: first byte after the opening quote */
    uint16_t end;   /* strings: the closing quote */
    uint16_t size;  /* objects: pairs, arrays: items */
    uint16_t next;  /* index of the token after this subtree */
} esl_jtok_t;

#define ESL_JSON_MAX_LEN   65535
#define ESL_JSON_MAX_DEPTH 16

#define ESL_JSON_ERR_SYNTAX  (-1)
#define ESL_JSON_ERR_NOMEM   (-2)
#define ESL_JSON_ERR_DEPTH   (-3)
#define ESL_JSON_ERR_TOO_BIG (-4)

/* Returns the number of tokens (token 0 is the root) or a negative error. */
int esl_json_parse(const char *js, size_t len, esl_jtok_t *tok, unsigned max_tokens);

/* Index of the value for key in object obj, or -1. Keys are compared raw. */
int esl_json_obj_get(const char *js, const esl_jtok_t *tok, int obj, const char *key);

bool esl_json_str_eq(const char *js, const esl_jtok_t *t, const char *s);

/* Unescapes a string token into UTF-8. Returns the length, -1 for a bad
 * escape or -2 if out_max is too small. Does not NUL-terminate. */
int esl_json_str_decode(const char *js, const esl_jtok_t *t, char *out, size_t out_max);

/* ---- writer ------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} esl_sb_t;

/* buf is always NUL-terminated; at most cap-1 characters are stored. */
void esl_sb_init(esl_sb_t *sb, char *buf, size_t cap);
void esl_sb_putc(esl_sb_t *sb, char c);
void esl_sb_puts(esl_sb_t *sb, const char *s);
void esl_sb_printf(esl_sb_t *sb, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* Writes a quoted JSON string from Latin-1 bytes (converted to UTF-8). */
void esl_sb_json_latin1(esl_sb_t *sb, const uint8_t *s, size_t len);
/* Writes a quoted JSON string from a NUL-terminated ASCII/UTF-8 string. */
void esl_sb_json_str(esl_sb_t *sb, const char *s);
