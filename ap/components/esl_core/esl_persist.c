#include "esl_persist.h"
#include <string.h>

size_t esl_persist_encode(uint64_t mac, const esl_content_t *c, uint8_t *out)
{
    size_t o = 0;
    out[o++] = ESL_PERSIST_VER;
    for (int i = 0; i < 8; i++)
        out[o++] = (uint8_t)(mac >> (8 * i));
    out[o++] = c->layout;
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
    if (len < 12 || in[0] != ESL_PERSIST_VER)
        return false;
    uint64_t m = 0;
    for (int i = 8; i >= 1; i--)
        m = (m << 8) | in[i];
    uint8_t nfields = in[11];
    if (esl_content_init(c, in[9], in[10]) != ESL_CONTENT_OK || nfields > ESL_MAX_FIELDS)
        return false;
    size_t o = 12 + nfields;
    if (o > len)
        return false;
    for (unsigned i = 0; i < nfields; i++) {
        uint8_t fl = in[12 + i];
        if (o + fl > len || esl_content_add_field(c, &in[o], fl) != ESL_CONTENT_OK)
            return false;
        o += fl;
    }
    if (o != len)
        return false;
    *mac = m;
    return true;
}
