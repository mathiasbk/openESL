#include "esl_proto.h"
#include <string.h>

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
    out->fw_ver = get16(&p[2]);
    out->battery_mv = get16(&p[4]);
    out->content_id = get32(&p[6]);
    out->flags = p[10];
    return true;
}

size_t esl_poll_build(uint8_t *out, const esl_poll_t *poll)
{
    out[0] = ESL_MSG_POLL;
    out[1] = ESL_PROTO_VER;
    put16(&out[2], poll->fw_ver);
    put16(&out[4], poll->battery_mv);
    put32(&out[6], poll->content_id);
    out[10] = poll->flags;
    out[11] = 0;
    return ESL_POLL_LEN;
}

size_t esl_idle_build(uint8_t *out, uint16_t next_poll_s)
{
    out[0] = ESL_MSG_IDLE;
    out[1] = ESL_PROTO_VER;
    put16(&out[2], next_poll_s);
    return ESL_IDLE_LEN;
}

size_t esl_content_msg_build(uint8_t *out, uint16_t next_poll_s, uint32_t content_id, const esl_content_t *c)
{
    out[0] = ESL_MSG_CONTENT;
    out[1] = ESL_PROTO_VER;
    put16(&out[2], next_poll_s);
    put32(&out[4], content_id);
    return ESL_CONTENT_HDR_LEN + esl_content_body(c, &out[ESL_CONTENT_HDR_LEN]);
}

bool esl_frame_parse_from_tag(const uint8_t *psdu, size_t psdu_len, esl_rx_frame_t *out)
{
    if (psdu_len < ESL_MHR_LEN + ESL_FCS_LEN || psdu_len > ESL_PSDU_MAX)
        return false;
    uint16_t fcf = get16(psdu);
    /* Ignore frame pending, ACK request and frame version bits. */
    if ((fcf & ~(uint16_t)0x3030) != (ESL_FCF_TAG_TO_AP & ~(uint16_t)0x3030))
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
