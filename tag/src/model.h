/*
 * model.h - everything that depends on which tag the firmware is built for
 * (requirements G6, T19). One firmware build per model from the same code,
 * selected with -DMODEL=<build> in the compiler flags (29, the default, or 154; the list
 * is shared/models.json).
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

/* MODEL_CODE, MODEL_NAME, MODEL_HAS_RED, DRAW_W, DRAW_H for -DMODEL=<build>, from the
 * model registry shared/models.json (tools/gen_models.py) */
#include "model_gen.h"
/* EPD_TRANSPOSE, EPD_DATA_ENTRY, EPD_UPDATE_SEQ, EPD_INIT, ... for the model's panel
 * (tag/src/panels/, "panel" in shared/models.json) */
#include MODEL_PANEL_H
#if !defined(EPD_TRANSPOSE) || !defined(EPD_ROTATE_180) || !defined(EPD_DATA_ENTRY) || \
    !defined(EPD_GATE_SCAN) || !defined(EPD_UPDATE_SEQ) || !defined(EPD_WRITE_TEMP) || !defined(EPD_INIT)
#error "the panel header must define EPD_TRANSPOSE, EPD_ROTATE_180, EPD_DATA_ENTRY, EPD_GATE_SCAN, EPD_UPDATE_SEQ, EPD_WRITE_TEMP and EPD_INIT (EPD_SLEEP_CTRL1 and EPD_TEMP_C are optional)"
#endif

#if DRAW_H > 248
#error "a column is indexed with uint8_t; DRAW_H must stay below 249"
#endif

#endif /* MODEL_H */
