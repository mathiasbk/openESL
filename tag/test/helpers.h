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

/* 802.15.4 frame AP -> tag (without FCS) around a payload. Returns the length. */
uint8_t make_ap_frame(uint8_t *out, uint8_t seq, uint16_t pan, const uint8_t *dst_mac,
                      const uint8_t *payload, uint8_t plen);

#endif
