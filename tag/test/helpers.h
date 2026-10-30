/* helpers.h - builds packets the way the AP would send them, for the tests. */
#ifndef HELPERS_H
#define HELPERS_H

#include "compat.h"

/* IDLE payload. Returns the length. */
uint8_t make_idle(uint8_t *out, uint16_t next_poll_s);

/* CONFIG payload (settings in the order of shared/proto.h). Returns the length. */
uint8_t make_config(uint8_t *out, uint16_t next_poll_s, uint16_t ver, uint16_t poll_s,
                    uint16_t max_poll_s, uint16_t lost_ap_s);
/* CONFIG with the long backoff (long_poll_s, long_after_min) too */
uint8_t make_config_long(uint8_t *out, uint16_t next_poll_s, uint16_t ver, uint16_t poll_s,
                         uint16_t max_poll_s, uint16_t lost_ap_s, uint16_t long_poll_s,
                         uint16_t long_after_min);

/* UTF-8 -> Latin-1 the way the AP does it (dashes become '-', curly quotes straight,
   "…" becomes "...", other unknown characters '?'). Returns the length. */
uint8_t to_latin1(uint8_t *out, const char *utf8);

/* Like to_latin1, but in one of four rotating static buffers. len may be NULL. */
const uint8_t *L(const char *utf8, uint8_t *len);

/* FW payload: one block (data, n bytes) of a firmware image. Returns the length. */
uint8_t make_fw_block(uint8_t *out, uint16_t next_poll_s, uint16_t ver, uint16_t size, uint16_t pos,
                      uint32_t crc, uint8_t model, const uint8_t *data, uint8_t n);

/* IMG payload: one block (data, n bytes) of a screen image. Returns the length. */
uint8_t make_img_block(uint8_t *out, uint16_t next_poll_s, uint32_t id, uint16_t size, uint16_t pos,
                       uint8_t model, uint8_t fmt, const uint8_t *data, uint8_t n);

/* 802.15.4 frame AP -> tag (without FCS) around a payload. Returns the length. */
uint8_t make_ap_frame(uint8_t *out, uint8_t seq, uint16_t pan, const uint8_t *dst_mac,
                      const uint8_t *payload, uint8_t plen);

#endif
