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

/*
 * CRC-32 of the image the panel shows: what epd_draw last streamed into it, 0 = unknown.
 * epd_draw computes the CRC of the new image while it writes the panel's RAM, and when
 * it equals this one it skips the refresh (the 2.9 s update that costs as much as ~280
 * polls and makes the screen flash): the panel already shows that image. Kept between
 * polls (deep sleep returns to the loop); app_init() clears it at start-up, since the
 * panel may show anything after a reset or a reflash.
 */
extern uint32_t __xdata epd_shown_crc;

/* 1 if the last epd_draw ran a refresh, 0 if it skipped it because the image was the same. */
extern uint8_t epd_refreshed;

/* Returns false if the display did not answer (BUSY never went low). */
bool epd_draw(draw_scene_t *s);

#endif /* EPD_H */
