#include "esl_proto.h"
#include "esl_crc32.h"
#include <string.h>

/* The CONTENT header in shared/proto.h counts layout and flags; the AP builds
 * up to content_id and lets esl_content_body() add the rest. */
_Static_assert(ESL_CONTENT_OFF_LAYOUT + 2 == ESL_CONTENT_HDR_LEN, "CONTENT layout");
_Static_assert(ESL_POLL_OFF_CFG_VER == ESL_POLL_LEN && ESL_POLL_LEN_CFG == ESL_POLL_LEN + 2, "POLL extension");
_Static_assert(ESL_CONFIG_OFF_LOST_AP + 2 == ESL_CONFIG_LEN, "CONFIG layout");
_Static_assert(ESL_MHR_LEN == 15 && ESL_PSDU_MAX == 127, "802.15.4 frame");

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

bool esl_poll_parse(const uint8_t *p, size_t len, esl_poll_t *out)
{
    if (len < ESL_POLL_LEN || p[0] != ESL_MSG_POLL || p[1] != ESL_PROTO_VER)
        return false;
    out->fw_ver = get16(&p[ESL_POLL_OFF_FW]);
    out->battery_mv = get16(&p[ESL_POLL_OFF_BATT]);
    out->content_id = get32(&p[ESL_POLL_OFF_CID]);
    out->flags = p[ESL_POLL_OFF_FLAGS];
    out->model = p[ESL_POLL_OFF_MODEL];
    out->has_cfg_ver = len >= ESL_POLL_LEN_CFG;
    out->cfg_ver = out->has_cfg_ver ? get16(&p[ESL_POLL_OFF_CFG_VER]) : 0;
    out->has_layout_ver = len >= ESL_POLL_LEN_LAYOUT;
    out->layout_ver = out->has_layout_ver ? get32(&p[ESL_POLL_OFF_LAYOUT_VER]) : 0;
    return true;
}

size_t esl_poll_build(uint8_t *out, const esl_poll_t *poll)
{
    out[0] = ESL_MSG_POLL;
    out[1] = ESL_PROTO_VER;
    put16(&out[ESL_POLL_OFF_FW], poll->fw_ver);
    put16(&out[ESL_POLL_OFF_BATT], poll->battery_mv);
    put32(&out[ESL_POLL_OFF_CID], poll->content_id);
    out[ESL_POLL_OFF_FLAGS] = poll->flags;
    out[ESL_POLL_OFF_MODEL] = poll->model;
    if (!poll->has_cfg_ver && !poll->has_layout_ver)
        return ESL_POLL_LEN;
    put16(&out[ESL_POLL_OFF_CFG_VER], poll->has_cfg_ver ? poll->cfg_ver : 0);
    if (!poll->has_layout_ver)
        return ESL_POLL_LEN_CFG;
    put32(&out[ESL_POLL_OFF_LAYOUT_VER], poll->layout_ver);
    return ESL_POLL_LEN_LAYOUT;
}

bool esl_layout_req_parse(const uint8_t *p, size_t len, esl_layout_req_t *out)
{
    if (len < ESL_LREQ_LEN || p[0] != ESL_MSG_LAYOUT_REQ || p[1] != ESL_PROTO_VER)
        return false;
    out->model = p[ESL_LREQ_OFF_MODEL];
    out->max = p[ESL_LREQ_OFF_MAX];
    out->layout_ver = get32(&p[ESL_LREQ_OFF_VER]);
    out->pos = get16(&p[ESL_LREQ_OFF_POS]);
    return true;
}

size_t esl_layout_req_build(uint8_t *out, const esl_layout_req_t *req)
{
    out[0] = ESL_MSG_LAYOUT_REQ;
    out[1] = ESL_PROTO_VER;
    out[ESL_LREQ_OFF_MODEL] = req->model;
    out[ESL_LREQ_OFF_MAX] = req->max;
    put32(&out[ESL_LREQ_OFF_VER], req->layout_ver);
    put16(&out[ESL_LREQ_OFF_POS], req->pos);
    return ESL_LREQ_LEN;
}

bool esl_fw_req_parse(const uint8_t *p, size_t len, esl_fw_req_t *out)
{
    if (len < ESL_FREQ_LEN || p[0] != ESL_MSG_FW_REQ || p[1] != ESL_PROTO_VER)
        return false;
    out->model = p[ESL_FREQ_OFF_MODEL];
    out->max = p[ESL_FREQ_OFF_MAX];
    out->ver = get16(&p[ESL_FREQ_OFF_VER]);
    out->pos = get16(&p[ESL_FREQ_OFF_POS]);
    out->crc = get32(&p[ESL_FREQ_OFF_CRC]);
    return true;
}

size_t esl_fw_req_build(uint8_t *out, const esl_fw_req_t *req)
{
    out[0] = ESL_MSG_FW_REQ;
    out[1] = ESL_PROTO_VER;
    out[ESL_FREQ_OFF_MODEL] = req->model;
    out[ESL_FREQ_OFF_MAX] = req->max;
    put16(&out[ESL_FREQ_OFF_VER], req->ver);
    put16(&out[ESL_FREQ_OFF_POS], req->pos);
    put32(&out[ESL_FREQ_OFF_CRC], req->crc);
    return ESL_FREQ_LEN;
}

size_t esl_fw_msg_build(uint8_t *out, uint16_t next_poll_s, uint16_t ver, uint16_t size, uint16_t pos,
                        uint32_t crc, uint8_t model, const uint8_t *data, size_t n)
{
    if (n > ESL_FW_CHUNK_MAX)
        n = ESL_FW_CHUNK_MAX;
    out[0] = ESL_MSG_FW;
    out[1] = ESL_PROTO_VER;
    put16(&out[ESL_OFF_NEXT_POLL], next_poll_s);
    put16(&out[ESL_FW_OFF_VER], ver);
    put16(&out[ESL_FW_OFF_SIZE], size);
    put16(&out[ESL_FW_OFF_POS], pos);
    put32(&out[ESL_FW_OFF_CRC], crc);
    out[ESL_FW_OFF_MODEL] = model;
    if (n)
        memcpy(&out[ESL_FW_OFF_DATA], data, n);
    return ESL_FW_HDR_LEN + n;
}

size_t esl_layout_msg_build(uint8_t *out, uint16_t next_poll_s, uint32_t ver, uint16_t size, uint16_t pos,
                            const uint8_t *data, size_t n)
{
    if (n > ESL_LAYOUT_CHUNK_MAX)
        n = ESL_LAYOUT_CHUNK_MAX;
    out[0] = ESL_MSG_LAYOUT;
    out[1] = ESL_PROTO_VER;
    put16(&out[ESL_OFF_NEXT_POLL], next_poll_s);
    put32(&out[ESL_LAYOUT_OFF_VER], ver);
    put16(&out[ESL_LAYOUT_OFF_SIZE], size);
    put16(&out[ESL_LAYOUT_OFF_POS], pos);
    if (n)
        memcpy(&out[ESL_LAYOUT_OFF_DATA], data, n);
    return ESL_LAYOUT_HDR_LEN + n;
}

bool esl_tag_cfg_is_default(const esl_tag_cfg_t *c)
{
    return !c->poll_s && !c->max_poll_s && !c->lost_ap_s;
}

uint16_t esl_tag_cfg_version(const esl_tag_cfg_t *c)
{
    uint8_t b[6];
    if (esl_tag_cfg_is_default(c))
        return 0;
    put16(&b[0], c->poll_s);
    put16(&b[2], c->max_poll_s);
    put16(&b[4], c->lost_ap_s);
    uint32_t crc = esl_crc32(b, sizeof(b));
    uint16_t v = (uint16_t)(crc ^ (crc >> 16));
    return v ? v : 1;
}

size_t esl_config_msg_build(uint8_t *out, uint16_t next_poll_s, const esl_tag_cfg_t *c)
{
    out[0] = ESL_MSG_CONFIG;
    out[1] = ESL_PROTO_VER;
    put16(&out[ESL_OFF_NEXT_POLL], next_poll_s);
    put16(&out[ESL_CONFIG_OFF_VER], esl_tag_cfg_version(c));
    put16(&out[ESL_CONFIG_OFF_POLL], c->poll_s);
    put16(&out[ESL_CONFIG_OFF_MAX_POLL], c->max_poll_s);
    put16(&out[ESL_CONFIG_OFF_LOST_AP], c->lost_ap_s);
    return ESL_CONFIG_LEN;
}

size_t esl_idle_build(uint8_t *out, uint16_t next_poll_s)
{
    out[0] = ESL_MSG_IDLE;
    out[1] = ESL_PROTO_VER;
    put16(&out[ESL_OFF_NEXT_POLL], next_poll_s);
    return ESL_IDLE_LEN;
}

size_t esl_content_msg_build(uint8_t *out, uint16_t next_poll_s, uint32_t content_id, const esl_content_t *c)
{
    out[0] = ESL_MSG_CONTENT;
    out[1] = ESL_PROTO_VER;
    put16(&out[ESL_OFF_NEXT_POLL], next_poll_s);
    put32(&out[ESL_CONTENT_OFF_CID], content_id);
    return ESL_CONTENT_OFF_LAYOUT + esl_content_body(c, &out[ESL_CONTENT_OFF_LAYOUT]);
}

bool esl_reply_parse(const uint8_t *p, size_t len, esl_reply_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len < ESL_IDLE_LEN || p[ESL_OFF_VER] != ESL_PROTO_VER)
        return false;
    out->type = p[ESL_OFF_TYPE];
    out->next_poll_s = get16(&p[ESL_OFF_NEXT_POLL]);
    if (out->type == ESL_MSG_IDLE)
        return len == ESL_IDLE_LEN;
    if (out->type == ESL_MSG_LAYOUT) {
        if (len < ESL_LAYOUT_HDR_LEN || len > ESL_MAX_PAYLOAD)
            return false;
        out->layout_ver = get32(&p[ESL_LAYOUT_OFF_VER]);
        out->layout_size = get16(&p[ESL_LAYOUT_OFF_SIZE]);
        out->layout_pos = get16(&p[ESL_LAYOUT_OFF_POS]);
        out->layout_data = &p[ESL_LAYOUT_OFF_DATA];
        out->layout_len = len - ESL_LAYOUT_HDR_LEN;
        return out->layout_pos <= out->layout_size &&
               out->layout_len <= (size_t)(out->layout_size - out->layout_pos) &&
               (out->layout_ver != 0 || out->layout_size == 0);
    }
    if (out->type == ESL_MSG_FW) {
        if (len < ESL_FW_HDR_LEN || len > ESL_MAX_PAYLOAD)
            return false;
        out->fw_ver = get16(&p[ESL_FW_OFF_VER]);
        out->layout_size = get16(&p[ESL_FW_OFF_SIZE]);
        out->layout_pos = get16(&p[ESL_FW_OFF_POS]);
        out->fw_crc = get32(&p[ESL_FW_OFF_CRC]);
        out->fw_model = p[ESL_FW_OFF_MODEL];
        out->layout_data = &p[ESL_FW_OFF_DATA];
        out->layout_len = len - ESL_FW_HDR_LEN;
        return out->fw_ver != 0 && out->layout_pos <= out->layout_size &&
               out->layout_len <= (size_t)(out->layout_size - out->layout_pos);
    }
    if (out->type == ESL_MSG_CONFIG) {
        if (len < ESL_CONFIG_LEN)
            return false;
        out->cfg_ver = get16(&p[ESL_CONFIG_OFF_VER]);
        out->cfg.poll_s = get16(&p[ESL_CONFIG_OFF_POLL]);
        out->cfg.max_poll_s = get16(&p[ESL_CONFIG_OFF_MAX_POLL]);
        out->cfg.lost_ap_s = get16(&p[ESL_CONFIG_OFF_LOST_AP]);
        return true;
    }
    if (out->type != ESL_MSG_CONTENT || len < ESL_CONTENT_HDR_LEN)
        return false;
    out->content_id = get32(&p[ESL_CONTENT_OFF_CID]);
    if (esl_content_init(&out->content, p[ESL_CONTENT_OFF_LAYOUT], p[ESL_CONTENT_OFF_FLAGS]) != ESL_CONTENT_OK)
        return false;
    size_t o = ESL_CONTENT_OFF_FIELDS;
    while (o < len) {
        uint8_t fl = p[o++];
        if (fl > len - o || esl_content_add_field(&out->content, &p[o], fl) != ESL_CONTENT_OK)
            return false;
        o += fl;
    }
    return true;
}

static bool fcf_matches(uint16_t fcf, uint16_t want)
{
    /* Ignore frame pending, ACK request and frame version bits. */
    return (fcf & ~(uint16_t)0x3030) == (want & ~(uint16_t)0x3030);
}

bool esl_frame_parse_from_tag(const uint8_t *psdu, size_t psdu_len, esl_rx_frame_t *out)
{
    if (psdu_len < ESL_MHR_LEN + ESL_FCS_LEN || psdu_len > ESL_PSDU_MAX)
        return false;
    if (!fcf_matches(get16(psdu), ESL_FCF_TAG_TO_AP))
        return false;
    out->seq = psdu[2];
    out->dst_pan = get16(&psdu[3]);
    out->dst_short = get16(&psdu[5]);
    out->src_mac = get64(&psdu[7]);
    out->payload = &psdu[ESL_MHR_LEN];
    out->payload_len = psdu_len - ESL_MHR_LEN - ESL_FCS_LEN;
    return true;
}

size_t esl_frame_build_to_tag(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t src_short,
                              uint64_t dst_mac, const uint8_t *payload, size_t payload_len)
{
    if (ESL_MHR_LEN + payload_len + ESL_FCS_LEN > ESL_PSDU_MAX)
        return 0;
    put16(&out[0], ESL_FCF_AP_TO_TAG);
    out[2] = seq;
    put16(&out[3], pan);
    put64(&out[5], dst_mac);
    put16(&out[13], src_short);
    memcpy(&out[ESL_MHR_LEN], payload, payload_len);
    memset(&out[ESL_MHR_LEN + payload_len], 0, ESL_FCS_LEN);
    return ESL_MHR_LEN + payload_len + ESL_FCS_LEN;
}

size_t esl_frame_build_from_tag(uint8_t *out, uint8_t seq, uint16_t pan, uint16_t dst_short,
                                uint64_t src_mac, const uint8_t *payload, size_t payload_len)
{
    if (ESL_MHR_LEN + payload_len + ESL_FCS_LEN > ESL_PSDU_MAX)
        return 0;
    put16(&out[0], ESL_FCF_TAG_TO_AP);
    out[2] = seq;
    put16(&out[3], pan);
    put16(&out[5], dst_short);
    put64(&out[7], src_mac);
    memcpy(&out[ESL_MHR_LEN], payload, payload_len);
    memset(&out[ESL_MHR_LEN + payload_len], 0, ESL_FCS_LEN);
    return ESL_MHR_LEN + payload_len + ESL_FCS_LEN;
}

bool esl_frame_parse_to_tag(const uint8_t *psdu, size_t psdu_len, esl_ap_frame_t *out)
{
    if (psdu_len < ESL_MHR_LEN + ESL_FCS_LEN || psdu_len > ESL_PSDU_MAX)
        return false;
    if (!fcf_matches(get16(psdu), ESL_FCF_AP_TO_TAG))
        return false;
    out->seq = psdu[ESL_AT_OFF_SEQ];
    out->dst_pan = get16(&psdu[ESL_AT_OFF_PAN]);
    out->dst_mac = get64(&psdu[ESL_AT_OFF_DST]);
    out->src_short = get16(&psdu[ESL_AT_OFF_SRC]);
    out->payload = &psdu[ESL_MHR_LEN];
    out->payload_len = psdu_len - ESL_MHR_LEN - ESL_FCS_LEN;
    return true;
}

bool esl_mac_parse(const char *s, size_t len, uint64_t *out)
{
    uint64_t v = 0;
    if (len != 16)
        return false;
    for (size_t i = 0; i < 16; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9')
            d = (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F')
            d = (unsigned)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f')
            d = (unsigned)(c - 'a' + 10);
        else
            return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

void esl_mac_format(uint64_t mac, char out[17])
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) {
        out[i] = hex[mac & 0xF];
        mac >>= 4;
    }
    out[16] = '\0';
}

const char *esl_model_name(uint8_t model)
{
    switch (model) {
    case ESL_MODEL_29:  return "2.9";
    case ESL_MODEL_154: return "1.54";
    default:            return NULL;
    }
}

bool esl_model_size(uint8_t model, uint16_t *w, uint16_t *h)
{
    switch (model) {
    case ESL_MODEL_29:  *w = ESL_MODEL_29_W;  *h = ESL_MODEL_29_H;  return true;
    case ESL_MODEL_154: *w = ESL_MODEL_154_W; *h = ESL_MODEL_154_H; return true;
    default:            return false;
    }
}

uint8_t esl_model_parse(const char *s, size_t len)
{
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
        const char *name = esl_model_name(m);
        if (strlen(name) == len && memcmp(name, s, len) == 0)
            return m;
    }
    return ESL_MODEL_UNKNOWN;
}
