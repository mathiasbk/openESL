#pragma once
#include <stddef.h>
#include <stdint.h>

/* ESL_IMG_FMT_2D (shared/proto.h): a raw screen image (ESL_IMG_FMT_RAW, column by
 * column) coded column against column, as docs/protokoll.md part 1 describes it.
 *
 * Codes the n raw bytes at in, columns of h pixels, into out, at most max bytes;
 * n must be a whole number of columns. Returns the coded length, 0 when it does not
 * fit in max (or h is over ESL_IMG_2D_MAX_H). out may be in - margin, margin >= 0
 * (coding in place): a column is read before its code is written, and the coder
 * gives 0 rather than write over a column it has not read. out NULL only counts: the
 * length the code would have, 0 when it is over max. A tag decodes it with the
 * previous and the current column in RAM (tag/src/image.c). Kept free of the rest of
 * esl_core, so the tag's host tests code with it too. */
#define ESL_IMG_2D_MAX_H 512
size_t esl_img_2d_encode(const uint8_t *in, size_t n, uint16_t h, uint8_t *out, size_t max);
/* The length esl_img_2d_encode(in, n, h, in - margin, max) would give, without writing:
 * 0 also when coding in place margin bytes ahead would overtake the image */
size_t esl_img_2d_count(const uint8_t *in, size_t n, uint16_t h, size_t margin, size_t max);
