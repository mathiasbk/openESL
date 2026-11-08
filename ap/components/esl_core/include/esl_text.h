#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Convert UTF-8 to Latin-1 (ISO 8859-1) for the tag font.
 *
 * - Code points U+00A1..U+00FF are copied as-is (æøåÆØÅ etc.).
 * - U+00A0 (no-break space) and thin/narrow spaces become ' '.
 * - Dashes (U+2010..U+2015, U+2212) become '-', curly quotes become ' or ".
 * - Control characters, U+007F..U+009F, invalid UTF-8 and everything else
 *   becomes '?'.
 *
 * Writes at most out_max bytes. Returns the number of bytes written; sets
 * *overflow (if non-NULL) when the input did not fit. */
size_t esl_utf8_to_latin1(const char *in, size_t in_len, uint8_t *out, size_t out_max, bool *overflow);

/* Writes the number in in (Latin-1, e.g. "1299.9", "1299,90", "1 299,90", "199,-") as
 * ESL_LE_NUM_* in opts asks (shared/proto.h): decimals, a decimal comma and a space as
 * the thousands separator, or with esl_number_set_point a decimal point and a comma
 * (1,299.90; 199.-). Returns the length written to out (cap bytes, at least
 * ESL_LE_NUM_MAX_LEN), or 0 when opts asks for no format or in is not a number. */
size_t esl_format_number(const uint8_t *in, size_t len, uint8_t opts, uint8_t *out, size_t cap);

/* The decimal sign esl_format_number writes (#402): a point when the AP's "lang" is set to
 * "en", a comma otherwise (not set, as before 0.11.0, or "nb"). Set from outside as the
 * language of the date fields is (main/sched.c on the AP, web_set_number_point on the web
 * page). A comma until set. */
void esl_number_set_point(bool point);
bool esl_number_point(void);
