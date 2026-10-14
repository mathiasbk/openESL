/*
 * layout.h - places the text fields from a CONTENT packet on the screen.
 *
 * A layout is a list of elements (layout_elem_t) ending with LE_END. The built-in
 * layouts "price" and "text" are such lists in flash, one set per model (layout.c).
 * Requirement G7/T17 (layouts from a file sent by the AP) can later feed a list
 * received over radio into the same layout_apply(); the format below is a proposal
 * for that, the radio message is not agreed yet (docs/protokoll.md part 3).
 *
 * Built-in layouts (2.9", the 1.54" has the same elements in a 152 x 152 box):
 *   1 "price": name (medium font, 2 lines; small font, 3 lines, if it does not fit),
 *              price right-aligned (large font, then mid, then medium; red if flag bit0),
 *              one subtitle (small font, bottom left).
 *   2 "text":  4 lines, each in the medium font, or the small font if it does not fit.
 * Flag bit1 gives a red frame around the whole screen.
 * Text that does not fit even in the last font is truncated with "…".
 * The text pointed to must live until drawing is done.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include "compat.h"
#include "draw.h"
#include "proto_tag.h"

#define LAYOUT_FRAME_W    3    /* thickness of the red frame (flag bit1) */
#if DRAW_W >= 200
#define LAYOUT_MARGIN     6    /* left/right margin */
#else
#define LAYOUT_MARGIN     5
#endif

#define LAYOUT_MAX_FONTS  3    /* fonts to try per text element */
#define LAYOUT_MAX_LINES  3    /* lines per text element */

/* Element kinds */
#define LE_END        0
#define LE_TEXT       1        /* one text field in a box, wrapped onto lines */
#define LE_RECT       2        /* filled rectangle */

#define LE_NO_FONT    0xFF     /* ends the font list early */

/* opts for LE_TEXT: horizontal | vertical alignment */
#define LE_LEFT       0x00
#define LE_CENTER     0x01
#define LE_RIGHT      0x02
#define LE_HMASK      0x03
#define LE_TOP        0x00
#define LE_MIDDLE     0x04
#define LE_BOTTOM     0x08     /* the last line sits on one baseline shared by every font in
                                  the list, so the box does not jump when a fallback font is used */
#define LE_VMASK      0x0C

/* color */
#define LE_BLACK      0
#define LE_RED        1
#define LE_RED_PRICE  2        /* red if flag bit0 (ESL_CF_PRICE_RED) is set, else black */
#define LE_RED_FRAME  3        /* red if flag bit1 (ESL_CF_RED_FRAME) is set, else not drawn */

/*
 * One element. LE_TEXT puts text field `field` inside the box x, y, w, h:
 *   - The fonts in `font` are tried in order. A font is used if every character exists
 *     in it and the text fits on the lines without truncation or breaking a word.
 *     If no font passes, the last one is used, with wrapping and "…".
 *   - lines = maximum number of lines; 0 = as many as the box height holds (at least 1,
 *     at most LAYOUT_MAX_LINES). Lines are one font height apart.
 * LE_RECT fills the box x, y, w, h (field, font, opts and lines are unused).
 */
typedef struct {
    uint8_t kind;
    uint8_t field;
    uint8_t font[LAYOUT_MAX_FONTS];
    uint8_t opts;
    uint8_t lines;
    uint8_t color;
    uint16_t x;
    uint8_t y;
    uint16_t w;
    uint8_t h;
} layout_elem_t;

/* The built-in layout for ESL_LAYOUT_PRICE or ESL_LAYOUT_TEXT (others give "text"). */
const layout_elem_t __code *layout_builtin(uint8_t id);

/*
 * Clears the scene and adds every element. text/lens hold nfields text fields
 * (fields past nfields count as empty). flags are the CONTENT flags.
 * e is a generic pointer so that a layout received into XRAM can be used later.
 */
void layout_apply(draw_scene_t *s, const layout_elem_t *e, const uint8_t *const *text,
                  const uint8_t *lens, uint8_t nfields, uint8_t flags);

/* Builds the scene from a parsed CONTENT packet. buf is the buffer the packet was parsed from. */
void layout_build(draw_scene_t *s, const proto_reply_t *c, const uint8_t *buf);

/* Builds a simple screen with up to 4 lines (start-up, "waiting for AP") with the text layout. */
void layout_build_lines(draw_scene_t *s, const uint8_t *const *lines, const uint8_t *lens, uint8_t n);

/*
 * Helpers, exported for the tests.
 * layout_fit: how many characters of s fit in maxw. *ellipsis = 1 if the text
 *             was truncated and "…" should be drawn after it (its width is then subtracted).
 * layout_wrap: splits the text into two lines at the last space that fits (one space
 *             earlier if line 2 would only be a word of 1-2 characters, like "g").
 *             Returns the length of line 1; *next is the index where line 2 starts.
 *             If there is no such space the word is broken: the return value equals
 *             *next and *next < len.
 */
uint8_t layout_fit(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *ellipsis);
uint8_t layout_wrap(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *next);

#endif /* LAYOUT_H */
