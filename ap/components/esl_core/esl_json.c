#include "esl_json.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
    S_VALUE,
    S_VALUE_OR_END, /* just after '[' */
    S_KEY,          /* after ',' in an object */
    S_KEY_OR_END,   /* just after '{' */
    S_COLON,
    S_COMMA_OR_END,
    S_DONE,
};

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static unsigned hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (unsigned)(c - 'a' + 10);
    return (unsigned)(c - 'A' + 10);
}

/* Scans a string starting at the opening quote js[*i]. On success *i points
 * past the closing quote and [*start, *end) is the raw content. */
static bool scan_string(const char *js, size_t len, size_t *i, size_t *start, size_t *end)
{
    size_t j = *i + 1;
    while (j < len) {
        unsigned char ch = (unsigned char)js[j];
        if (ch == '"') {
            *start = *i + 1;
            *end = j;
            *i = j + 1;
            return true;
        }
        if (ch < 0x20)
            return false;
        if (ch == '\\') {
            if (++j >= len)
                return false;
            char e = js[j];
            if (e == 'u') {
                if (j + 4 >= len)
                    return false;
                for (int k = 1; k <= 4; k++)
                    if (!is_hex(js[j + k]))
                        return false;
                j += 4;
            } else if (!strchr("\"\\/bfnrt", e) || e == '\0') {
                return false;
            }
        }
        j++;
    }
    return false;
}

int esl_json_parse(const char *js, size_t len, esl_jtok_t *tok, unsigned max_tokens)
{
    int stack[ESL_JSON_MAX_DEPTH];
    int depth = 0;
    unsigned n = 0;
    int state = S_VALUE;
    size_t i = 0;

    if (len > ESL_JSON_MAX_LEN)
        return ESL_JSON_ERR_TOO_BIG;

    while (i < len) {
        char c = js[i];
        if (is_ws(c)) {
            i++;
            continue;
        }
        switch (state) {
        case S_DONE:
            return ESL_JSON_ERR_SYNTAX;

        case S_COLON:
            if (c != ':')
                return ESL_JSON_ERR_SYNTAX;
            i++;
            state = S_VALUE;
            continue;

        case S_KEY_OR_END:
        case S_KEY:
        case S_COMMA_OR_END:
        case S_VALUE_OR_END:
            if ((c == '}' && (state == S_KEY_OR_END || state == S_COMMA_OR_END)) ||
                (c == ']' && (state == S_VALUE_OR_END || state == S_COMMA_OR_END))) {
                if (depth == 0)
                    return ESL_JSON_ERR_SYNTAX;
                int top = stack[depth - 1];
                if (tok[top].type != (c == '}' ? ESL_JSON_OBJECT : ESL_JSON_ARRAY))
                    return ESL_JSON_ERR_SYNTAX;
                tok[top].end = (uint16_t)(i + 1);
                tok[top].next = (uint16_t)n;
                depth--;
                i++;
                state = depth == 0 ? S_DONE : S_COMMA_OR_END;
                continue;
            }
            if (state == S_COMMA_OR_END) {
                if (c != ',')
                    return ESL_JSON_ERR_SYNTAX;
                i++;
                state = tok[stack[depth - 1]].type == ESL_JSON_OBJECT ? S_KEY : S_VALUE;
                continue;
            }
            if (state == S_KEY || state == S_KEY_OR_END) {
                size_t s, e;
                if (c != '"' || !scan_string(js, len, &i, &s, &e))
                    return ESL_JSON_ERR_SYNTAX;
                if (n >= max_tokens)
                    return ESL_JSON_ERR_NOMEM;
                tok[n] = (esl_jtok_t){ESL_JSON_STRING, (uint16_t)s, (uint16_t)e, 0, (uint16_t)(n + 1)};
                n++;
                tok[stack[depth - 1]].size++;
                state = S_COLON;
                continue;
            }
            /* S_VALUE_OR_END with a value: fall through to value parsing */
            /* fall through */
        case S_VALUE: {
            if (n >= max_tokens)
                return ESL_JSON_ERR_NOMEM;
            if (depth > 0 && tok[stack[depth - 1]].type == ESL_JSON_ARRAY)
                tok[stack[depth - 1]].size++;
            esl_jtok_t *t = &tok[n];
            if (c == '{' || c == '[') {
                if (depth >= ESL_JSON_MAX_DEPTH)
                    return ESL_JSON_ERR_DEPTH;
                *t = (esl_jtok_t){c == '{' ? ESL_JSON_OBJECT : ESL_JSON_ARRAY, (uint16_t)i, 0, 0, 0};
                stack[depth++] = (int)n;
                n++;
                i++;
                state = c == '{' ? S_KEY_OR_END : S_VALUE_OR_END;
                continue;
            }
            if (c == '"') {
                size_t s, e;
                if (!scan_string(js, len, &i, &s, &e))
                    return ESL_JSON_ERR_SYNTAX;
                *t = (esl_jtok_t){ESL_JSON_STRING, (uint16_t)s, (uint16_t)e, 0, (uint16_t)(n + 1)};
            } else if (c == '-' || (c >= '0' && c <= '9')) {
                size_t s = i;
                while (i < len && (strchr("+-.eE", js[i]) || (js[i] >= '0' && js[i] <= '9')) && js[i] != '\0')
                    i++;
                *t = (esl_jtok_t){ESL_JSON_NUMBER, (uint16_t)s, (uint16_t)i, 0, (uint16_t)(n + 1)};
            } else {
                static const struct { const char *word; uint8_t type; } lits[] = {
                    {"true", ESL_JSON_TRUE}, {"false", ESL_JSON_FALSE}, {"null", ESL_JSON_NULL}};
                int found = -1;
                for (int k = 0; k < 3; k++) {
                    size_t wl = strlen(lits[k].word);
                    if (len - i >= wl && memcmp(&js[i], lits[k].word, wl) == 0) {
                        found = k;
                        break;
                    }
                }
                if (found < 0)
                    return ESL_JSON_ERR_SYNTAX;
                size_t wl = strlen(lits[found].word);
                *t = (esl_jtok_t){lits[found].type, (uint16_t)i, (uint16_t)(i + wl), 0, (uint16_t)(n + 1)};
                i += wl;
            }
            n++;
            state = depth == 0 ? S_DONE : S_COMMA_OR_END;
            continue;
        }
        }
    }
    return state == S_DONE ? (int)n : ESL_JSON_ERR_SYNTAX;
}

bool esl_json_str_eq(const char *js, const esl_jtok_t *t, const char *s)
{
    size_t l = strlen(s);
    return t->type == ESL_JSON_STRING && (size_t)(t->end - t->start) == l && memcmp(&js[t->start], s, l) == 0;
}

int esl_json_obj_get(const char *js, const esl_jtok_t *tok, int obj, const char *key)
{
    if (obj < 0 || tok[obj].type != ESL_JSON_OBJECT)
        return -1;
    int i = obj + 1;
    for (unsigned k = 0; k < tok[obj].size; k++) {
        int val = i + 1;
        if (esl_json_str_eq(js, &tok[i], key))
            return val;
        i = tok[val].next;
    }
    return -1;
}

static int put_utf8(char *out, size_t max, size_t *o, uint32_t cp)
{
    char b[4];
    size_t n;
    if (cp < 0x80) {
        b[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*o + n > max)
        return -2;
    memcpy(&out[*o], b, n);
    *o += n;
    return 0;
}

static uint32_t read_u4(const char *p)
{
    return (hex_val(p[0]) << 12) | (hex_val(p[1]) << 8) | (hex_val(p[2]) << 4) | hex_val(p[3]);
}

int esl_json_str_decode(const char *js, const esl_jtok_t *t, char *out, size_t out_max)
{
    size_t o = 0;
    if (t->type != ESL_JSON_STRING)
        return -1;
    for (size_t i = t->start; i < t->end; i++) {
        char c = js[i];
        uint32_t cp;
        if (c != '\\') {
            if (o >= out_max)
                return -2;
            out[o++] = c;
            continue;
        }
        c = js[++i];
        switch (c) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            cp = read_u4(&js[i + 1]);
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < t->end &&js[i + 1] == '\\' && js[i + 2] == 'u') {
                uint32_t lo = read_u4(&js[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF)
                cp = '?'; /* unpaired surrogate */
            break;
        default:
            return -1;
        }
        if (put_utf8(out, out_max, &o, cp) < 0)
            return -2;
    }
    return (int)o;
}

/* ---- writer ------------------------------------------------------------ */

void esl_sb_init(esl_sb_t *sb, char *buf, size_t cap)
{
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->overflow = cap == 0;
    if (cap)
        buf[0] = '\0';
}

void esl_sb_putc(esl_sb_t *sb, char c)
{
    if (sb->len + 1 >= sb->cap) {
        sb->overflow = true;
        return;
    }
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

void esl_sb_puts(esl_sb_t *sb, const char *s)
{
    while (*s)
        esl_sb_putc(sb, *s++);
}

void esl_sb_printf(esl_sb_t *sb, const char *fmt, ...)
{
    if (sb->cap == 0) {
        sb->overflow = true;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    size_t room = sb->cap - sb->len;
    int n = vsnprintf(&sb->buf[sb->len], room, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= room) {
        sb->overflow = true;
        sb->buf[sb->len] = '\0';
        return;
    }
    sb->len += (size_t)n;
}

static void put_escaped(esl_sb_t *sb, unsigned char c)
{
    switch (c) {
    case '"': esl_sb_puts(sb, "\\\""); break;
    case '\\': esl_sb_puts(sb, "\\\\"); break;
    case '\n': esl_sb_puts(sb, "\\n"); break;
    case '\r': esl_sb_puts(sb, "\\r"); break;
    case '\t': esl_sb_puts(sb, "\\t"); break;
    default:
        if (c < 0x20)
            esl_sb_printf(sb, "\\u%04x", c);
        else
            esl_sb_putc(sb, (char)c);
    }
}

void esl_sb_json_latin1(esl_sb_t *sb, const uint8_t *s, size_t len)
{
    esl_sb_putc(sb, '"');
    for (size_t i = 0; i < len; i++) {
        uint8_t c = s[i];
        if (c >= 0x80) {
            esl_sb_putc(sb, (char)(0xC0 | (c >> 6)));
            esl_sb_putc(sb, (char)(0x80 | (c & 0x3F)));
        } else {
            put_escaped(sb, c);
        }
    }
    esl_sb_putc(sb, '"');
}

void esl_sb_json_str(esl_sb_t *sb, const char *s)
{
    esl_sb_putc(sb, '"');
    while (*s)
        put_escaped(sb, (unsigned char)*s++);
    esl_sb_putc(sb, '"');
}
