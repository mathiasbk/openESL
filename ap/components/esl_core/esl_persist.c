#include "esl_persist.h"
#include <string.h>

size_t esl_persist_encode(uint64_t mac, const esl_content_t *c, uint8_t *out)
{
    size_t o = 0;
    bool named = c->layout == ESL_LAYOUT_NAMED;
    out[o++] = named ? ESL_PERSIST_VER_NAMED : ESL_PERSIST_VER;
    for (int i = 0; i < 8; i++)
        out[o++] = (uint8_t)(mac >> (8 * i));
    out[o++] = c->layout;
    if (named) {
        size_t nl = strlen(c->name);
        out[o++] = (uint8_t)nl;
        memcpy(&out[o], c->name, nl);
        o += nl;
    }
    out[o++] = c->flags;
    out[o++] = c->nfields;
    for (unsigned i = 0; i < c->nfields; i++)
        out[o++] = c->len[i];
    size_t text = esl_content_text_len(c);
    memcpy(&out[o], c->text, text);
    return o + text;
}

bool esl_persist_decode(const uint8_t *in, size_t len, uint64_t *mac, esl_content_t *c)
{
    if (len < 12 || (in[0] != ESL_PERSIST_VER && in[0] != ESL_PERSIST_VER_NAMED))
        return false;
    uint64_t m = 0;
    for (int i = 8; i >= 1; i--)
        m = (m << 8) | in[i];
    size_t o = 10;
    if (in[0] == ESL_PERSIST_VER_NAMED) {
        size_t nl = in[o++];
        if (in[9] != ESL_LAYOUT_NAMED || o + nl + 2 > len ||
            esl_content_init_named(c, (const char *)&in[o], nl, in[o + nl]) != ESL_CONTENT_OK)
            return false;
        o += nl;
    } else if (esl_content_init(c, in[9], in[10]) != ESL_CONTENT_OK) {
        return false;
    }
    o++; /* flags */
    uint8_t nfields = in[o++];
    if (nfields > ESL_MAX_FIELDS)
        return false;
    size_t t = o + nfields;
    if (t > len)
        return false;
    for (unsigned i = 0; i < nfields; i++) {
        uint8_t fl = in[o + i];
        if (t + fl > len || esl_content_add_field(c, &in[t], fl) != ESL_CONTENT_OK)
            return false;
        t += fl;
    }
    if (t != len)
        return false;
    *mac = m;
    return true;
}

size_t esl_persist_cfg_encode(uint64_t mac, const esl_tag_cfg_t *cfg, uint8_t *out)
{
    const uint16_t v[3] = {cfg->poll_s, cfg->max_poll_s, cfg->lost_ap_s};
    size_t o = 0;
    out[o++] = ESL_PERSIST_CFG_VER;
    for (int i = 0; i < 8; i++)
        out[o++] = (uint8_t)(mac >> (8 * i));
    for (int i = 0; i < 3; i++) {
        out[o++] = (uint8_t)v[i];
        out[o++] = (uint8_t)(v[i] >> 8);
    }
    return o;
}

bool esl_persist_cfg_decode(const uint8_t *in, size_t len, uint64_t *mac, esl_tag_cfg_t *cfg)
{
    if (len != ESL_PERSIST_CFG_LEN || in[0] != ESL_PERSIST_CFG_VER)
        return false;
    uint64_t m = 0;
    for (int i = 8; i >= 1; i--)
        m = (m << 8) | in[i];
    *mac = m;
    cfg->poll_s = (uint16_t)(in[9] | (in[10] << 8));
    cfg->max_poll_s = (uint16_t)(in[11] | (in[12] << 8));
    cfg->lost_ap_s = (uint16_t)(in[13] | (in[14] << 8));
    return true;
}

size_t esl_persist_seen_encode(uint64_t mac, const esl_seen_t *s, uint8_t *out)
{
    size_t o = 0;
    out[o++] = ESL_PERSIST_SEEN_VER;
    for (int i = 0; i < 8; i++)
        out[o++] = (uint8_t)(mac >> (8 * i));
    out[o++] = s->model;
    out[o++] = (uint8_t)s->fw;
    out[o++] = (uint8_t)(s->fw >> 8);
    out[o++] = (uint8_t)s->battery_mv;
    out[o++] = (uint8_t)(s->battery_mv >> 8);
    out[o++] = (uint8_t)s->rssi;
    for (int i = 0; i < 4; i++)
        out[o++] = (uint8_t)(s->unix_s >> (8 * i));
    return o;
}

bool esl_persist_seen_decode(const uint8_t *in, size_t len, uint64_t *mac, esl_seen_t *s)
{
    if (len != ESL_PERSIST_SEEN_LEN || in[0] != ESL_PERSIST_SEEN_VER)
        return false;
    uint64_t m = 0;
    for (int i = 8; i >= 1; i--)
        m = (m << 8) | in[i];
    *mac = m;
    s->model = in[9];
    s->fw = (uint16_t)(in[10] | (in[11] << 8));
    s->battery_mv = (uint16_t)(in[12] | (in[13] << 8));
    s->rssi = (int8_t)in[14];
    s->unix_s = (uint32_t)in[15] | ((uint32_t)in[16] << 8) | ((uint32_t)in[17] << 16) | ((uint32_t)in[18] << 24);
    return true;
}
