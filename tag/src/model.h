/*
 * model.h - everything that depends on which tag the firmware is built for
 * (requirements G6, T19). One firmware build per model from the same code,
 * selected with -DMODEL=29 (default) or -DMODEL=154 in the compiler flags.
 *
 * The screen is described in the orientation the layouts use: DRAW_W columns from
 * left to right, each DRAW_H pixels tall. draw.c computes one column at a time and
 * epd.c streams the columns to the display controller.
 *
 * Both supported tags are black/white only (no red): the SoluM ST-GM29XX2, 2.9",
 * 296 x 128, and the SoluM ST-GM16XX2, 1.54", 200 x 200 (docs/krav.md G6).
 * MODEL_HAS_RED = 0 for both: draw.c only fills the black plane (highlighting is an
 * inverted field, white on black) and epd.c sends only the black plane.
 * Which controller the small tag has is not confirmed yet (phase 0).
 */
#ifndef MODEL_H
#define MODEL_H

#ifndef MODEL
#define MODEL 29
#endif

#if MODEL == 29
/* SoluM ST-GM29XX2, 2.9", black/white */
#define MODEL_CODE       1          /* model byte in POLL (requirement T18), = ESL_MODEL_29 */
#define MODEL_NAME       "2.9"
#define MODEL_HAS_RED    0
#define DRAW_W           296
#define DRAW_H           128
#elif MODEL == 154
/* SoluM ST-GM16XX2, 1.54", black/white */
#define MODEL_CODE       2          /* = ESL_MODEL_154 */
#define MODEL_NAME       "1.54"
#define MODEL_HAS_RED    0
#define DRAW_W           200
#define DRAW_H           200
#else
#error "unknown MODEL (use 29 or 154)"
#endif

#if DRAW_H > 248
#error "a column is indexed with uint8_t; DRAW_H must stay below 249"
#endif

#endif /* MODEL_H */
