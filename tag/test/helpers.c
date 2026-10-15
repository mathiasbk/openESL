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
    out[0] = ESL_MSG_CONFIG;
    out[1] = ESL_PROTO_VER;
    ESL_PUT_U16(out + ESL_OFF_NEXT_POLL, next_poll_s);
    ESL_PUT_U16(out + ESL_CONFIG_OFF_VER, ver);
    ESL_PUT_U16(out + ESL_CONFIG_OFF_POLL, poll_s);
    ESL_PUT_U16(out + ESL_CONFIG_OFF_MAX_POLL, max_poll_s);
    ESL_PUT_U16(out + ESL_CONFIG_OFF_LOST_AP, lost_ap_s);
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

uint8_t make_layout_block(uint8_t *out, uint16_t next_poll_s, uint32_t ver, uint16_t size, uint16_t pos,
                          const uint8_t *data, uint8_t n)
{
    out[0] = ESL_MSG_LAYOUT;
    out[1] = ESL_PROTO_VER;
    ESL_PUT_U16(out + ESL_OFF_NEXT_POLL, next_poll_s);
    ESL_PUT_U32(out + ESL_LAYOUT_OFF_VER, ver);
    ESL_PUT_U16(out + ESL_LAYOUT_OFF_SIZE, size);
    ESL_PUT_U16(out + ESL_LAYOUT_OFF_POS, pos);
    memcpy(out + ESL_LAYOUT_OFF_DATA, data, n);
    return (uint8_t)(ESL_LAYOUT_HDR_LEN + n);
}

uint8_t *put_elem(uint8_t *out, uint8_t kind, uint8_t field, uint8_t f0, uint8_t f1, uint8_t f2,
                  uint8_t opts, uint8_t lines, uint8_t color, uint16_t x, uint8_t y, uint16_t w, uint8_t h)
{
    out[ESL_LE_OFF_KIND] = kind;
    out[ESL_LE_OFF_FIELD] = field;
    out[ESL_LE_OFF_FONT] = f0;
    out[ESL_LE_OFF_FONT + 1] = f1;
    out[ESL_LE_OFF_FONT + 2] = f2;
    out[ESL_LE_OFF_OPTS] = opts;
    out[ESL_LE_OFF_LINES] = lines;
    out[ESL_LE_OFF_COLOR] = color;
    ESL_PUT_U16(out + ESL_LE_OFF_X, x);
    out[ESL_LE_OFF_Y] = y;
    ESL_PUT_U16(out + ESL_LE_OFF_W, w);
    out[ESL_LE_OFF_H] = h;
    return out + ESL_LE_SIZE;
}

uint16_t make_test_layout_file(uint8_t *out, uint32_t *ver)
{
    uint8_t *p = out;
    uint32_t crc;
    const uint8_t N = ESL_FONT_NONE;

    *p++ = ESL_LBIN_FORMAT;
    *p++ = MODEL_CODE;
    *p++ = 2;
    *p++ = 0;

    *p++ = 5;
    p = put_elem(p, ESL_LE_RECT, 0, N, N, N, 0, 0, ESL_LE_BLACK, 0, 0, DRAW_W, 30);
    p = put_elem(p, ESL_LE_TEXT, 0, ESL_FONT_MEDIUM, ESL_FONT_SMALL, N, ESL_LE_CENTER | ESL_LE_MIDDLE, 1,
                 ESL_LE_WHITE, 4, 0, DRAW_W - 8, 30);
    p = put_elem(p, ESL_LE_TEXT, 1, ESL_FONT_LARGE, ESL_FONT_MID, ESL_FONT_MEDIUM, ESL_LE_RIGHT | ESL_LE_BOTTOM, 1,
                 ESL_LE_PRICE_HL, 4, 32, DRAW_W - 8, DRAW_H - 32 - 20);
    p = put_elem(p, ESL_LE_TEXT, 2, ESL_FONT_SMALL, N, N, ESL_LE_LEFT, 1, ESL_LE_BLACK,
                 4, DRAW_H - 18, DRAW_W / 2 - 4, 17);
    p = put_elem(p, ESL_LE_TEXT, 3, ESL_FONT_SMALL, N, N, ESL_LE_RIGHT, 1, ESL_LE_BLACK,
                 DRAW_W / 2, DRAW_H - 18, DRAW_W / 2 - 4, 17);

    *p++ = 5;
    p = put_elem(p, ESL_LE_TEXT, 0, ESL_FONT_MEDIUM, ESL_FONT_SMALL, N, ESL_LE_CENTER | ESL_LE_MIDDLE, 0,
                 ESL_LE_BLACK, 8, 8, DRAW_W - 16, DRAW_H - 16);
    p = put_elem(p, ESL_LE_RECT, 0, N, N, N, 0, 0, ESL_LE_FRAME, 0, 0, DRAW_W, 3);
    p = put_elem(p, ESL_LE_RECT, 0, N, N, N, 0, 0, ESL_LE_FRAME, 0, DRAW_H - 3, DRAW_W, 3);
    p = put_elem(p, ESL_LE_RECT, 0, N, N, N, 0, 0, ESL_LE_FRAME, 0, 0, 3, DRAW_H);
    p = put_elem(p, ESL_LE_RECT, 0, N, N, N, 0, 0, ESL_LE_FRAME, DRAW_W - 3, 0, 3, DRAW_H);

    crc = proto_crc32(0, out, (uint16_t)(p - out));
    *ver = crc ? crc : 1;
    return (uint16_t)(p - out);
}
