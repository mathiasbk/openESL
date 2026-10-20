/*
 * status.h - the screens the tag draws itself (requirement T9): the start screen and
 * "Waiting for content" / "No AP found", with the tag's name, MAC and firmware version.
 * Everything else on the screen is an image the AP draws (image.h).
 *
 * Up to STATUS_LINES lines of text, one per box of equal height down the screen, each in
 * the medium font, or the small one if it does not fit. A line that is too long wraps
 * onto as many lines as the box holds (at most STATUS_MAX_WRAP) and ends with "…" if it
 * still does not fit. The text pointed to must live until drawing is done.
 */
#ifndef STATUS_H
#define STATUS_H

#include "compat.h"
#include "draw.h"

#define STATUS_LINES     4
#define STATUS_MAX_WRAP  3

/* Clears the scene and adds up to STATUS_LINES lines of Latin-1 text. */
void status_build(draw_scene_t *s, const uint8_t *const *lines, const uint8_t *lens, uint8_t n);

/*
 * Helpers, exported for the tests.
 * status_fit:  how many characters of s fit in maxw. *ellipsis = 1 if the text
 *              was truncated and "…" should be drawn after it (its width is then subtracted).
 * status_wrap: splits the text into two lines at the last space that fits (one space
 *              earlier if line 2 would only be a word of 1-3 characters, like "g").
 *              Returns the length of line 1; *next is the index where line 2 starts.
 *              If there is no such space the word is broken: the return value equals
 *              *next and *next < len.
 */
uint8_t status_fit(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *ellipsis);
uint8_t status_wrap(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *next);

#endif /* STATUS_H */
