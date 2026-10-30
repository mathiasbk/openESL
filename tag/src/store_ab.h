/*
 * store_ab.h - the settings record kept in two flash pages in turn (hal_store_*, hal.h).
 * The decisions are here, without the flash itself, so the host tests run them; the HAL
 * gives the two pages (read as memory) and store_ab_page to erase and write one.
 *
 * A copy: 0xA6, sequence number, length, data, sum of the data bytes. The newer good copy
 * counts. Firmware before radio security kept one copy in page A as 0xA5, length, data,
 * sum. This firmware never leaves such a copy in A next to its own (its first write
 * rewrites A too), so one found there was written by older firmware after a downgrade,
 * and it counts over the copy in B.
 */
#ifndef STORE_AB_H
#define STORE_AB_H

#include "compat.h"
#include "hal/hal.h"

#define STORE_AB_SIZE (4 + HAL_STORE_MAX)   /* the most a copy takes */

/* Erases page A (to_a) or B and writes len bytes from buf to its start; false if the
   flash did not finish. In the HAL (or the test). */
bool store_ab_page(bool to_a, const uint8_t *buf, uint8_t len);

/* The newer good copy of pages a and b: 1 = a, 2 = b, 0 = none; its data and length */
uint8_t store_ab_newest(const uint8_t *a, const uint8_t *b, const uint8_t **data, uint8_t *len);
/* A new copy of data into the page without the newest one (buf: STORE_AB_SIZE bytes of
   scratch). False: the copy before still counts (or there was none). */
bool store_ab_write(const uint8_t *a, const uint8_t *b, uint8_t *buf, const uint8_t *data, uint8_t len);
uint8_t store_ab_read(const uint8_t *a, const uint8_t *b, uint8_t *data, uint8_t maxlen);

#endif
