#include "font.h" /* tag/src: font_t and the built-in fonts */
#include "esl_font.h"
#include "esl_crc32.h"
#include "esl_proto.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct esl_ufont {
    font_t font;         /* widths and bitmap point into blob, offsets into offsets */
    uint32_t version;    /* CRC32 of the blob, never 0 */
    uint16_t blob_len;
    uint16_t nchars;     /* characters with a glyph */
    char name[ESL_FONT_NAME_MAX + 1];
    char source[ESL_FONT_SOURCE_MAX + 1];
    uint16_t *offsets;   /* [last - first + 1], right after this struct */
    uint8_t *blob;       /* after offsets */
};

/* The built-in fonts in id order (ESL_FONT_* in shared/proto.h) */
static const char *const builtin_names[] = {"small", "medium", "large", "mid", "text20", "price44"};
static const uint8_t builtin_height[] = {ESL_FONT_SMALL_H, ESL_FONT_MEDIUM_H, ESL_FONT_LARGE_H, ESL_FONT_MID_H,
                                         ESL_FONT_TEXT20_H, ESL_FONT_PRICE44_H};
_Static_assert(ESL_FONT_SMALL == 0 && ESL_FONT_MEDIUM == 1 && ESL_FONT_LARGE == 2 && ESL_FONT_MID == 3 &&
                   ESL_FONT_TEXT20 == 4 && ESL_FONT_PRICE44 == 5 && ESL_FONT_COUNT == 6,
               "builtin_names order");
_Static_assert(ESL_FONT_USER_BASE >= ESL_FONT_COUNT && ESL_FONT_USER_BASE + ESL_FONT_USER_MAX <= ESL_FONT_NONE,
               "font file ids");
_Static_assert(ESL_FONT_MAX_BYTES <= 0xFFFFFF, "font bytes");

static esl_ufont_t *slots[ESL_FONT_USER_MAX];

/* ---- blob ---------------------------------------------------------------------------------- */

#define HDR 6 /* ver height baseline first last name_len */

/* Checks a blob and builds the font around a copy of it. */
esl_ufont_t *esl_font_from_blob(const uint8_t *b, size_t len)
{
    if (!b || len < HDR + 1 || len > 0xFFFF || b[0] != ESL_FONT_BLOB_VER)
        return NULL;
    uint8_t height = b[1], baseline = b[2], first = b[3], last = b[4], nl = b[5];
    if (height < ESL_FONT_MIN_H || height > ESL_FONT_MAX_H || baseline > height || first < 0x20 || last < first ||
        nl < 1 || nl > ESL_FONT_NAME_MAX || len < HDR + (size_t)nl + 1)
        return NULL;
    size_t pos = HDR + nl;
    uint8_t sl = b[pos++];
    size_t n = (size_t)(last - first) + 1;
    if (sl > ESL_FONT_SOURCE_MAX || len < pos + sl + n)
        return NULL;
    char name[ESL_FONT_NAME_MAX + 1];
    memcpy(name, b + HDR, nl);
    name[nl] = '\0';
    if (!esl_font_name_ok(name, nl))
        return NULL;
    for (size_t i = 0; i < sl; i++)
        if (b[pos + i] < 0x20 || b[pos + i] > 0x7E)
            return NULL; /* the source note is plain ASCII */
    size_t wpos = pos + sl, bpc = (height + 7u) / 8u, bits = 0;
    uint16_t nchars = 0;
    for (size_t i = 0; i < n; i++) {
        bits += b[wpos + i] * bpc;
        nchars += b[wpos + i] != 0;
    }
    if (!nchars || bits > 0xFFFF || len != wpos + n + bits)
        return NULL;

    esl_ufont_t *f = malloc(sizeof(*f) + n * sizeof(uint16_t) + len);
    if (!f)
        return NULL;
    memset(f, 0, sizeof(*f));
    f->offsets = (uint16_t *)(f + 1);
    f->blob = (uint8_t *)(f->offsets + n);
    memcpy(f->blob, b, len);
    f->blob_len = (uint16_t)len;
    f->nchars = nchars;
    memcpy(f->name, name, nl + 1);
    memcpy(f->source, b + pos, sl);
    f->source[sl] = '\0';
    uint16_t off = 0;
    for (size_t i = 0; i < n; i++) {
        f->offsets[i] = off;
        off = (uint16_t)(off + b[wpos + i] * bpc);
    }
    f->font.height = height;
    f->font.baseline = baseline;
    f->font.bytes_per_col = (uint8_t)bpc;
    f->font.first = first;
    f->font.last = last;
    f->font.widths = f->blob + wpos;
    f->font.offsets = f->offsets;
    f->font.bitmap = f->blob + wpos + n;
    f->version = esl_crc32(f->blob, len);
    if (!f->version)
        f->version = 1;
    return f;
}

void esl_font_free(esl_ufont_t *f)
{
    free(f);
}

const uint8_t *esl_font_blob(const esl_ufont_t *f, size_t *len)
{
    *len = f->blob_len;
    return f->blob;
}

const char *esl_font_name(const esl_ufont_t *f)
{
    return f->name;
}

bool esl_font_name_ok(const char *name, size_t len)
{
    if (len < 1 || len > ESL_FONT_NAME_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    for (unsigned i = 0; i < ESL_FONT_COUNT; i++)
        if (strlen(builtin_names[i]) == len && memcmp(builtin_names[i], name, len) == 0)
            return false;
    return true;
}

/* ---- JSON ---------------------------------------------------------------------------------- */

/* Keys and widths take a token each: 2 per key, at most 224 widths (0x20..0xFF) */
#define FONT_TOKENS 300

static esl_ufont_t *fail(char *err, size_t errlen, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

static esl_ufont_t *fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return NULL;
}

/* A whole number 0..max, or -1 */
static long num(const char *js, const esl_jtok_t *tok, int v, long max)
{
    if (v < 0 || tok[v].type != ESL_JSON_NUMBER || tok[v].end <= tok[v].start)
        return -1;
    long n = 0;
    for (unsigned i = tok[v].start; i < tok[v].end; i++) {
        if (js[i] < '0' || js[i] > '9' || n > max)
            return -1;
        n = n * 10 + (js[i] - '0');
    }
    return n > max ? -1 : n;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

/* Base64 (RFC 4648, padding optional) of s[0..n) into out; the byte count, or -1 if it is not
 * base64 or more than cap bytes */
static long b64_decode(const char *s, size_t n, uint8_t *out, size_t cap)
{
    while (n && s[n - 1] == '=')
        n--;
    if (n % 4 == 1)
        return -1;
    size_t o = 0;
    uint32_t acc = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64_value(s[i]);
        if (v < 0)
            return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap)
                return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (long)o;
}

esl_ufont_t *esl_font_from_json(const char *js, size_t len, const char *want, char *err, size_t errlen)
{
    static const char *const keys[] = {"format", "version", "name", "height", "baseline", "first",
                                       "last",   "widths",  "bitmap", "source", NULL};
    if (!js || len == 0 || len > ESL_JSON_MAX_LEN)
        return fail(err, errlen, "missing font file");
    esl_jtok_t *tok = malloc(sizeof(*tok) * FONT_TOKENS);
    uint8_t *blob = NULL;
    esl_ufont_t *f = NULL;
    if (!tok)
        return fail(err, errlen, "out of memory");
    int n = esl_json_parse(js, len, tok, FONT_TOKENS);
    if (n < 0) {
        fail(err, errlen, n == ESL_JSON_ERR_NOMEM ? "too many values (widths covers at most 224 characters)"
                                                   : "invalid JSON");
        goto done;
    }
    if (tok[0].type != ESL_JSON_OBJECT) {
        fail(err, errlen, "the font file must be a JSON object");
        goto done;
    }
    int k = 1;
    for (unsigned i = 0; i < tok[0].size; i++, k = tok[k + 1].next) {
        bool known = tok[k].end > tok[k].start && js[tok[k].start] == '_';
        for (unsigned j = 0; keys[j] && !known; j++)
            known = esl_json_str_eq(js, &tok[k], keys[j]);
        if (!known) {
            fail(err, errlen, "unknown key \"%.*s\" (keys starting with _ are ignored)",
                 (int)(tok[k].end - tok[k].start > 24 ? 24 : tok[k].end - tok[k].start), &js[tok[k].start]);
            goto done;
        }
    }
    int fm = esl_json_obj_get(js, tok, 0, "format");
    if (fm < 0 || !esl_json_str_eq(js, &tok[fm], "esl-font")) {
        fail(err, errlen, "format must be \"esl-font\"");
        goto done;
    }
    if (num(js, tok, esl_json_obj_get(js, tok, 0, "version"), 255) != 1) {
        fail(err, errlen, "version must be 1");
        goto done;
    }

    char name[ESL_FONT_NAME_MAX + 1];
    size_t nl = 0;
    int nm = esl_json_obj_get(js, tok, 0, "name");
    if (nm >= 0) {
        int r = tok[nm].type == ESL_JSON_STRING ? esl_json_str_decode(js, &tok[nm], name, ESL_FONT_NAME_MAX) : -1;
        if (r < 0 || !esl_font_name_ok(name, (size_t)r)) {
            fail(err, errlen, "name must be 1 to %d of a-z, 0-9, _ and -, and not a built-in font's", ESL_FONT_NAME_MAX);
            goto done;
        }
        nl = (size_t)r;
        name[nl] = '\0';
        if (want && strcmp(want, name) != 0) {
            fail(err, errlen, "the file is font \"%s\", not \"%s\"", name, want);
            goto done;
        }
    } else if (want) {
        nl = strlen(want);
        if (!esl_font_name_ok(want, nl)) {
            fail(err, errlen, "a font name is 1 to %d of a-z, 0-9, _ and -, and not a built-in font's",
                 ESL_FONT_NAME_MAX);
            goto done;
        }
        memcpy(name, want, nl + 1);
    } else {
        fail(err, errlen, "name is missing");
        goto done;
    }

    long height = num(js, tok, esl_json_obj_get(js, tok, 0, "height"), 255);
    long baseline = num(js, tok, esl_json_obj_get(js, tok, 0, "baseline"), 255);
    long first = num(js, tok, esl_json_obj_get(js, tok, 0, "first"), 255);
    long last = num(js, tok, esl_json_obj_get(js, tok, 0, "last"), 255);
    if (height < ESL_FONT_MIN_H || height > ESL_FONT_MAX_H) {
        fail(err, errlen, "height must be %d to %d pixels", ESL_FONT_MIN_H, ESL_FONT_MAX_H);
        goto done;
    }
    if (baseline < 0 || baseline > height) {
        fail(err, errlen, "baseline must be 0 to height (rows above the baseline)");
        goto done;
    }
    if (first < 0x20 || last < first) {
        fail(err, errlen, "first and last must be character codes 32 to 255 (Latin-1), first <= last");
        goto done;
    }
    size_t nch = (size_t)(last - first) + 1, bpc = ((size_t)height + 7) / 8;

    char src[ESL_FONT_SOURCE_MAX + 1];
    size_t sl = 0;
    int so = esl_json_obj_get(js, tok, 0, "source");
    if (so >= 0) {
        int r = tok[so].type == ESL_JSON_STRING ? esl_json_str_decode(js, &tok[so], src, ESL_FONT_SOURCE_MAX) : -1;
        for (int i = 0; i < r; i++)
            if (src[i] < 0x20 || src[i] > 0x7E)
                r = -1;
        if (r < 0) {
            fail(err, errlen, "source must be plain ASCII text of at most %d bytes", ESL_FONT_SOURCE_MAX);
            goto done;
        }
        sl = (size_t)r;
    }

    int w = esl_json_obj_get(js, tok, 0, "widths");
    if (w < 0 || tok[w].type != ESL_JSON_ARRAY || tok[w].size != nch) {
        fail(err, errlen, "widths must be a list of %u widths, one for each character from first to last",
             (unsigned)nch);
        goto done;
    }
    int bm = esl_json_obj_get(js, tok, 0, "bitmap");
    if (bm < 0 || tok[bm].type != ESL_JSON_STRING) {
        fail(err, errlen, "bitmap must be a base64 string");
        goto done;
    }
    size_t cap = HDR + nl + 1 + sl + nch + (size_t)(tok[bm].end - tok[bm].start) / 4 * 3 + 3;
    blob = malloc(cap);
    if (!blob) {
        fail(err, errlen, "out of memory");
        goto done;
    }
    size_t pos = 0, bits = 0;
    blob[pos++] = ESL_FONT_BLOB_VER;
    blob[pos++] = (uint8_t)height;
    blob[pos++] = (uint8_t)baseline;
    blob[pos++] = (uint8_t)first;
    blob[pos++] = (uint8_t)last;
    blob[pos++] = (uint8_t)nl;
    memcpy(blob + pos, name, nl);
    pos += nl;
    blob[pos++] = (uint8_t)sl;
    memcpy(blob + pos, src, sl);
    pos += sl;
    k = w + 1;
    for (size_t i = 0; i < nch; i++, k = tok[k].next) {
        long cw = num(js, tok, k, 255);
        if (cw < 0) {
            fail(err, errlen, "widths[%u] must be 0 (no such character) to 255 pixels", (unsigned)i);
            goto done;
        }
        blob[pos++] = (uint8_t)cw;
        bits += (size_t)cw * bpc;
    }
    if (bits > 0xFFFF) {
        fail(err, errlen, "the bitmap is %u bytes: at most 65535", (unsigned)bits);
        goto done;
    }
    long got = b64_decode(&js[tok[bm].start], (size_t)(tok[bm].end - tok[bm].start), blob + pos, cap - pos);
    if (got < 0 || (size_t)got != bits) {
        fail(err, errlen, "bitmap must be base64 of %u bytes (the widths times %u bytes a column)", (unsigned)bits,
             (unsigned)bpc);
        goto done;
    }
    pos += (size_t)got;
    f = esl_font_from_blob(blob, pos);
    if (!f)
        fail(err, errlen, bits ? "out of memory" : "the font has no characters");

done:
    free(tok);
    free(blob);
    return f;
}

static void sb_base64(esl_sb_t *out, const uint8_t *p, size_t n)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n)
            v |= p[i + 2];
        esl_sb_putc(out, abc[v >> 18]);
        esl_sb_putc(out, abc[(v >> 12) & 63]);
        esl_sb_putc(out, i + 1 < n ? abc[(v >> 6) & 63] : '=');
        esl_sb_putc(out, i + 2 < n ? abc[v & 63] : '=');
    }
}

void esl_font_write_json(const esl_ufont_t *f, esl_sb_t *out)
{
    const font_t *t = &f->font;
    size_t n = (size_t)(t->last - t->first) + 1, bits = 0;
    esl_sb_puts(out, "{\"format\":\"esl-font\",\"version\":1,\"name\":");
    esl_sb_json_str(out, f->name);
    esl_sb_printf(out, ",\"height\":%u,\"baseline\":%u,\"first\":%u,\"last\":%u,", t->height, t->baseline, t->first,
                  t->last);
    if (f->source[0]) {
        esl_sb_puts(out, "\"source\":");
        esl_sb_json_str(out, f->source);
        esl_sb_putc(out, ',');
    }
    esl_sb_puts(out, "\"widths\":[");
    for (size_t i = 0; i < n; i++) {
        esl_sb_printf(out, i ? ",%u" : "%u", t->widths[i]);
        bits += (size_t)t->widths[i] * t->bytes_per_col;
    }
    esl_sb_puts(out, "],\"bitmap\":\"");
    sb_base64(out, t->bitmap, bits);
    esl_sb_puts(out, "\"}");
}

void esl_font_write_info(const esl_ufont_t *f, esl_sb_t *out, bool open)
{
    esl_sb_puts(out, "{\"name\":");
    esl_sb_json_str(out, f->name);
    esl_sb_printf(out,
                  ",\"height\":%u,\"baseline\":%u,\"first\":%u,\"last\":%u,\"characters\":%u,\"bytes\":%u,"
                  "\"version\":%lu,\"source\":",
                  f->font.height, f->font.baseline, f->font.first, f->font.last, f->nchars, f->blob_len,
                  (unsigned long)f->version);
    esl_sb_json_str(out, f->source);
    if (!open)
        esl_sb_putc(out, '}');
}

/* ---- the table ------------------------------------------------------------------------------ */

static const esl_ufont_t *user(uint8_t id)
{
    return id >= ESL_FONT_USER_BASE && id < ESL_FONT_USER_BASE + ESL_FONT_USER_MAX ? slots[id - ESL_FONT_USER_BASE]
                                                                                    : NULL;
}

const font_t *esl_font_get(uint8_t id)
{
    const esl_ufont_t *f = user(id);
    return f ? &f->font : font_get(id);
}

int esl_font_find(const char *name, size_t len)
{
    for (unsigned i = 0; i < ESL_FONT_COUNT; i++)
        if (strlen(builtin_names[i]) == len && memcmp(builtin_names[i], name, len) == 0)
            return (int)i;
    int s = esl_font_slot_of(name, len);
    return s < 0 ? -1 : ESL_FONT_USER_BASE + s;
}

uint8_t esl_font_height(uint8_t id)
{
    if (id < ESL_FONT_COUNT)
        return builtin_height[id];
    const esl_ufont_t *f = user(id);
    return f ? f->font.height : 0;
}

uint32_t esl_font_version(uint8_t id)
{
    const esl_ufont_t *f = user(id);
    return f ? f->version : 0;
}

const esl_ufont_t *esl_font_slot(unsigned n)
{
    return n < ESL_FONT_USER_MAX ? slots[n] : NULL;
}

int esl_font_slot_of(const char *name, size_t len)
{
    for (unsigned i = 0; i < ESL_FONT_USER_MAX; i++)
        if (slots[i] && strlen(slots[i]->name) == len && memcmp(slots[i]->name, name, len) == 0)
            return (int)i;
    return -1;
}

int esl_font_free_slot(void)
{
    for (unsigned i = 0; i < ESL_FONT_USER_MAX; i++)
        if (!slots[i])
            return (int)i;
    return -1;
}

esl_ufont_t *esl_font_swap(unsigned n, esl_ufont_t *f)
{
    if (n >= ESL_FONT_USER_MAX)
        return f;
    esl_ufont_t *old = slots[n];
    slots[n] = f;
    return old;
}

size_t esl_font_bytes(void)
{
    size_t n = 0;
    for (unsigned i = 0; i < ESL_FONT_USER_MAX; i++)
        if (slots[i])
            n += slots[i]->blob_len;
    return n;
}

void esl_font_write_builtin(esl_sb_t *out)
{
    esl_sb_putc(out, '[');
    for (unsigned i = 0; i < ESL_FONT_COUNT; i++) {
        const font_t *f = font_get((uint8_t)i);
        esl_sb_printf(out, "%s{\"name\":\"%s\",\"height\":%u,\"baseline\":%u,\"first\":%u,\"last\":%u}", i ? "," : "",
                      builtin_names[i], f->height, f->baseline, f->first, f->last);
    }
    esl_sb_putc(out, ']');
}

void esl_font_clear(void)
{
    for (unsigned i = 0; i < ESL_FONT_USER_MAX; i++) {
        free(slots[i]);
        slots[i] = NULL;
    }
}
