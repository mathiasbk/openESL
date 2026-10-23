#include "esl_text.h"
#include <string.h>
#include "proto.h" /* shared/proto.h */

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

#define NUM_INT_MAX  12 /* digits before the decimal sign */
#define NUM_FRAC_MAX 6  /* and after it: 10^18 still fits in 64 bits */

size_t esl_format_number(const uint8_t *in, size_t len, uint8_t opts, uint8_t *out, size_t cap)
{
    uint8_t mode = opts & ESL_LE_NUM_MASK;
    size_t a = 0, b = len, dec = len, i;
    unsigned nint = 0, nfrac = 0, nd, commas = 0, dots = 0;
    bool neg = false, dash = false;
    uint64_t v = 0, p;

    if (mode < ESL_LE_NUM_KEEP || mode > ESL_LE_NUM_DASH || cap < ESL_LE_NUM_MAX_LEN)
        return 0;
    while (a < b && in[a] == ' ')
        a++;
    while (b > a && in[b - 1] == ' ')
        b--;
    if (a < b && in[a] == '-') {
        neg = true;
        a++;
    }
    if (b - a >= 2 && in[b - 1] == '-' && (in[b - 2] == ',' || in[b - 2] == '.')) {
        dash = true; /* "199,-" */
        b -= 2;
    }
    for (i = a; i < b; i++) {
        if (in[i] == ',')
            commas++, dec = i;
        else if (in[i] == '.')
            dots++;
        else if ((in[i] < '0' || in[i] > '9') && in[i] != ' ')
            return 0;
    }
    /* With a comma it is the decimal sign and dots group thousands (1.299,90);
     * without, one dot is the decimal sign (1299.9) and several group thousands, as
     * does a dot before ",-" (1.299,-). */
    if (commas > 1 || (dash && commas))
        return 0;
    if (!commas && dots == 1 && !dash)
        for (i = a; i < b; i++)
            if (in[i] == '.')
                dec = i;
    if (dec < b)
        for (i = dec + 1; i < b; i++)
            if (in[i] == '.' || in[i] == ' ')
                return 0;
    for (i = a; i < b; i++) {
        if (in[i] < '0' || in[i] > '9')
            continue;
        if (i < dec ? ++nint > NUM_INT_MAX : ++nfrac > NUM_FRAC_MAX)
            return 0;
        v = v * 10 + (uint64_t)(in[i] - '0');
    }
    if (!nint && !nfrac)
        return 0;

    nd = mode == ESL_LE_NUM_KEEP ? nfrac : mode == ESL_LE_NUM_0 ? 0 : 2;
    for (; nfrac > nd; nfrac--)
        v = nfrac == nd + 1 ? (v + 5) / 10 : v / 10; /* rounds half up at the last digit dropped */
    for (; nfrac < nd; nfrac++)
        v *= 10;
    for (p = 1, i = 0; i < nd; i++)
        p *= 10;
    if (nd && v % p == 0 && (mode == ESL_LE_NUM_AUTO || mode == ESL_LE_NUM_DASH)) {
        dash = mode == ESL_LE_NUM_DASH;
        v /= p;
        nd = 0;
        p = 1;
    } else if (mode != ESL_LE_NUM_KEEP) {
        dash = false;
    }

    char digits[24];
    uint64_t whole = v / p, frac = v % p;
    unsigned nw = 0;
    do {
        digits[nw++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);

    size_t n = 0;
    if (neg && v)
        out[n++] = '-';
    for (i = nw; i-- > 0;) {
        out[n++] = (uint8_t)digits[i];
        if ((opts & ESL_LE_NUM_THOUSANDS) && i && i % 3 == 0 && nw > 3)
            out[n++] = ' ';
    }
    if (nd) {
        out[n++] = ',';
        for (i = nd; i-- > 0;) {
            out[n + i] = (uint8_t)('0' + frac % 10);
            frac /= 10;
        }
        n += nd;
    } else if (dash) {
        out[n++] = ',';
        out[n++] = '-';
    }
    return n;
}
