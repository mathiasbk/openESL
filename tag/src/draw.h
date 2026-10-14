/*
 * draw.h - draws the screen one column at a time.
 *
 * The screen is 296 x 128 in landscape. ZBS243 does not have RAM for a whole image,
 * so the epd layer asks for one column (x) at a time: 128 pixels = 16 bytes per colour plane.
 * This fits the display controller's memory, where one line is 128 pixels along the short side.
 *
 * Bit order in a column: byte 0 MSB = y 0 (top), byte 15 LSB = y 127.
 * Bit = 1 means ink (black in black, red in red). The epd layer flips polarity and
 * direction if the controller needs something different.
 */
#ifndef DRAW_H_INCLUDED
#define DRAW_H_INCLUDED

#include "compat.h"

#define DRAW_W           296
#define DRAW_H           128
#define DRAW_COL_BYTES   (DRAW_H / 8)

#define DRAW_MAX_RUNS    6
#define DRAW_MAX_RECTS   4

#define DRAW_BLACK       0
#define DRAW_RED         1

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

typedef struct {
    uint8_t nruns;
    uint8_t nrects;
    draw_run_t run[DRAW_MAX_RUNS];
    draw_rect_t rect[DRAW_MAX_RECTS];
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
 * Computes column x (0..DRAW_W-1) into black and red (DRAW_COL_BYTES each).
 * Fastest when x increases by 1 per call; if x goes backwards, the cursors start over.
 * Where both planes have ink, red wins.
 */
void draw_column(draw_scene_t *s, uint16_t x, uint8_t *black, uint8_t *red);

#endif /* DRAW_H_INCLUDED */
