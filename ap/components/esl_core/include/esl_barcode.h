#pragma once
#include <stddef.h>
#include <stdint.h>
#include "proto.h"

/* Width check for barcode layout elements (ESL_LE_BARCODE in shared/proto.h).
 * The tag encodes and draws the code (tag/src/barcode.c); the AP only needs to
 * know whether a field can be encoded and how wide it gets, so it can reject
 * content the tag would leave blank. The rules are the tag's:
 *   Code 128: ASCII 32-127; a text of 2 or more digits uses code set C (two
 *             digits per symbol, an odd last digit in code set B).
 *   EAN-13:   12 digits, or 13 with a correct check digit.
 * Returns the width in modules including both quiet zones, or -1 if t cannot
 * be encoded. */
int esl_barcode_modules(uint8_t sym, const uint8_t *t, size_t len);

/* "code128", "ean13" */
const char *esl_barcode_name(uint8_t sym);
