#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_wifi.h"

/* The AP's own status screen (boards with a small LCD, main/display.c): four
 * lines of text in the tag's small font (17 px), independent of ESP-IDF so the
 * host tests can render it.
 *
 *   OpenESL 0.4.0   ch 25
 *   9 OK  2 busy  1 bad    the dashboard's tag states, in green, yellow and red
 *   192.168.1.40           or  Setup: openesl-1A2B / joining "Net" / WiFi off
 *   up 3 d 4 h, 9 seen     or  192.168.4.1/setup while the setup network is open
 *
 * A byte from 1 to ESL_SCREEN_DIM in a line's text sets the colour of the
 * rest of the line, so one line can have several colours. */

#define ESL_SCREEN_LINES 4
#define ESL_SCREEN_LINE_MAX 40

typedef enum {
    ESL_SCREEN_BG = 0,
    ESL_SCREEN_TEXT,  /* white */
    ESL_SCREEN_TITLE, /* first line */
    ESL_SCREEN_OK,    /* tags that are OK */
    ESL_SCREEN_WARN,  /* setup network, joining, busy or late tags */
    ESL_SCREEN_BAD,   /* WiFi off, tags that need attention */
    ESL_SCREEN_DIM,   /* last line */
} esl_screen_color_t;

typedef struct {
    const char *fw;
    uint8_t channel;
    unsigned tags, tags_max;
    bool table_full;   /* a new tag found no room (esl_store_full) */
    /* the tags by what the dashboard shows for them (esl_api_tag_level) */
    unsigned ok, busy, bad;
    unsigned seen;     /* tags heard from in the last 10 minutes */
    uint64_t uptime_s;
    esl_wifi_status_t wifi;
    bool serial_api;   /* the USB API is there when WiFi is not */
} esl_screen_info_t;

typedef struct {
    char text[ESL_SCREEN_LINES][ESL_SCREEN_LINE_MAX]; /* Latin-1 */
    uint8_t color[ESL_SCREEN_LINES];                  /* esl_screen_color_t */
} esl_screen_t;

void esl_screen_make(const esl_screen_info_t *info, esl_screen_t *out);

/* Draws the lines into pix (w * h bytes, one esl_screen_color_t per pixel,
 * row by row). Text that does not fit is cut. */
void esl_screen_render(const esl_screen_t *s, uint8_t *pix, unsigned w, unsigned h);
