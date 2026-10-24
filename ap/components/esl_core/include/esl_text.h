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
 * the thousands separator. Returns the length written to out (cap bytes, at least
 * ESL_LE_NUM_MAX_LEN), or 0 when opts asks for no format or in is not a number. */
size_t esl_format_number(const uint8_t *in, size_t len, uint8_t opts, uint8_t *out, size_t cap);
