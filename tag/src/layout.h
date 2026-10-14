/*
 * layout.h - places the text fields from a CONTENT packet on the screen.
 *
 * Layout 1 "price":  name (1-2 lines, medium font), price (large font, right-aligned,
 *                    black or red), one subtitle (small font, bottom left).
 * Layout 2 "text":   up to 4 lines in the medium font.
 * Long text is truncated with "…". The text pointed to must live until drawing is done.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include "compat.h"
#include "draw.h"
#include "proto_tag.h"

#define LAYOUT_MARGIN     6    /* left/right margin */
#define LAYOUT_FRAME_W    3    /* thickness of the red frame (flag bit1) */

/* Builds the scene from a parsed CONTENT packet. buf is the buffer the packet was parsed from. */
void layout_build(draw_scene_t *s, const proto_reply_t *c, const uint8_t *buf);

/* Builds a simple screen with up to 4 lines (start-up, "waiting for AP"). */
void layout_build_lines(draw_scene_t *s, const uint8_t *const *lines, const uint8_t *lens, uint8_t n);

/*
 * Helpers, exported for the tests.
 * layout_fit: how many characters of s fit in maxw. *ellipsis = 1 if the text
 *             was truncated and "…" should be drawn after it (its width is then subtracted).
 * layout_wrap: splits the text into two lines at the last space that fits.
 *             Returns the length of line 1; *next is the index where line 2 starts.
 */
uint8_t layout_fit(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *ellipsis);
uint8_t layout_wrap(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *next);

#endif /* LAYOUT_H */
