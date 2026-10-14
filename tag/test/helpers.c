#include <string.h>
#include "proto.h"
#include "helpers.h"

uint8_t make_idle(uint8_t *out, uint16_t next_poll_s)
{
    out[0] = ESL_MSG_IDLE;
    out[1] = ESL_PROTO_VER;
    ESL_PUT_U16(out + 2, next_poll_s);
    return ESL_IDLE_LEN;
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
        out[n++] = cp <= 0xFF ? (uint8_t)cp : '?';
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

uint8_t make_content(uint8_t *out, uint16_t next_poll_s, uint32_t cid, uint8_t layout,
                     uint8_t flags, int nfields, const char *const *fields)
{
    uint8_t pos = ESL_CONTENT_HDR_LEN;
    int i;

    out[0] = ESL_MSG_CONTENT;
    out[1] = ESL_PROTO_VER;
    ESL_PUT_U16(out + 2, next_poll_s);
    ESL_PUT_U32(out + 4, cid);
    out[8] = layout;
    out[9] = flags;
    for (i = 0; i < nfields; i++) {
        uint8_t n = to_latin1(out + pos + 1, fields[i]);
        out[pos] = n;
        pos += 1 + n;
    }
    return pos;
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
