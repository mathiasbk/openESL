#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* QR codes (ISO/IEC 18004) for the qr layout element, written from the standard: byte mode
 * (ISO 8859-1, which is what the fields are in), versions 1 to ESL_QR_VERSION_MAX, the
 * four error correction levels, and the mask with the lowest penalty. */

#define ESL_QR_VERSION_MAX 10
#define ESL_QR_SIZE(v)     (17 + 4 * (v))              /* modules per side */
#define ESL_QR_SIZE_MAX    ESL_QR_SIZE(ESL_QR_VERSION_MAX)
#define ESL_QR_BYTES       ((ESL_QR_SIZE_MAX * ESL_QR_SIZE_MAX + 7) / 8)

#define ESL_QR_ECC_L 0 /* about 7 % of the code can be lost */
#define ESL_QR_ECC_M 1 /* 15 % */
#define ESL_QR_ECC_Q 2 /* 25 % */
#define ESL_QR_ECC_H 3 /* 30 % */

/* The smallest version that holds len bytes at level ecc, or 0 if none up to max does. */
uint8_t esl_qr_version(size_t len, uint8_t ecc, uint8_t max);

/* The most bytes version v holds at level ecc. */
size_t esl_qr_capacity(uint8_t v, uint8_t ecc);

/* Encodes len bytes at level ecc in the smallest version up to max. The modules go into
 * bits, row by row from the top, bit 7 of byte 0 the top-left module, 1 = dark; no quiet
 * zone. Returns the side in modules, 0 if the text does not fit. */
uint8_t esl_qr_encode(const uint8_t *text, size_t len, uint8_t ecc, uint8_t max, uint8_t bits[ESL_QR_BYTES]);

/* Module (x, y) of a code of side size made by esl_qr_encode */
static inline bool esl_qr_module(const uint8_t *bits, uint8_t size, unsigned x, unsigned y)
{
    unsigned i = y * size + x;
    return (bits[i >> 3] & (0x80 >> (i & 7))) != 0;
}
