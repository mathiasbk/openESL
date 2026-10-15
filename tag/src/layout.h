/*
 * layout.h - places the text fields from a CONTENT packet on the screen.
 *
 * A layout is a list of elements (layout_elem_t). The built-in layouts "price" and
 * "text" are such lists in flash, one set per model (layout.c), ending with LE_END.
 * A layout file downloaded from the AP (requirements G7/T17, docs/protokoll.md part 1,
 * "Layout transfer") holds up to 16 more layouts as 14-byte elements in the same field
 * order (ESL_LE_* in shared/proto.h); layout_apply_bin() draws one of them straight
 * from the downloaded bytes in XRAM.
 *
 * Built-in layouts (2.9", the 1.54" has the same elements in a 200 x 200 box):
 *   1 "price": name (medium font, 2 lines; small font, 3 lines, if it does not fit),
 *              price right-aligned (large font, then mid, then medium), one subtitle
 *              (small font, bottom left). Flag bit0 highlights the price: white digits
 *              in a filled black box (an inverted field).
 *   2 "text":  4 lines, each in the medium font, or the small font if it does not fit.
 * Flag bit1 gives a black frame around the whole screen.
 * Both panels are black/white, so all highlighting is black/white (model.h).
 * Text that does not fit even in the last font is truncated with "…".
 * The text pointed to must live until drawing is done.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include "compat.h"
#include "draw.h"
#include "proto_tag.h"

#define LAYOUT_FRAME_W    3    /* thickness of the frame (flag bit1) */
#define LAYOUT_INVERT_PAD 4    /* black around the text of an inverted field, each side */
#if DRAW_W >= 200
#define LAYOUT_MARGIN     6    /* left/right margin */
#else
#define LAYOUT_MARGIN     5
#endif

#define LAYOUT_MAX_FONTS  3    /* fonts to try per text element */
#define LAYOUT_MAX_LINES  3    /* lines per text element */

/* Element kinds */
#define LE_END        0
#define LE_TEXT       ESL_LE_TEXT     /* one text field in a box, wrapped onto lines */
#define LE_RECT       ESL_LE_RECT     /* filled rectangle */
#define LE_BARCODE    ESL_LE_BARCODE  /* one text field as a barcode (font[0] = ESL_BC_*, lines = module width) */

#define LE_NO_FONT    ESL_FONT_NONE   /* ends the font list early */

/* opts for LE_TEXT: horizontal | vertical alignment */
#define LE_LEFT       ESL_LE_LEFT
#define LE_CENTER     ESL_LE_CENTER
#define LE_RIGHT      ESL_LE_RIGHT
#define LE_HMASK      0x03
#define LE_TOP        0x00
#define LE_MIDDLE     ESL_LE_MIDDLE
#define LE_BOTTOM     ESL_LE_BOTTOM   /* the last line sits on one baseline shared by every font
                                         in the list, so the box does not jump when a fallback
                                         font is used */
#define LE_VMASK      0x0C

/* color */
#define LE_BLACK      ESL_LE_BLACK
#define LE_WHITE      ESL_LE_WHITE    /* white text or rectangle, on top of an LE_RECT in black */
#define LE_INVERT     ESL_LE_INVERT   /* LE_TEXT: white text in a black box around it */
#define LE_PRICE_HL   ESL_LE_PRICE_HL /* LE_INVERT if flag bit0 (ESL_CF_PRICE_HIGHLIGHT) is set, else black */
#define LE_FRAME      ESL_LE_FRAME    /* black if flag bit1 (ESL_CF_FRAME) is set, else not drawn */

/*
 * One element. LE_TEXT puts text field `field` inside the box x, y, w, h:
 *   - The fonts in `font` are tried in order. A font is used if every character exists
 *     in it and the text fits on the lines without truncation or breaking a word.
 *     If no font passes, the last one is used, with wrapping and "…".
 *   - lines = maximum number of lines; 0 = as many as the box height holds (at least 1,
 *     at most LAYOUT_MAX_LINES). Lines are one font height apart.
 *   - LE_INVERT (and LE_PRICE_HL when highlighted): the text is placed in the box made
 *     LAYOUT_INVERT_PAD smaller on every side, a font must also fit its height, and a
 *     black rectangle covers the font cells of the lines plus LAYOUT_INVERT_PAD. The
 *     rectangle hugs the text (right-aligned text gets a right-aligned box) and stays
 *     inside x, y, w, h. It uses one of the DRAW_MAX_RECTS rectangles.
 * LE_RECT fills the box x, y, w, h (field, font, opts and lines are unused).
 * LE_BARCODE draws text field `field` as a barcode with symbology font[0] (ESL_BC_*), bars
 * the full height of the box, modules `lines` pixels wide (0 = the widest that fits, up to
 * ESL_BC_MODULE_MAX), placed by the horizontal alignment in opts with the quiet zones
 * inside the box. One per screen (DRAW_MAX_BARCODES). Nothing is drawn if the text cannot
 * be encoded or does not fit.
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

/*
 * Downloaded layout file (docs/protokoll.md part 1). One buffer in XRAM, filled block by
 * block by app.c; layout_file_ver is 0 while nothing valid is in it. app.c keeps a copy in
 * flash (requirement T12) and puts it back with layout_file_restore() after a restart.
 */
extern uint8_t __xdata layout_file[ESL_LBIN_MAX];
extern uint16_t __xdata layout_file_len;
extern uint32_t __xdata layout_file_ver;

/*
 * Checks a complete file in layout_file[0..len): CRC32 against ver, then the structure
 * (format, model, element kinds, fields, fonts, colours, boxes inside the screen).
 * Installs it (layout_file_ver = ver) and returns true, or clears it and returns false.
 */
bool layout_file_install(uint16_t len, uint32_t ver);

/*
 * layout_file[] holds bytes read back from flash (a file, or anything else). Finds the
 * file's length from its structure and installs it with its CRC-32 as the version, so it
 * gets every check a download gets. Returns true if a file is installed.
 */
bool layout_file_restore(void);

/* Forgets the downloaded file. */
void layout_file_clear(void);

/* Elements of layout n (0-based) in the installed file, or 0 if there is no such layout. */
const uint8_t *layout_file_find(uint8_t n, uint8_t *nelem);

/* The built-in layout for ESL_LAYOUT_PRICE or ESL_LAYOUT_TEXT (others give "text"). */
const layout_elem_t __code *layout_builtin(uint8_t id);

/*
 * Clears the scene and adds every element. text/lens hold nfields text fields
 * (fields past nfields count as empty). flags are the CONTENT flags.
 * e is a generic pointer, so the list may be in flash or XRAM.
 */
void layout_apply(draw_scene_t *s, const layout_elem_t *e, const uint8_t *const *text,
                  const uint8_t *lens, uint8_t nfields, uint8_t flags);

/* As layout_apply, for nelem elements in the 14-byte wire format (ESL_LE_* in proto.h). */
void layout_apply_bin(draw_scene_t *s, const uint8_t *wire, uint8_t nelem, const uint8_t *const *text,
                      const uint8_t *lens, uint8_t nfields, uint8_t flags);

/*
 * Builds the scene from a parsed CONTENT packet. buf is the buffer the packet was parsed from.
 * Layout ESL_LAYOUT_FILE + n uses layout n of the downloaded file; if the tag has no such
 * layout (yet), the fields are shown with the built-in "text" layout instead.
 */
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
