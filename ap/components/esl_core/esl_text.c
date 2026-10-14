#include "esl_text.h"
#include <string.h>

static uint8_t map_codepoint(uint32_t cp)
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
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015:
    case 0x2212:
        return '-';
    case 0x2018: case 0x2019: case 0x201A: case 0x2032:
        return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033:
        return '"';
    case 0x2022:
        return 0xB7; /* bullet -> middle dot */
    default:
        return '?';
    }
}

/* Characters that become more than one byte. Returns the byte count written
 * to out (at most 3), or 0 if cp is not one of them. */
static size_t expand_codepoint(uint32_t cp, uint8_t out[3])
{
    if (cp == 0x2026) { /* horizontal ellipsis -> "..." */
        out[0] = out[1] = out[2] = '.';
        return 3;
    }
    return 0;
}

/* Decodes one code point at in[*i]. Returns false (and advances one byte) on
 * invalid or truncated sequences so the decoder resynchronises. */
static bool decode_one(const uint8_t *in, size_t len, size_t *i, uint32_t *cp)
{
    uint8_t b = in[*i];
    size_t n;
    uint32_t v, min;

    if (b < 0x80) {
        *cp = b;
        *i += 1;
        return true;
    } else if ((b & 0xE0) == 0xC0) {
        n = 2; v = b & 0x1F; min = 0x80;
    } else if ((b & 0xF0) == 0xE0) {
        n = 3; v = b & 0x0F; min = 0x800;
    } else if ((b & 0xF8) == 0xF0) {
        n = 4; v = b & 0x07; min = 0x10000;
    } else {
        *i += 1;
        return false;
    }
    if (*i + n > len) {
        *i += 1;
        return false;
    }
    for (size_t k = 1; k < n; k++) {
        uint8_t c = in[*i + k];
        if ((c & 0xC0) != 0x80) {
            *i += 1;
            return false;
        }
        v = (v << 6) | (c & 0x3F);
    }
    *i += n;
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
        return false;
    *cp = v;
    return true;
}

size_t esl_utf8_to_latin1(const char *in, size_t in_len, uint8_t *out, size_t out_max, bool *overflow)
{
    const uint8_t *p = (const uint8_t *)in;
    size_t i = 0, o = 0;

    if (overflow)
        *overflow = false;
    while (i < in_len) {
        uint32_t cp = 0;
        uint8_t buf[3];
        size_t n = 1;
        if (decode_one(p, in_len, &i, &cp)) {
            n = expand_codepoint(cp, buf);
            if (n == 0) {
                buf[0] = map_codepoint(cp);
                n = 1;
            }
        } else {
            buf[0] = '?';
        }
        if (o + n > out_max) {
            if (overflow)
                *overflow = true;
            break;
        }
        memcpy(&out[o], buf, n);
        o += n;
    }
    return o;
}
