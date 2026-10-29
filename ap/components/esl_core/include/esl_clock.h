#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Date fields in layouts (#183): a fixed text in a layout file may hold {date},
 * {date_short}, {weekday} and {week}, which the AP fills in with today's date when it
 * draws the screen (esl_render). Only the date, never the time of day: each new value is
 * a new image and a full refresh of the panel, so a layout that uses them is redrawn
 * once a day ({week} alone: once a week), and a tag whose image came out the same is not
 * sent it again (the image id is its CRC).
 *
 *   {weekday}    "Thursday"
 *   {date}       "29 October 2026"
 *   {date_short} "29.10.2026"
 *   {week}       "44" (ISO 8601 week)
 *
 * In English. A word in braces that is not
 * one of these is drawn as it is. Drawn without the time (the web page before it is set),
 * the fields are empty.
 *
 * The fields are filled in with the AP's local date, in the time zone set on the AP
 * (Settings, General; UTC until one is set). A layout with them is not drawn at all while
 * the time is unknown (after a restart, until NTP or PUT /api/time): its tags keep what
 * they show (esl_image.c draw()).
 *
 * The compiler notes which layouts use them (esl_layout_bin_t clock), and the version
 * the AP keeps with a drawn image (esl_layouts_image_ver) mixes in the day or week for
 * those, so a new day draws them again as a new layout file does. The version is read
 * before the image is drawn: should the day change in between, the image of the new day
 * is kept with the old day's version, and the next POLL draws it once more (the same
 * bytes, so nothing is sent); the other way round, an old image with a new version that
 * would stand for a day, cannot happen. */

#define ESL_CLOCK_NONE 0
#define ESL_CLOCK_WEEK 1 /* only {week} */
#define ESL_CLOCK_DAY  2 /* any of the others */

#define ESL_CLOCK_TEXT_MAX 96 /* a fixed text with its fields filled in, longest (cut there) */
#define ESL_CLOCK_TEXTS    4  /* texts with date fields per layout (esl_render.c keeps them filled in) */

/* Today in local time as days since 1970-01-01, -1 while the time is unknown (the start).
 * Set from outside rather than asked for, so drawing (under the layouts lock) takes no
 * other lock: on the AP the schedule's tick sets it every second (main/sched.c), on the
 * web page the renderer from the browser's clock (web_set_today). */
void esl_clock_set_today(int32_t days);
/* False while it is unknown */
bool esl_clock_today(int32_t *days);

/* ESL_CLOCK_* for a fixed text (Latin-1, n bytes): the finest field it holds */
uint8_t esl_clock_res(const uint8_t *t, size_t n);

/* t (Latin-1, n bytes) with its fields filled in for days (days < 0: unknown, empty) into
 * out (cap bytes, cut short where it ends). Returns the length. */
size_t esl_clock_fill(const uint8_t *t, size_t n, int32_t days, uint8_t *out, size_t cap);

/* A value that changes when a text of resolution res would change: the day or the week
 * now, never 0; 0 for ESL_CLOCK_NONE */
uint32_t esl_clock_stamp(uint8_t res);
