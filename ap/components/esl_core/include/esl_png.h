#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A PNG decoder for pictures a client sends the AP (PUT /api/tags/{mac}/image),
 * written from the PNG specification (W3C, ISO/IEC 15948), zlib (RFC 1950) and
 * deflate (RFC 1951).
 *
 * It streams: the file is pulled through io->read as it is needed and handed on
 * one row at a time, so a picture never has to be in RAM whole, only the deflate
 * window (32 KB) and two rows. Every colour type and bit depth is read, with
 * transparency (tRNS, alpha) laid over white; interlaced files (Adam7) are not.
 * Chunk CRCs and the zlib Adler-32 are checked. */

typedef enum {
    ESL_PNG_OK = 0,
    ESL_PNG_ERR_READ,       /* io->read failed: the body stopped arriving */
    ESL_PNG_ERR_NOT_PNG,    /* no PNG signature */
    ESL_PNG_ERR_BAD,        /* a broken file: bad chunk, CRC, zlib or deflate data */
    ESL_PNG_ERR_UNSUPPORTED,/* interlaced, or a colour type and bit depth PNG does not have */
    ESL_PNG_ERR_SIZE,       /* io->header refused the size */
    ESL_PNG_ERR_NOMEM,
    ESL_PNG_ERR_STOPPED,    /* io->row returned false */
} esl_png_err_t;

typedef struct {
    void *ctx;
    /* Exactly n bytes of the file into buf; false when they do not come. */
    bool (*read)(void *ctx, uint8_t *buf, size_t n);
    /* The picture's size, before any row: false refuses it (ESL_PNG_ERR_SIZE). */
    bool (*header)(void *ctx, uint32_t width, uint32_t height);
    /* Row y (from the top), width pixels of R, G, B (8 bits each), transparency
       already laid over white. False stops the decoding. */
    bool (*row)(void *ctx, uint32_t y, const uint8_t *rgb);
} esl_png_io_t;

/* The 8 bytes every PNG file starts with */
extern const uint8_t esl_png_signature[8];

/* Pictures wider than this are refused before anything is allocated */
#define ESL_PNG_MAX_WIDTH 4096

/* Decodes one PNG file. Allocates about 32 KB plus a few rows, freed again. */
esl_png_err_t esl_png_decode(const esl_png_io_t *io);

/* Text for an error, for the API */
const char *esl_png_err_text(esl_png_err_t err);
