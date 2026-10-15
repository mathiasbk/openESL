/*
 * barcode.h - Code 128 and EAN-13 as a row of modules (bars and spaces), for the
 * barcode layout element (ESL_LE_BARCODE in shared/proto.h).
 *
 * The encodings follow the public standards (ISO/IEC 15417 for Code 128, ISO/IEC 15420
 * for EAN-13). Code 128 uses code set C for runs of digits (whole text of digits, an odd
 * last digit in code set B) and code set B for everything else, ASCII 32-127 only.
 */
#ifndef BARCODE_H
#define BARCODE_H

#include "compat.h"
#include "proto.h"

/*
 * Encodes t[0..len) with symbology sym (ESL_BC_*) into bits: module i is bit 7 - i % 8
 * of bits[i / 8], 1 = black. The quiet zones are not included. Returns the number of
 * modules, or 0 if t cannot be encoded or needs more than max modules.
 */
uint16_t barcode_encode(uint8_t sym, const uint8_t *t, uint8_t len, uint8_t *bits, uint16_t max);

/* Quiet zone left and right of the code, in modules. */
uint8_t barcode_quiet_left(uint8_t sym);
uint8_t barcode_quiet_right(uint8_t sym);

#endif /* BARCODE_H */
