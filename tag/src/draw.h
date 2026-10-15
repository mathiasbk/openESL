/*
 * draw.h - draws the screen one column at a time.
 *
 * The screen size comes from model.h (296 x 128 on the 2.9", 200 x 200 on the 1.54").
 * Both panels are black/white only. ZBS243 does not have RAM for a whole image, so the
 * epd layer asks for one column (x) at a time: DRAW_H pixels = DRAW_COL_BYTES bytes
 * (16 on the 2.9", 25 on the 1.54").
 * This fits the display controller's memory, where one line is one column here.
 *
 * Bit order in a column: byte 0 MSB = y 0 (top), then downwards. Unused bits at the
 * end of the last byte (when DRAW_H is not a multiple of 8) are always 0.
 * Bit = 1 means black. The epd layer flips polarity and direction if the controller
 * needs something different.
 *
 * Highlighting is black/white too: white text on a filled black rectangle (an inverted
 * field, like the price box of the stock firmware). Rectangles are drawn first, in the
 * order they were added, then the barcode, then text runs in order; DRAW_WHITE clears pixels.
 */
#ifndef DRAW_H_INCLUDED
#define DRAW_H_INCLUDED

#include "compat.h"
#include "model.h"

#define DRAW_COL_BYTES   ((DRAW_H + 7) / 8)

#define DRAW_MAX_RUNS    16     /* text runs (one per line), 15 bytes each in XRAM */
#define DRAW_MAX_RECTS   10     /* frame (4), rect elements and inverted fields, 7 bytes each */
#define DRAW_MAX_BARCODES 1     /* one barcode per screen */
#define DRAW_BC_BYTES    ((DRAW_W + 7) / 8)

#define DRAW_BLACK       0
#define DRAW_WHITE       1      /* clears: white text or a white rectangle on black */

/* One text run on one line. text points at Latin-1 bytes that must live until drawing is done. */
typedef struct {
    const uint8_t *text;
    uint8_t len;
    uint8_t ellipsis;    /* 1 = draw "…" after the text */
    uint8_t font;
    uint8_t color;
    uint16_t x;          /* left edge */
    uint8_t y;           /* top row */
    uint16_t w;          /* width in pixels, computed by draw_add_text */
    /* internal cursor, moved forward as x increases */
    uint8_t ci;
    uint16_t cx;
} draw_run_t;

/* Filled rectangle, bounds included. */
typedef struct {
    uint16_t x0, x1;
    uint8_t y0, y1;
    uint8_t color;
} draw_rect_t;

/* A barcode: nmod modules of mw pixels from x0, black where the bit is 1 (barcode.h),
   rows y0..y1. */
typedef struct {
    uint16_t x0;
    uint16_t nmod;
    uint8_t mw;
    uint8_t y0, y1;
    uint8_t bits[DRAW_BC_BYTES];
} draw_bars_t;

typedef struct {
    uint8_t nruns;
    uint8_t nrects;
    uint8_t nbars;            /* 0 or 1: bars is in use */
    draw_run_t run[DRAW_MAX_RUNS];
    draw_rect_t rect[DRAW_MAX_RECTS];
    draw_bars_t bars;
    uint16_t last_x;
} draw_scene_t;

void draw_clear(draw_scene_t *s);

/* Adds text. Returns false if there is no room for more. */
bool draw_add_text(draw_scene_t *s, uint8_t font, uint8_t color, uint16_t x, uint8_t y,
                   const uint8_t *text, uint8_t len, uint8_t ellipsis);

bool draw_add_rect(draw_scene_t *s, uint16_t x0, uint8_t y0, uint16_t x1, uint8_t y1, uint8_t color);

/* Resets the cursors. Call before the first column. */
void draw_begin(draw_scene_t *s);

/*
 * Computes column x (0..DRAW_W-1) into black (DRAW_COL_BYTES, bit = 1 is black).
 * Fastest when x increases by 1 per call; if x goes backwards, the cursors start over.
 * red (DRAW_COL_BYTES) is always cleared: no supported panel has red (MODEL_HAS_RED = 0).
 * It stays in the signature because epd.c streams two planes on a model with red.
 */
void draw_column(draw_scene_t *s, uint16_t x, uint8_t *black, uint8_t *red);

#endif /* DRAW_H_INCLUDED */
