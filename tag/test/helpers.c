#include <string.h>
#include "proto.h"
#include "helpers.h"
#include "model.h"
#include "proto_tag.h"

uint8_t make_idle(uint8_t *out, uint16_t next_poll_s)
{
    out[0] = ESL_MSG_IDLE;
    out[1] = ESL_PROTO_VER;
    ESL_PUT_U16(out + 2, next_poll_s);
    return ESL_IDLE_LEN;
}

uint8_t make_config(uint8_t *out, uint16_t next_poll_s, uint16_t ver, uint16_t poll_s,
                    uint16_t max_poll_s, uint16_t lost_ap_s)
{
    return make_config_long(out, next_poll_s, ver, poll_s, max_poll_s, lost_ap_s, 0, 0);
}

uint8_t make_config_long(uint8_t *out, uint16_t next_poll_s, uint16_t ver, uint16_t poll_s,
                         uint16_t max_poll_s, uint16_t lost_ap_s, uint16_t long_poll_s,
                         uint16_t long_after_min)
{
    out[0] = ESL_MSG_CONFIG;
    out[1] = ESL_PROTO_VER;
    ESL_PUT_U16(out + ESL_OFF_NEXT_POLL, next_poll_s);
    ESL_PUT_U16(out + ESL_CONFIG_OFF_VER, ver);
    ESL_PUT_U16(out + ESL_CONFIG_OFF_POLL, poll_s);
    ESL_PUT_U16(out + ESL_CONFIG_OFF_MAX_POLL, max_poll_s);
    ESL_PUT_U16(out + ESL_CONFIG_OFF_LOST_AP, lost_ap_s);
    ESL_PUT_U16(out + ESL_CONFIG_OFF_LONG_POLL, long_poll_s);
    ESL_PUT_U16(out + ESL_CONFIG_OFF_LONG_AFTER, long_after_min);
    ESL_PUT_U16(out + ESL_CONFIG_OFF_CHANNEL, 0);
    return ESL_CONFIG_LEN;
}

/* The same mapping as the AP (ap/components/esl_core/esl_text.c, map_codepoint). */
static uint8_t map_cp(unsigned cp)
{
    if (cp >= 0x20 && cp < 0x7F)
        return (uint8_t)cp;
    if (cp == 0xA0)
        return ' ';
    if (cp > 0xA0 && cp <= 0xFF)
        return (uint8_t)cp;
    switch (cp) {
    case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F:
        return ' ';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        return '-';
    case 0x2018: case 0x2019: case 0x201A: case 0x2032:
        return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033:
        return '"';
    case 0x2022:
        return 0xB7;
    default:
        return '?';
    }
}

uint8_t to_latin1(uint8_t *out, const char *utf8)
{
    const unsigned char *s = (const unsigned char *)utf8;
    uint8_t n = 0;
    unsigned cp;
    int extra;

    while (*s) {
        if (*s < 0x80) { cp = *s++; extra = 0; }
        else if ((*s & 0xE0) == 0xC0) { cp = *s++ & 0x1F; extra = 1; }
        else if ((*s & 0xF0) == 0xE0) { cp = *s++ & 0x0F; extra = 2; }
        else { cp = *s++ & 0x07; extra = 3; }
        while (extra-- && (*s & 0xC0) == 0x80)
            cp = (cp << 6) | (*s++ & 0x3F);
        if (cp == 0x2026) {          /* the AP sends "…" as "..." */
            out[n++] = '.';
            out[n++] = '.';
            out[n++] = '.';
        } else {
            out[n++] = map_cp(cp);
        }
    }
    return n;
}

const uint8_t *L(const char *utf8, uint8_t *len)
{
    static uint8_t bufs[4][256];
    static int k;
    uint8_t *b = bufs[k++ & 3];
    uint8_t n = to_latin1(b, utf8);
    if (len)
        *len = n;
    return b;
}

uint8_t make_ap_frame(uint8_t *out, uint8_t seq, uint16_t pan, const uint8_t *dst_mac,
                      const uint8_t *payload, uint8_t plen)
{
    ESL_PUT_U16(out + ESL_AT_OFF_FC, ESL_FC_AP_TO_TAG);
    out[ESL_AT_OFF_SEQ] = seq;
    ESL_PUT_U16(out + ESL_AT_OFF_PAN, pan);
    memcpy(out + ESL_AT_OFF_DST, dst_mac, 8);
    ESL_PUT_U16(out + ESL_AT_OFF_SRC, ESL_AP_SHORT_ADDR);
    memcpy(out + ESL_MAC_HDR_LEN, payload, plen);
    return (uint8_t)(ESL_MAC_HDR_LEN + plen);
}

uint8_t make_fw_block(uint8_t *out, uint16_t next_poll_s, uint16_t ver, uint16_t size, uint16_t pos,
                      uint32_t crc, uint8_t model, const uint8_t *data, uint8_t n)
{
    out[0] = ESL_MSG_FW;
    out[1] = ESL_PROTO_VER;
    ESL_PUT_U16(out + ESL_OFF_NEXT_POLL, next_poll_s);
    ESL_PUT_U16(out + ESL_FW_OFF_VER, ver);
    ESL_PUT_U16(out + ESL_FW_OFF_SIZE, size);
    ESL_PUT_U16(out + ESL_FW_OFF_POS, pos);
    ESL_PUT_U32(out + ESL_FW_OFF_CRC, crc);
    out[ESL_FW_OFF_MODEL] = model;
    memcpy(out + ESL_FW_OFF_DATA, data, n);
    return (uint8_t)(ESL_FW_HDR_LEN + n);
}

uint8_t make_img_block(uint8_t *out, uint16_t next_poll_s, uint32_t id, uint16_t size, uint16_t pos,
                       uint8_t model, uint8_t fmt, const uint8_t *data, uint8_t n)
{
    out[0] = ESL_MSG_IMG;
    out[1] = ESL_PROTO_VER;
    ESL_PUT_U16(out + ESL_OFF_NEXT_POLL, next_poll_s);
    ESL_PUT_U32(out + ESL_IMG_OFF_ID, id);
    ESL_PUT_U16(out + ESL_IMG_OFF_SIZE, size);
    ESL_PUT_U16(out + ESL_IMG_OFF_POS, pos);
    out[ESL_IMG_OFF_MODEL] = model;
    out[ESL_IMG_OFF_FMT] = fmt;
    memcpy(out + ESL_IMG_OFF_DATA, data, n);
    return (uint8_t)(ESL_IMG_HDR_LEN + n);
}
