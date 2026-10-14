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
