#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"
#include "esl_layout.h"

/* Draws a tag's screen on the AP: the content (layout, flags and fields) placed with
 * the model's built-in layouts or one from its compiled layout file, into a 1-bit
 * image in screen coordinates, column by column as the tag's draw_column() gives it
 * (docs/protokoll.md, "Image transfer"); the tag's epd layer rotates, transposes and
 * inverts it for its panel.
 *
 * One drawing for every model: the tag's fonts and column format, with the fitting and
 * wrapping the tags used when they drew content themselves (sim/reference/ holds the
 * CRC of every screen, so a change shows up there).
 *
 * Image format: the black plane, then the red plane on a model with red. A plane is
 * width columns from left to right, each ESL_RENDER_COL_BYTES(height) bytes: byte 0
 * MSB is row 0 (the top), then downwards; bit 1 = ink in that plane, and a pixel is
 * never set in both planes. Unused bits at the end of the last byte are 0.
 *
 * Not reentrant: the scene is static (about 2 KB, and esl_qr.c keeps 3.3 KB for one code),
 * so calls must not overlap (the AP's callers hold the layouts lock while drawing, which
 * also keeps file valid).
 * Screens up to 256 rows; a taller model gives 0. */

#define ESL_RENDER_COL_BYTES(h) (((h) + 7) / 8)

/* Width, height and whether the model has red; false for an unknown model. */
bool esl_render_model(uint8_t model, uint16_t *w, uint16_t *h, bool *has_red);

/* Bytes of the image for a model (every plane), 0 for an unknown model. */
size_t esl_render_size(uint8_t model);

/* Draws content c for a tag of model into out (cap bytes). file is the model's
 * compiled layout file, or NULL when it has none. A layout number the file does not
 * have (0x10 + n) is drawn with the built-in "text" layout, as the tag does; a named
 * layout the file does not have (or with no file) is not drawn, since the AP does not
 * send such content (content_err). Returns the image size, or 0 for an unknown model,
 * a missing named layout, or when cap is too small. */
size_t esl_render(uint8_t model, const esl_layout_bin_t *file, const esl_content_t *c, uint8_t *out, size_t cap);
