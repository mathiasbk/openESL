#include "esl_content.h"
#include "esl_crc32.h"
#include <string.h>

int esl_content_max_fields(uint8_t layout)
{
    switch (layout) {
    case ESL_LAYOUT_PRICE: return 3;
    case ESL_LAYOUT_TEXT:  return 4;
    case ESL_LAYOUT_NAMED: return ESL_MAX_FIELDS;
    default:
        return layout >= ESL_LAYOUT_FILE && layout < ESL_LAYOUT_FILE + ESL_LAYOUT_FILE_MAX ? ESL_MAX_FIELDS : 0;
    }
}

esl_content_err_t esl_content_init(esl_content_t *c, uint8_t layout, uint8_t flags)
{
    memset(c, 0, sizeof(*c));
    if (esl_content_max_fields(layout) == 0 || layout == ESL_LAYOUT_NAMED)
        return ESL_CONTENT_ERR_LAYOUT;
    c->layout = layout;
    c->flags = flags;
    return ESL_CONTENT_OK;
}

bool esl_layout_name_valid(const char *s, size_t len)
{
    if (len == 0 || len > ESL_LAYOUT_NAME_MAX)
        return false;
    if ((len == 5 && memcmp(s, "price", 5) == 0) || (len == 4 && memcmp(s, "text", 4) == 0))
        return false;
    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
        if (!alnum && (i == 0 || (ch != '.' && ch != '_' && ch != '-')))
            return false;
    }
    return true;
}

esl_content_err_t esl_content_init_named(esl_content_t *c, const char *name, size_t len, uint8_t flags)
{
    memset(c, 0, sizeof(*c));
    if (!esl_layout_name_valid(name, len))
        return ESL_CONTENT_ERR_LAYOUT;
    c->layout = ESL_LAYOUT_NAMED;
    c->flags = flags;
    memcpy(c->name, name, len);
    return ESL_CONTENT_OK;
}

void esl_content_resolve(const esl_content_t *c, uint8_t index, esl_content_t *out)
{
    *out = *c;
    if (c->layout == ESL_LAYOUT_NAMED) {
        out->layout = (uint8_t)(ESL_LAYOUT_FILE + index);
        memset(out->name, 0, sizeof(out->name));
    }
}

size_t esl_content_text_len(const esl_content_t *c)
{
    size_t n = 0;
    for (unsigned i = 0; i < c->nfields; i++)
        n += c->len[i];
    return n;
}

esl_content_err_t esl_content_add_field(esl_content_t *c, const uint8_t *latin1, size_t len)
{
    if (c->nfields >= esl_content_max_fields(c->layout))
        return ESL_CONTENT_ERR_TOO_MANY_FIELDS;
    size_t used = esl_content_text_len(c);
    if (used + len + c->nfields + 1 > ESL_CONTENT_FIELD_SPACE)
        return ESL_CONTENT_ERR_TOO_LONG;
    memcpy(&c->text[used], latin1, len);
    c->len[c->nfields++] = (uint8_t)len;
    return ESL_CONTENT_OK;
}

void esl_content_trim(esl_content_t *c)
{
    while (c->nfields > 0 && c->len[c->nfields - 1] == 0)
        c->nfields--;
}

const uint8_t *esl_content_field(const esl_content_t *c, unsigned idx, uint8_t *len)
{
    size_t off = 0;
    if (idx >= c->nfields) {
        *len = 0;
        return c->text;
    }
    for (unsigned i = 0; i < idx; i++)
        off += c->len[i];
    *len = c->len[idx];
    return &c->text[off];
}

size_t esl_content_body(const esl_content_t *c, uint8_t *out)
{
    size_t o = 0, off = 0;
    out[o++] = c->layout;
    if (c->layout == ESL_LAYOUT_NAMED) {
        size_t nl = 0;
        while (nl < ESL_LAYOUT_NAME_MAX && c->name[nl])
            nl++;
        out[o++] = (uint8_t)nl;
        memcpy(&out[o], c->name, nl);
        o += nl;
    }
    out[o++] = c->flags;
    for (unsigned i = 0; i < c->nfields; i++) {
        out[o++] = c->len[i];
        memcpy(&out[o], &c->text[off], c->len[i]);
        o += c->len[i];
        off += c->len[i];
    }
    return o;
}

uint32_t esl_content_id(const esl_content_t *c)
{
    uint8_t body[ESL_CONTENT_BODY_MAX];
    size_t n = esl_content_body(c, body);
    uint32_t id = esl_crc32(body, n);
    return id ? id : 1;
}

bool esl_content_equal(const esl_content_t *a, const esl_content_t *b)
{
    uint8_t ba[ESL_CONTENT_BODY_MAX], bb[ESL_CONTENT_BODY_MAX];
    size_t na = esl_content_body(a, ba);
    size_t nb = esl_content_body(b, bb);
    return na == nb && memcmp(ba, bb, na) == 0;
}
