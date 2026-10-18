/*
 * epd.h - the display (SSD16xx family; size and colours from model.h).
 *
 * epd_draw: wakes the display, streams all columns from draw_column into the
 * controller's black and red planes (only black on a black/white model), starts an
 * update with the OTP waveform, waits for BUSY and puts the display into deep sleep.
 *
 * It is not yet confirmed which controller it is, or which ZBS243 pins it is
 * connected to (phase 0). See epd.c.
 */
#ifndef EPD_H
#define EPD_H

#include "compat.h"
#include "draw.h"

/*
 * In a panel header's EPD_INIT list, with count 0: set_window() goes here. 0xFF is not
 * an SSD16xx command; a panel whose controller has one would need another marker.
 */
#define EPD_OP_WINDOW 0xFF

/* Returns false if the display did not answer (BUSY never went low). */
bool epd_draw(draw_scene_t *s);

#endif /* EPD_H */
