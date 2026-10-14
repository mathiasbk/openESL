/*
 * epd.h - the display (SSD16xx family, 296 x 128, black/white/red).
 *
 * epd_draw: wakes the display, streams all columns from draw_column into the
 * controller's black and red planes, starts an update with the OTP waveform,
 * waits for BUSY and puts the display into deep sleep.
 *
 * It is not yet confirmed which controller it is, or which ZBS243 pins it is
 * connected to (phase 0). See epd.c.
 */
#ifndef EPD_H
#define EPD_H

#include "compat.h"
#include "draw.h"

/* Returns false if the display did not answer (BUSY never went low). */
bool epd_draw(draw_scene_t *s);

#endif /* EPD_H */
