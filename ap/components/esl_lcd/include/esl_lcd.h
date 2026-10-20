#pragma once
#include "esl_screen.h"
#include "esp_err.h"

/* The status screen's panel: a small ST7789 LCD (ESL_DISPLAY, Kconfig in this
 * component). It draws an esl_screen_t (esl_core, host-tested); what goes on
 * the screen is up to the caller: main/display.c in the AP, ap/radio/ in a
 * radio coprocessor that shows its AP host's status.
 *
 * Without CONFIG_ESL_DISPLAY every call returns ESP_ERR_NOT_SUPPORTED. */

/* Sets up the panel and shows s with a frame around the edge (so wrong gaps
 * show as a missing or doubled side), then turns the backlight on. */
esp_err_t esl_lcd_start(const esl_screen_t *s);
/* Draws s. Not thread-safe: call it from one task. */
esp_err_t esl_lcd_show(const esl_screen_t *s);
