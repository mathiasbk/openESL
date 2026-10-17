#include "proto_tag.h"
#include "model.h"

#if MODEL_CODE != MODEL_ESL_CODE
#error "MODEL_CODE in model_gen.h does not match ESL_MODEL_* in shared/models_gen.h (run tools/gen_models.py)"
#endif

uint8_t proto_build_poll(uint8_t *out, uint16_t fw_ver, uint16_t battery_mv,
                         uint32_t content_id, uint8_t flags, uint16_t cfg_ver,
                         uint32_t layout_ver)
{
    out[ESL_OFF_TYPE] = ESL_MSG_POLL;
    out[ESL_OFF_VER] = ESL_PROTO_VER;
    ESL_PUT_U16(out + ESL_POLL_OFF_FW, fw_ver);
    ESL_PUT_U16(out + ESL_POLL_OFF_BATT, battery_mv);
    ESL_PUT_U32(out + ESL_POLL_OFF_CID, content_id);
    out[ESL_POLL_OFF_FLAGS] = flags;
    out[ESL_POLL_OFF_MODEL] = MODEL_CODE;     /* requirement T18 */
    ESL_PUT_U16(out + ESL_POLL_OFF_CFG_VER, cfg_ver);   /* also says: this tag understands CONFIG */
    ESL_PUT_U32(out + ESL_POLL_OFF_LAYOUT_VER, layout_ver);
    return ESL_POLL_LEN_LAYOUT;
}

uint8_t proto_build_layout_req(uint8_t *out, uint32_t layout_ver, uint16_t pos, uint8_t max)
{
    out[ESL_OFF_TYPE] = ESL_MSG_LAYOUT_REQ;
    out[ESL_OFF_VER] = ESL_PROTO_VER;
    out[ESL_LREQ_OFF_MODEL] = MODEL_CODE;
    out[ESL_LREQ_OFF_MAX] = max;
    ESL_PUT_U32(out + ESL_LREQ_OFF_VER, layout_ver);
    ESL_PUT_U16(out + ESL_LREQ_OFF_POS, pos);
    return ESL_LREQ_LEN;
}

uint8_t proto_build_fw_req(uint8_t *out, uint16_t fw_ver, uint16_t pos, uint32_t crc, uint8_t max)
{
    out[ESL_OFF_TYPE] = ESL_MSG_FW_REQ;
    out[ESL_OFF_VER] = ESL_PROTO_VER;
    out[ESL_FREQ_OFF_MODEL] = MODEL_CODE;
    out[ESL_FREQ_OFF_MAX] = max;
    ESL_PUT_U16(out + ESL_FREQ_OFF_VER, fw_ver);
    ESL_PUT_U16(out + ESL_FREQ_OFF_POS, pos);
    ESL_PUT_U32(out + ESL_FREQ_OFF_CRC, crc);
    return ESL_FREQ_LEN;
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
    r->lay_size = 0;
    r->lay_pos = 0;
    r->fw_ver = 0;
    r->fw_crc = 0;
    r->fw_model = 0;
    r->cfg_ver = 0;
    r->cfg_poll_s = 0;
    r->cfg_max_poll_s = 0;
    r->cfg_lost_ap_s = 0;

    if (len < ESL_IDLE_LEN)
        return PROTO_E_SHORT;
    if (p[ESL_OFF_VER] != ESL_PROTO_VER)
        return PROTO_E_VERSION;
    r->type = p[ESL_OFF_TYPE];
    r->next_poll_s = ESL_GET_U16(p + ESL_OFF_NEXT_POLL);

    if (r->type == ESL_MSG_IDLE)
        return PROTO_OK;
    if (len > ESL_MAX_PAYLOAD)
        return PROTO_E_LONG;
    if (r->type == ESL_MSG_LAYOUT) {
        if (len < ESL_LAYOUT_HDR_LEN)
            return PROTO_E_SHORT;
        r->content_id = ESL_GET_U32(p + ESL_LAYOUT_OFF_VER);
        r->lay_size = ESL_GET_U16(p + ESL_LAYOUT_OFF_SIZE);
        r->lay_pos = ESL_GET_U16(p + ESL_LAYOUT_OFF_POS);
        r->field_off[0] = ESL_LAYOUT_OFF_DATA;
        r->field_len[0] = len - ESL_LAYOUT_OFF_DATA;
        if (r->lay_pos > r->lay_size || r->field_len[0] > r->lay_size - r->lay_pos)
            return PROTO_E_FIELD;
        return PROTO_OK;
    }
    if (r->type == ESL_MSG_FW) {
        if (len < ESL_FW_HDR_LEN)
            return PROTO_E_SHORT;
        r->fw_ver = ESL_GET_U16(p + ESL_FW_OFF_VER);
        r->lay_size = ESL_GET_U16(p + ESL_FW_OFF_SIZE);
        r->lay_pos = ESL_GET_U16(p + ESL_FW_OFF_POS);
        r->fw_crc = ESL_GET_U32(p + ESL_FW_OFF_CRC);
        r->fw_model = p[ESL_FW_OFF_MODEL];
        r->field_off[0] = ESL_FW_OFF_DATA;
        r->field_len[0] = len - ESL_FW_OFF_DATA;
        if (r->lay_pos > r->lay_size || r->field_len[0] > r->lay_size - r->lay_pos)
            return PROTO_E_FIELD;
        return PROTO_OK;
    }
    if (r->type == ESL_MSG_CONFIG) {
        if (len < ESL_CONFIG_LEN)
            return PROTO_E_SHORT;
        r->cfg_ver = ESL_GET_U16(p + ESL_CONFIG_OFF_VER);
        r->cfg_poll_s = ESL_GET_U16(p + ESL_CONFIG_OFF_POLL);
        r->cfg_max_poll_s = ESL_GET_U16(p + ESL_CONFIG_OFF_MAX_POLL);
        r->cfg_lost_ap_s = ESL_GET_U16(p + ESL_CONFIG_OFF_LOST_AP);
        return PROTO_OK;
    }
    if (r->type != ESL_MSG_CONTENT)
        return PROTO_E_TYPE;
    if (len < ESL_CONTENT_HDR_LEN)
        return PROTO_E_SHORT;

    r->content_id = ESL_GET_U32(p + ESL_CONTENT_OFF_CID);
    r->layout = p[ESL_CONTENT_OFF_LAYOUT];
    r->flags = p[ESL_CONTENT_OFF_FLAGS];
    if (r->layout != ESL_LAYOUT_PRICE && r->layout != ESL_LAYOUT_TEXT &&
        (r->layout < ESL_LAYOUT_FILE || r->layout >= ESL_LAYOUT_FILE + ESL_LAYOUT_FILE_MAX))
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

/* Four bits at a time: 64 bytes of table instead of 1 KB, 2 steps per byte. */
static const uint32_t __code crc_nib[16] = {
    0x00000000ul, 0x1DB71064ul, 0x3B6E20C8ul, 0x26D930ACul,
    0x76DC4190ul, 0x6B6B51F4ul, 0x4DB26158ul, 0x5005713Cul,
    0xEDB88320ul, 0xF00F9344ul, 0xD6D6A3E8ul, 0xCB61B38Cul,
    0x9B64C2B0ul, 0x86D3D2D4ul, 0xA00AE278ul, 0xBDBDF21Cul
};

uint32_t proto_crc32(uint32_t crc, const uint8_t *p, uint16_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc_nib[(uint8_t)crc & 0x0F];
        crc = (crc >> 4) ^ crc_nib[(uint8_t)crc & 0x0F];
    }
    return ~crc;
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
