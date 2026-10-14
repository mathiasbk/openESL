#include "proto_tag.h"

uint8_t proto_build_poll(uint8_t *out, uint16_t fw_ver, uint16_t battery_mv,
                         uint32_t content_id, uint8_t flags)
{
    out[ESL_OFF_TYPE] = ESL_MSG_POLL;
    out[ESL_OFF_VER] = ESL_PROTO_VER;
    ESL_PUT_U16(out + ESL_POLL_OFF_FW, fw_ver);
    ESL_PUT_U16(out + ESL_POLL_OFF_BATT, battery_mv);
    ESL_PUT_U32(out + ESL_POLL_OFF_CID, content_id);
    out[ESL_POLL_OFF_FLAGS] = flags;
    out[ESL_POLL_OFF_RSVD] = 0;
    return ESL_POLL_LEN;
}

int8_t proto_parse_reply(const uint8_t *p, uint8_t len, proto_reply_t *r)
{
    uint8_t i, pos, flen;

    r->nfields = 0;
    for (i = 0; i < ESL_MAX_FIELDS; i++) {
        r->field_off[i] = 0;
        r->field_len[i] = 0;
    }
    r->content_id = 0;
    r->layout = 0;
    r->flags = 0;

    if (len < ESL_IDLE_LEN)
        return PROTO_E_SHORT;
    if (p[ESL_OFF_VER] != ESL_PROTO_VER)
        return PROTO_E_VERSION;
    r->type = p[ESL_OFF_TYPE];
    r->next_poll_s = ESL_GET_U16(p + ESL_OFF_NEXT_POLL);

    if (r->type == ESL_MSG_IDLE)
        return PROTO_OK;
    if (r->type != ESL_MSG_CONTENT)
        return PROTO_E_TYPE;
    if (len > ESL_MAX_PAYLOAD)
        return PROTO_E_LONG;
    if (len < ESL_CONTENT_HDR_LEN)
        return PROTO_E_SHORT;

    r->content_id = ESL_GET_U32(p + ESL_CONTENT_OFF_CID);
    r->layout = p[ESL_CONTENT_OFF_LAYOUT];
    r->flags = p[ESL_CONTENT_OFF_FLAGS];
    if (r->layout != ESL_LAYOUT_PRICE && r->layout != ESL_LAYOUT_TEXT)
        return PROTO_E_LAYOUT;

    pos = ESL_CONTENT_OFF_FIELDS;
    while (r->nfields < ESL_MAX_FIELDS && pos < len) {
        flen = p[pos++];
        if (flen > len - pos)
            return PROTO_E_FIELD;
        r->field_off[r->nfields] = pos;
        r->field_len[r->nfields] = flen;
        r->nfields++;
        pos += flen;
    }
    return PROTO_OK;
}

uint8_t proto_build_frame(uint8_t *out, uint8_t seq, uint16_t pan, const uint8_t *mac,
                          const uint8_t *payload, uint8_t plen)
{
    uint8_t i;

    if (plen > ESL_MAX_PAYLOAD)
        return 0;
    ESL_PUT_U16(out + ESL_TA_OFF_FC, ESL_FC_TAG_TO_AP);
    out[ESL_TA_OFF_SEQ] = seq;
    ESL_PUT_U16(out + ESL_TA_OFF_PAN, pan);
    ESL_PUT_U16(out + ESL_TA_OFF_DST, ESL_AP_SHORT_ADDR);
    for (i = 0; i < MAC_ADDR_LEN; i++)
        out[ESL_TA_OFF_SRC + i] = mac[i];
    for (i = 0; i < plen; i++)
        out[ESL_MAC_HDR_LEN + i] = payload[i];
    return ESL_MAC_HDR_LEN + plen;
}

int8_t proto_check_frame(const uint8_t *f, uint8_t len, uint16_t pan, const uint8_t *mac,
                         uint8_t *payload_off)
{
    uint8_t i;

    if (len < ESL_MAC_HDR_LEN)
        return PROTO_E_SHORT;
    if (len > ESL_MAC_MAX_FRAME - ESL_MAC_FCS_LEN)
        return PROTO_E_LONG;
    if (ESL_GET_U16(f + ESL_AT_OFF_FC) != ESL_FC_AP_TO_TAG)
        return PROTO_E_FRAME;
    if (ESL_GET_U16(f + ESL_AT_OFF_PAN) != pan)
        return PROTO_E_PAN;
    if (ESL_GET_U16(f + ESL_AT_OFF_SRC) != ESL_AP_SHORT_ADDR)
        return PROTO_E_ADDR;
    for (i = 0; i < MAC_ADDR_LEN; i++)
        if (f[ESL_AT_OFF_DST + i] != mac[i])
            return PROTO_E_ADDR;
    *payload_off = ESL_MAC_HDR_LEN;
    return (int8_t)(len - ESL_MAC_HDR_LEN);
}

uint16_t proto_fcs(const uint8_t *p, uint8_t len)
{
    uint16_t crc = 0;
    uint8_t i, b;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : (crc >> 1);
    }
    return crc;
}
