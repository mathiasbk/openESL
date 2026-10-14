/* helpers.h - builds packets the way the AP would send them, for the tests. */
#ifndef HELPERS_H
#define HELPERS_H

#include "compat.h"

/* IDLE payload. Returns the length. */
uint8_t make_idle(uint8_t *out, uint16_t next_poll_s);

/* UTF-8 -> Latin-1 the way the AP does it (dashes become '-', curly quotes straight,
   "…" becomes "...", other unknown characters '?'). Returns the length. */
uint8_t to_latin1(uint8_t *out, const char *utf8);

/* Like to_latin1, but in one of four rotating static buffers. len may be NULL. */
const uint8_t *L(const char *utf8, uint8_t *len);

/* CONTENT payload with nfields UTF-8 strings, converted to Latin-1. Returns the length. */
uint8_t make_content(uint8_t *out, uint16_t next_poll_s, uint32_t cid, uint8_t layout,
                     uint8_t flags, int nfields, const char *const *fields);

/* LAYOUT payload: one block (data, n bytes) of a layout file. Returns the length. */
uint8_t make_layout_block(uint8_t *out, uint16_t next_poll_s, uint32_t ver, uint16_t size, uint16_t pos,
                          const uint8_t *data, uint8_t n);

/* Writes one element in the 14-byte wire format (ESL_LE_* in shared/proto.h). Returns out + 14. */
uint8_t *put_elem(uint8_t *out, uint8_t kind, uint8_t field, uint8_t f0, uint8_t f1, uint8_t f2,
                  uint8_t opts, uint8_t lines, uint8_t color, uint16_t x, uint8_t y, uint16_t w, uint8_t h);

/*
 * A small test layout file for the model the tests are built for, with two layouts:
 *   0: a black band at the top with field 0 in white, field 1 right-aligned below it in the
 *      price fonts (highlight colour), field 2 small at the bottom left, field 3 small at
 *      the bottom right
 *   1: field 0 centred in the middle of the screen, with a frame (flag bit1)
 * Returns the size; *ver gets the version (CRC32 of the file, 0 counted as 1).
 */
uint16_t make_test_layout_file(uint8_t *out, uint32_t *ver);

/* 802.15.4 frame AP -> tag (without FCS) around a payload. Returns the length. */
uint8_t make_ap_frame(uint8_t *out, uint8_t seq, uint16_t pan, const uint8_t *dst_mac,
                      const uint8_t *payload, uint8_t plen);

#endif
