#include "esl_barcode.h"
#include "esl_clock.h"
#include "esl_crc32.h"
#include "esl_layout.h"
#include "esl_qr.h"
#include "esl_text.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout file (JSON) -> the binary esl_render.c draws from. The format is in
 * docs/protokoll.md part 3; the limits come from shared/proto.h. Pure C, no ESP-IDF:
 * also built into the web page's renderer (web/wasm/web_api.c). */

typedef struct {
    const char *js;
    const esl_jtok_t *tok;
    char *err;
    size_t errlen;
    uint16_t scr_w, scr_h;
    /* current place in the file, for messages */
    int li, ei;
    /* the fixed texts of the whole file, in Latin-1, each once */
    uint8_t model;
    bool has_red;         /* the model draws red: red and red_invert are allowed */
    uint8_t ntext;
    uint8_t tlen[ESL_LBIN_TEXT_MAX];
    uint8_t (*text)[ESL_LBIN_TEXT_LEN_MAX]; /* ESL_LBIN_TEXT_MAX of them, malloc'd */
    int icon_names;       /* the fixed text with the first icon's name, -1 until an icon element names its icon by a field */
    struct cc_icons *ic;  /* malloc'd: the AP compiles at boot on the main task's small stack */
} cc_t;

esl_limits_t esl_limits = {
    .runs = ESL_DRAW_MAX_RUNS,
    .rects = ESL_DRAW_MAX_RECTS,
    .lines = ESL_DRAW_MAX_LINES,
    .icons = ESL_DRAW_MAX_ICONS,
    .file_bytes = ESL_LBIN_MAX,
    .json_bytes = ESL_LAYOUT_MAX_SIZE,
};

_Static_assert(ESL_DRAW_MAX_RUNS <= ESL_DRAW_CAP_RUNS && ESL_DRAW_MAX_RECTS <= ESL_DRAW_CAP_RECTS &&
                   ESL_DRAW_MAX_LINES <= ESL_DRAW_CAP_LINES && ESL_DRAW_MAX_ICONS <= ESL_DRAW_CAP_ICONS &&
                   ESL_LBIN_MAX <= ESL_LBIN_CAP,
               "a limit is larger than the room for it (ESL_*_CAP)");
_Static_assert(ESL_DRAW_CAP_RUNS <= 255 && ESL_DRAW_CAP_RECTS <= 255 && ESL_DRAW_CAP_LINES <= 255 &&
                   ESL_DRAW_CAP_ICONS <= 255,
               "esl_render.c counts runs, rects, lines and icons in a byte");
_Static_assert(ESL_LBIN_CAP <= 65535 && ESL_LAYOUT_MAX_SIZE <= 65535, "sizes are 16 bits");

/* The icons ("icons" in the file), checked first and written into the file at the end */
struct cc_icons {
    uint8_t n;
    char name[ESL_LBIN_ICON_MAX][ESL_LAYOUT_FIELD_NAME_MAX + 1];
    uint16_t w[ESL_LBIN_ICON_MAX];
    uint8_t h[ESL_LBIN_ICON_MAX];
    size_t size; /* bytes they take compiled */
    int data[ESL_LBIN_ICON_MAX]; /* the token of each icon's "data" */
};

static bool fail(cc_t *c, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static bool fail(cc_t *c, const char *fmt, ...)
{
    char msg[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (c->li >= 0 && c->ei >= 0)
        snprintf(c->err, c->errlen, "layouts[%d].elements[%d]: %s", c->li, c->ei, msg);
    else if (c->li >= 0)
        snprintf(c->err, c->errlen, "layouts[%d]: %s", c->li, msg);
    else
        snprintf(c->err, c->errlen, "%s", msg);
    return false;
}

static const esl_jtok_t *T(const cc_t *c, int i)
{
    return &c->tok[i];
}

/* Index of the string value in names[] (NULL-terminated), or -1. */
static int pick(const cc_t *c, int v, const char *const *names)
{
    if (T(c, v)->type != ESL_JSON_STRING)
        return -1;
    for (int i = 0; names[i]; i++)
        if (esl_json_str_eq(c->js, T(c, v), names[i]))
            return i;
    return -1;
}

/* A plain non-negative integer up to 65535. */
static bool get_uint(const cc_t *c, int v, unsigned *out)
{
    const esl_jtok_t *t = T(c, v);
    unsigned n = 0;
    if (t->type != ESL_JSON_NUMBER || t->end <= t->start)
        return false;
    for (unsigned i = t->start; i < t->end; i++) {
        char ch = c->js[i];
        if (ch < '0' || ch > '9' || n > 6553)
            return false;
        n = n * 10 + (unsigned)(ch - '0');
    }
    if (n > 65535)
        return false;
    *out = n;
    return true;
}

static void str_copy(const cc_t *c, int v, char *out, size_t cap)
{
    size_t n = (size_t)(T(c, v)->end - T(c, v)->start);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, &c->js[T(c, v)->start], n);
    out[n] = '\0';
}

/* Every key of object obj must be in allowed (NULL-terminated) or start with '_'. */
static bool keys_ok(cc_t *c, int obj, const char *const *allowed)
{
    int k = obj + 1;
    for (unsigned i = 0; i < T(c, obj)->size; i++) {
        const esl_jtok_t *key = T(c, k);
        bool ok = key->end > key->start && c->js[key->start] == '_';
        for (int a = 0; !ok && allowed[a]; a++)
            ok = esl_json_str_eq(c->js, key, allowed[a]);
        if (!ok) {
            char name[24];
            str_copy(c, k, name, sizeof(name));
            return fail(c, "unknown key \"%s\" (keys starting with _ are ignored)", name);
        }
        for (int j = obj + 1; j != k; j = T(c, j + 1)->next) {
            const esl_jtok_t *prev = T(c, j);
            if (prev->end - prev->start == key->end - key->start &&
                memcmp(&c->js[prev->start], &c->js[key->start], (size_t)(key->end - key->start)) == 0) {
                char name[24];
                str_copy(c, k, name, sizeof(name));
                return fail(c, "the key \"%s\" appears twice", name);
            }
        }
        k = T(c, k + 1)->next;
    }
    return true;
}

static const char *const font_names[] = {"small", "medium", "large", "mid", "text20", "price44", NULL};
static const uint8_t font_height[] = {ESL_FONT_SMALL_H, ESL_FONT_MEDIUM_H, ESL_FONT_LARGE_H, ESL_FONT_MID_H,
                                      ESL_FONT_TEXT20_H, ESL_FONT_PRICE44_H};
_Static_assert(ESL_FONT_SMALL == 0 && ESL_FONT_MEDIUM == 1 && ESL_FONT_LARGE == 2 && ESL_FONT_MID == 3 &&
               ESL_FONT_TEXT20 == 4 && ESL_FONT_PRICE44 == 5, "font_names order");

static const char *const align_names[] = {"left", "center", "right", NULL};
static const uint8_t align_opts[] = {ESL_LE_LEFT, ESL_LE_CENTER, ESL_LE_RIGHT};
static const char *const valign_names[] = {"top", "middle", "bottom", NULL};
static const uint8_t valign_opts[] = {0, ESL_LE_MIDDLE, ESL_LE_BOTTOM};
static const char *const color_names[] = {"black", "white", "invert", "highlight", "frame", "red", "red_invert", NULL};
static const uint8_t color_codes[] = {ESL_LE_BLACK, ESL_LE_WHITE, ESL_LE_INVERT, ESL_LE_PRICE_HL, ESL_LE_FRAME,
                                      ESL_LE_RED, ESL_LE_RED_INVERT};

typedef struct {
    uint8_t nfields;
    char fields[ESL_MAX_FIELDS][ESL_LAYOUT_FIELD_NAME_MAX + 1];
    unsigned runs, rects, nums; /* what the layout needs at most from the AP's scene (esl_render.c) */
    unsigned barcodes, lines, icons, qrs;
    int8_t bc_field;      /* the barcode element, for checking content (esl_layout_bin_t) */
    uint8_t bc_sym;
    uint16_t bc_max;
    int8_t qr_field;      /* the QR element, the same */
    uint8_t qr_ecc;
    uint16_t qr_cap;
    uint8_t clock;        /* ESL_CLOCK_*: the finest date field in its fixed texts */
    unsigned clocks;      /* text elements with date fields */
} lay_t;

static bool field_name_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > ESL_LAYOUT_FIELD_NAME_MAX || s[0] < 'a' || s[0] > 'z')
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
            return false;
    return true;
}

/* "text": a fixed text, stored once in the file. Returns its field byte, or -1 after fail(). */
static int fixed_text(cc_t *c, int v)
{
    char utf8[ESL_LBIN_TEXT_LEN_MAX * 4 + 4];
    uint8_t latin1[ESL_LBIN_TEXT_LEN_MAX];
    bool overflow = false;
    size_t n = 0;

    if (T(c, v)->type != ESL_JSON_STRING)
        return fail(c, "text must be a string"), -1;
    int ul = esl_json_str_decode(c->js, T(c, v), utf8, sizeof(utf8));
    if (ul == -1)
        return fail(c, "invalid string escape in text"), -1;
    if (ul >= 0)
        n = esl_utf8_to_latin1(utf8, (size_t)ul, latin1, sizeof(latin1), &overflow);
    if (ul < 0 || overflow)
        return fail(c, "text is longer than %d bytes", ESL_LBIN_TEXT_LEN_MAX), -1;
    if (n == 0)
        return fail(c, "text must not be empty"), -1;
    for (unsigned i = 0; i < c->ntext; i++)
        if (c->tlen[i] == n && memcmp(c->text[i], latin1, n) == 0)
            return (int)(ESL_LE_FIELD_FIXED | i);
    if (c->ntext >= ESL_LBIN_TEXT_MAX)
        return fail(c, "at most %d different fixed texts in a file", ESL_LBIN_TEXT_MAX), -1;
    memcpy(c->text[c->ntext], latin1, n);
    c->tlen[c->ntext] = (uint8_t)n;
    return (int)(ESL_LE_FIELD_FIXED | c->ntext++);
}

/* "decimals" and "thousands": how a text element draws a number (ESL_LE_NUM_* in opts). */
static bool number_format(cc_t *c, int e, uint8_t *opts)
{
    static const char *const modes[] = {"keep", "auto", "dash", "0-dash", NULL};
    static const uint8_t mode_bits[] = {ESL_LE_NUM_KEEP, ESL_LE_NUM_AUTO, ESL_LE_NUM_DASH, ESL_LE_NUM_0_DASH};
    uint8_t num = 0;

    int d = esl_json_obj_get(c->js, c->tok, e, "decimals");
    if (d >= 0) {
        unsigned n;
        int m = pick(c, d, modes);
        if (m >= 0)
            num = mode_bits[m];
        else if (get_uint(c, d, &n) && (n == 0 || n == 2))
            num = n ? ESL_LE_NUM_2 : ESL_LE_NUM_0;
        else
            return fail(c, "decimals must be 0, 2, \"auto\" (2, none for ,00), \"dash\" (2, ,- for ,00), \"0-dash\" (none, ,-) or \"keep\"");
    }
    int t = esl_json_obj_get(c->js, c->tok, e, "thousands");
    if (t >= 0) {
        if (T(c, t)->type != ESL_JSON_TRUE && T(c, t)->type != ESL_JSON_FALSE)
            return fail(c, "thousands must be true or false");
        if (T(c, t)->type == ESL_JSON_TRUE)
            num |= (uint8_t)((num ? 0 : ESL_LE_NUM_KEEP) | ESL_LE_NUM_THOUSANDS);
    }
    *opts |= num;
    return true;
}

/* The rest of a barcode element (box, field and colour are done). */
static bool barcode_element(cc_t *c, int e, lay_t *lay, uint8_t *out, unsigned fi, unsigned w, unsigned h,
                            uint8_t color)
{
    static const char *const syms[] = {"code128", "ean13", NULL};
    static const char *const text_only[] = {"fonts", "valign", "lines"};
    _Static_assert(ESL_BC_CODE128 == 0 && ESL_BC_EAN13 == 1, "syms order");

    for (int i = 0; i < 3; i++)
        if (esl_json_obj_get(c->js, c->tok, e, text_only[i]) >= 0)
            return fail(c, "%s is only for text elements", text_only[i]);
    if (color != ESL_LE_BLACK)
        return fail(c, "a barcode is black");
    int sy = esl_json_obj_get(c->js, c->tok, e, "symbology");
    int si = sy < 0 ? -1 : pick(c, sy, syms);
    if (si < 0)
        return fail(c, "symbology must be code128 or ean13");
    out[ESL_LE_OFF_SYM] = (uint8_t)si;

    int a = esl_json_obj_get(c->js, c->tok, e, "align");
    int ai = a < 0 ? 0 : pick(c, a, align_names);
    if (ai < 0)
        return fail(c, "align must be left, center or right");
    out[ESL_LE_OFF_OPTS] = align_opts[ai];

    unsigned mw = 0;
    int m = esl_json_obj_get(c->js, c->tok, e, "module");
    if (m >= 0 && (!get_uint(c, m, &mw) || mw > ESL_BC_MODULE_MAX))
        return fail(c, "module must be 0 (the widest that fits) to %d pixels", ESL_BC_MODULE_MAX);
    out[ESL_LE_OFF_MODULE] = (uint8_t)mw;

    if (h < ESL_BC_MIN_H)
        return fail(c, "a barcode must be at least %d px high", ESL_BC_MIN_H);
    unsigned max = w / (mw ? mw : 1);
    unsigned ean = 95 + ESL_BC_QUIET_EAN_L + ESL_BC_QUIET_EAN_R;
    if (si == ESL_BC_EAN13 && max < ean)
        return fail(c, "an EAN-13 with its quiet zones is %u modules: w must be at least %u", ean, ean * (mw ? mw : 1));
    if (++lay->barcodes > ESL_DRAW_MAX_BARCODES)
        return fail(c, "the AP draws at most %d barcode per screen", ESL_DRAW_MAX_BARCODES);
    lay->bc_field = (int8_t)fi;
    lay->bc_sym = (uint8_t)si;
    lay->bc_max = (uint16_t)max;
    return true;
}

/* One element into 14 bytes at out. */
/* Keys that only some element types take: "key" is refused unless the type is in kinds. */
typedef struct {
    const char *key;
    const char *kinds; /* the types that take it, as type letters: t text, r rect, b barcode, l line, i icon */
    const char *what;  /* for the message */
} only_t;

static bool only_ok(cc_t *c, int e, char kind)
{
    static const only_t only[] = {
        {"field", "tbqi", "text, barcode, QR and icon elements"}, {"text", "tbq", "text elements"},
        {"fonts", "t", "text elements"},                  {"valign", "tq", "text and QR elements"},
        {"lines", "t", "text elements"},                  {"align", "tbq", "text, barcode and QR elements"},
        {"symbology", "b", "barcode elements"},           {"module", "bq", "barcode and QR elements"},
        {"ecc", "q", "QR elements"},
        {"radius", "r", "rect elements"},                 {"border", "r", "rect elements"},
        {"direction", "l", "line elements"},              {"width", "l", "line elements"},
        {"icon", "i", "icon elements"},
        {"decimals", "t", "text elements"},               {"thousands", "t", "text elements"},
    };
    for (size_t i = 0; i < sizeof(only) / sizeof(only[0]); i++)
        if (!strchr(only[i].kinds, kind) && esl_json_obj_get(c->js, c->tok, e, only[i].key) >= 0)
            return fail(c, "%s is only for %s", only[i].key, only[i].what);
    return true;
}

/* An optional whole number key: def when it is missing; false after fail() when it is
 * not a number from lo to hi. */
static bool opt_uint(cc_t *c, int e, const char *key, unsigned lo, unsigned hi, unsigned def, unsigned *out)
{
    int t = esl_json_obj_get(c->js, c->tok, e, key);
    *out = def;
    if (t < 0)
        return true;
    if (!get_uint(c, t, out) || *out < lo || *out > hi)
        return fail(c, "%s must be a whole number from %u to %u", key, lo, hi);
    return true;
}

/* The rest of a QR element (box, field and colour are done). */
static bool qr_element(cc_t *c, int e, lay_t *lay, uint8_t *out, unsigned fi, unsigned w, unsigned h, uint8_t color)
{
    static const char *const levels[] = {"L", "M", "Q", "H", NULL};
    _Static_assert(ESL_QR_LEVEL_L == ESL_QR_ECC_L && ESL_QR_LEVEL_H == ESL_QR_ECC_H, "QR levels");

    if (color != ESL_LE_BLACK)
        return fail(c, "a QR code is black");
    int ec = esl_json_obj_get(c->js, c->tok, e, "ecc");
    int li = ec < 0 ? ESL_QR_LEVEL_M : pick(c, ec, levels);
    if (li < 0)
        return fail(c, "ecc must be L, M, Q or H (about 7, 15, 25 or 30 %% of the code can be lost)");
    out[ESL_LE_OFF_ECC] = (uint8_t)li;

    int a = esl_json_obj_get(c->js, c->tok, e, "align");
    int ai = a < 0 ? 0 : pick(c, a, align_names);
    int va = esl_json_obj_get(c->js, c->tok, e, "valign");
    int vi = va < 0 ? 0 : pick(c, va, valign_names);
    if (ai < 0)
        return fail(c, "align must be left, center or right");
    if (vi < 0)
        return fail(c, "valign must be top, middle or bottom");
    out[ESL_LE_OFF_OPTS] = (uint8_t)(align_opts[ai] | valign_opts[vi]);

    unsigned mw;
    if (!opt_uint(c, e, "module", 0, ESL_QR_MODULE_MAX, 0, &mw))
        return false;
    out[ESL_LE_OFF_MODULE] = (uint8_t)mw;

    /* the largest version the box holds with its quiet zone */
    unsigned side = (w < h ? w : h) / (mw ? mw : 1), v = 0;
    while (v < ESL_QR_VERSION_MAX && ESL_QR_SIZE(v + 1) + 2 * ESL_QR_QUIET <= side)
        v++;
    if (!v)
        return fail(c, "the box must be at least %u px wide and high for a QR code (21 modules%s and %d of white "
                    "on each side)", ESL_QR_MIN_SIDE * (mw ? mw : 1), mw > 1 ? "" : " of 1 px", (int)ESL_QR_QUIET);
    if (++lay->qrs > ESL_DRAW_MAX_QR)
        return fail(c, "the AP draws at most %d QR code per screen", ESL_DRAW_MAX_QR);
    lay->qr_field = (int8_t)fi;
    lay->qr_ecc = (uint8_t)li;
    lay->qr_cap = (uint16_t)esl_qr_capacity((uint8_t)v, (uint8_t)li);
    return true;
}

/* "field": a name from the layout's fields, or its index, into *fi; false after fail() */
static bool field_index(cc_t *c, int f, const lay_t *lay, unsigned *fi)
{
    if (T(c, f)->type == ESL_JSON_STRING) {
        char name[ESL_LAYOUT_FIELD_NAME_MAX + 2];
        str_copy(c, f, name, sizeof(name));
        for (*fi = 0; *fi < lay->nfields && strcmp(lay->fields[*fi], name) != 0; (*fi)++)
            ;
        if (*fi == lay->nfields)
            return fail(c, "field \"%s\" is not in the layout's fields", name);
    } else if (!get_uint(c, f, fi) || *fi >= lay->nfields) {
        return fail(c, "field must be one of the layout's fields (a name, or 0..%u)", (unsigned)lay->nfields - 1);
    }
    return true;
}

/* The names of the file's icons as fixed texts one after the other, for icon elements
 * that name their icon by a field (ESL_LE_OFF_NAMES); added once, at the first such element. */
static bool icon_names(cc_t *c)
{
    if (c->icon_names >= 0)
        return true;
    if (!c->ic->n)
        return fail(c, "an icon element with a field needs icons in the file: the field names one of them");
    if (c->ntext + c->ic->n > ESL_LBIN_TEXT_MAX)
        return fail(c, "at most %d different fixed texts in a file, with the %u icon names an icon by field takes",
                    ESL_LBIN_TEXT_MAX, (unsigned)c->ic->n);
    c->icon_names = c->ntext;
    for (unsigned i = 0; i < c->ic->n; i++) {
        size_t n = strlen(c->ic->name[i]);
        memcpy(c->text[c->ntext], c->ic->name[i], n);
        c->tlen[c->ntext++] = (uint8_t)n;
    }
    return true;
}

static bool element(cc_t *c, int e, lay_t *lay, uint8_t *out)
{
    static const char *const keys[] = {"type",  "x",      "y",      "w",  "h",  "field", "fonts", "align",
                                       "valign", "lines", "color",  "symbology", "module", "text", "radius",
                                       "border", "direction", "width", "icon", "ecc", "decimals", "thousands", NULL};
    static const char *const types[] = {"text", "rect", "barcode", "line", "icon", "qr", NULL};
    static const char kinds[] = "trbliq";
    static const char *const box[] = {"x", "y", "w", "h"};
    unsigned v[4];
    int icon = -1;
    unsigned icon_field = 0;
    bool by_field = false;

    if (T(c, e)->type != ESL_JSON_OBJECT)
        return fail(c, "must be an object");
    if (!keys_ok(c, e, keys))
        return false;
    int ty = esl_json_obj_get(c->js, c->tok, e, "type");
    int kind = ty < 0 ? -1 : pick(c, ty, types);
    if (kind < 0)
        return fail(c, "type must be \"text\", \"rect\", \"barcode\", \"qr\", \"line\" or \"icon\"");
    if (!only_ok(c, e, kinds[kind]))
        return false;
    bool text = kind == 0, barcode = kind == 2, line = kind == 3, is_icon = kind == 4, qr = kind == 5;
    bool rising = false;

    if (is_icon && esl_json_obj_get(c->js, c->tok, e, "field") >= 0) {
        /* the icon named by a field's text, in a box of its own: x, y, w and h as for a rect */
        if (esl_json_obj_get(c->js, c->tok, e, "icon") >= 0)
            return fail(c, "give either icon or field, not both");
        if (!field_index(c, esl_json_obj_get(c->js, c->tok, e, "field"), lay, &icon_field) || !icon_names(c))
            return false;
        by_field = true;
        for (int i = 0; i < 4; i++) {
            int t = esl_json_obj_get(c->js, c->tok, e, box[i]);
            if (t < 0 || !get_uint(c, t, &v[i]))
                return fail(c, "%s must be a whole number of pixels, 0 or more", box[i]);
        }
    } else if (is_icon) {
        int t = esl_json_obj_get(c->js, c->tok, e, "icon");
        if (t >= 0 && T(c, t)->type == ESL_JSON_STRING)
            for (icon = 0; icon < c->ic->n && !esl_json_str_eq(c->js, T(c, t), c->ic->name[icon]); icon++)
                ;
        if (t < 0 || T(c, t)->type != ESL_JSON_STRING)
            return fail(c, "an icon element needs icon: the name of an icon in the file's icons, or field: a field "
                           "that names one");
        if (icon == c->ic->n) {
            char name[ESL_LAYOUT_FIELD_NAME_MAX + 2];
            str_copy(c, t, name, sizeof(name));
            return fail(c, "icon \"%s\" is not in the file's icons", name);
        }
        for (int i = 0; i < 2; i++) {
            int b = esl_json_obj_get(c->js, c->tok, e, box[i]);
            if (b < 0 || !get_uint(c, b, &v[i]))
                return fail(c, "%s must be a whole number of pixels, 0 or more", box[i]);
        }
        v[2] = c->ic->w[icon];
        v[3] = c->ic->h[icon];
        for (int i = 2; i < 4; i++) {
            unsigned given;
            int b = esl_json_obj_get(c->js, c->tok, e, box[i]);
            if (b >= 0 && (!get_uint(c, b, &given) || given != v[i]))
                return fail(c, "%s must be the icon's %s (%u), or left out", box[i], i == 2 ? "width" : "height",
                            v[i]);
        }
    } else {
        for (int i = 0; i < 4; i++) {
            int t = esl_json_obj_get(c->js, c->tok, e, box[i]);
            if (t < 0 || !get_uint(c, t, &v[i]))
                return fail(c, "%s must be a whole number of pixels, 0 or more", box[i]);
        }
    }
    unsigned x = v[0], y = v[1], w = v[2], h = v[3];
    if (w == 0 || h == 0)
        return fail(c, "w and h must be at least 1");
    if (line) {
        static const char *const dirs[] = {"down", "up", NULL};
        int d = esl_json_obj_get(c->js, c->tok, e, "direction");
        int di = d < 0 ? 0 : pick(c, d, dirs);
        if (di < 0)
            return fail(c, "direction must be down (top-left to bottom-right) or up (bottom-left to top-right)");
        /* a straight line has no direction: the same line compiles the same */
        rising = di == 1 && w > 1 && h > 1;
    }
    if (x + w > c->scr_w)
        return fail(c, "x + w = %u is past the right edge of the screen (%u)", x + w, (unsigned)c->scr_w);
    if (y + h > c->scr_h)
        return fail(c, "y + h = %u is past the bottom of the screen (%u)", y + h, (unsigned)c->scr_h);
    if (y > 255 || h > 255)
        /* ESL_LE_OFF_Y and ESL_LE_OFF_H are one byte each in this format: a screen taller
           than 255 pixels needs a new layout format, not a silently cut coordinate */
        return fail(c, "y and h must be at most 255 (the layout format has one byte for each)");

    memset(out, 0, ESL_LE_SIZE);
    out[ESL_LE_OFF_KIND] = text      ? ESL_LE_TEXT
                           : barcode ? ESL_LE_BARCODE
                           : line    ? ESL_LE_LINE
                           : is_icon ? ESL_LE_ICON
                           : qr      ? ESL_LE_QR
                                     : ESL_LE_RECT;
    out[ESL_LE_OFF_FONT] = out[ESL_LE_OFF_FONT + 1] = out[ESL_LE_OFF_FONT + 2] = ESL_FONT_NONE;
    out[ESL_LE_OFF_X] = (uint8_t)x;
    out[ESL_LE_OFF_X + 1] = (uint8_t)(x >> 8);
    out[ESL_LE_OFF_Y] = (uint8_t)y;
    out[ESL_LE_OFF_W] = (uint8_t)w;
    out[ESL_LE_OFF_W + 1] = (uint8_t)(w >> 8);
    out[ESL_LE_OFF_H] = (uint8_t)h;

    int col = esl_json_obj_get(c->js, c->tok, e, "color");
    int ci = col < 0 ? 0 : pick(c, col, color_names);
    if (ci < 0)
        return fail(c, "color must be black, white, invert, highlight or frame%s",
                    c->has_red ? ", or red or red_invert" : "");
    uint8_t color = color_codes[ci];
    if ((color == ESL_LE_RED || color == ESL_LE_RED_INVERT) && !c->has_red)
        return fail(c, "the %s is a black/white model: red and red_invert need a model with red",
                    esl_model_name(c->model));
    out[ESL_LE_OFF_COLOR] = color;

    if (line || is_icon) {
        if (color != ESL_LE_BLACK && color != ESL_LE_WHITE && color != ESL_LE_RED)
            return fail(c, "a%s can be black, white or red", line ? " line" : "n icon");
        if (is_icon) {
            if (by_field) {
                out[ESL_LE_OFF_FIELD] = (uint8_t)(ESL_LE_ICON_FIELD | icon_field);
                out[ESL_LE_OFF_NAMES] = (uint8_t)c->icon_names;
            } else
                out[ESL_LE_OFF_FIELD] = (uint8_t)icon;
            if (++lay->icons > esl_limits.icons)
                return fail(c, "the AP draws at most %d icons per screen", esl_limits.icons);
            return true;
        }
        unsigned lw;
        if (!opt_uint(c, e, "width", 1, ESL_LINE_W_MAX, 1, &lw))
            return false;
        out[ESL_LE_OFF_WIDTH] = (uint8_t)lw;
        out[ESL_LE_OFF_OPTS] = rising ? ESL_LE_RISING : 0;
        if (++lay->lines > esl_limits.lines)
            return fail(c, "the AP draws at most %d lines per screen", esl_limits.lines);
        return true;
    }

    if (!text && !barcode && !qr) {
        unsigned radius, border, half = (w < h ? w : h) / 2;
        if (color == ESL_LE_INVERT || color == ESL_LE_PRICE_HL || color == ESL_LE_RED_INVERT)
            return fail(c, "a rect can be black, white, red or frame");
        if (!opt_uint(c, e, "radius", 0, half > 255 ? 255 : half, 0, &radius))
            return false;
        if (!opt_uint(c, e, "border", 0, half > 255 ? 255 : half, 0, &border))
            return false;
        out[ESL_LE_OFF_RADIUS] = (uint8_t)radius;
        out[ESL_LE_OFF_BORDER] = (uint8_t)border;
        lay->rects++;
        return true;
    }

    /* field: a name from the layout's fields, or its index; or a fixed text */
    int f = esl_json_obj_get(c->js, c->tok, e, "field");
    int fx = esl_json_obj_get(c->js, c->tok, e, "text");
    unsigned fi = 0;
    if (fx >= 0 && (barcode || qr))
        return fail(c, "a %s takes a field, not a fixed text", qr ? "QR code" : "barcode");
    if (fx >= 0 && f >= 0)
        return fail(c, "give either field or text, not both");
    if (fx >= 0) {
        int fb = fixed_text(c, fx);
        if (fb < 0)
            return false;
        out[ESL_LE_OFF_FIELD] = (uint8_t)fb;
        unsigned ti = (unsigned)fb & ~ESL_LE_FIELD_FIXED;
        uint8_t res = esl_clock_res(c->text[ti], c->tlen[ti]);
        if (res) {
            if (++lay->clocks > ESL_CLOCK_TEXTS)
                return fail(c, "at most %d texts with date fields ({date}, {week}, ...) in a layout", ESL_CLOCK_TEXTS);
            if (res > lay->clock)
                lay->clock = res;
        }
    } else if (f < 0)
        return fail(c, barcode ? "a barcode element needs a field"
                       : qr    ? "a QR element needs a field"
                               : "a text element needs a field or a fixed text");
    if (fx >= 0) {
        /* the fixed text is in place */
    } else if (!field_index(c, f, lay, &fi)) {
        return false;
    }
    if (fx < 0)
        out[ESL_LE_OFF_FIELD] = (uint8_t)fi;

    if (barcode)
        return barcode_element(c, e, lay, out, fi, w, h, color);
    if (qr)
        return qr_element(c, e, lay, out, fi, w, h, color);

    int fo = esl_json_obj_get(c->js, c->tok, e, "fonts");
    if (fo < 0 || T(c, fo)->type != ESL_JSON_ARRAY || T(c, fo)->size < 1 || T(c, fo)->size > 3)
        return fail(c, "fonts must be a list of 1 to 3 fonts (small, medium, mid, large, text20, price44), tried in order");
    unsigned min_h = 255;
    int k = fo + 1;
    for (unsigned i = 0; i < T(c, fo)->size; i++, k = T(c, k)->next) {
        int font = pick(c, k, font_names);
        if (font < 0)
            return fail(c, "unknown font in fonts (use small, medium, mid, large, text20 or price44)");
        out[ESL_LE_OFF_FONT + i] = (uint8_t)font;
        if (font_height[font] < min_h)
            min_h = font_height[font];
    }

    int a = esl_json_obj_get(c->js, c->tok, e, "align");
    int ai = a < 0 ? 0 : pick(c, a, align_names);
    int va = esl_json_obj_get(c->js, c->tok, e, "valign");
    int vi = va < 0 ? 0 : pick(c, va, valign_names);
    if (ai < 0)
        return fail(c, "align must be left, center or right");
    if (vi < 0)
        return fail(c, "valign must be top, middle or bottom");
    out[ESL_LE_OFF_OPTS] = (uint8_t)(align_opts[ai] | valign_opts[vi]);
    if (!number_format(c, e, &out[ESL_LE_OFF_OPTS]))
        return false;
    if ((out[ESL_LE_OFF_OPTS] & ESL_LE_NUM_MASK) && ++lay->nums > ESL_DRAW_MAX_NUMS)
        return fail(c, "the AP formats at most %d numbers (decimals, thousands) per layout", ESL_DRAW_MAX_NUMS);

    unsigned lines = 0;
    int ln = esl_json_obj_get(c->js, c->tok, e, "lines");
    if (ln >= 0 && (!get_uint(c, ln, &lines) || lines > ESL_LE_MAX_LINES))
        return fail(c, "lines must be 0 (as many as fit) to %d", ESL_LE_MAX_LINES);
    out[ESL_LE_OFF_LINES] = (uint8_t)lines;

    bool inverted = color == ESL_LE_INVERT || color == ESL_LE_PRICE_HL || color == ESL_LE_RED_INVERT;
    unsigned pad = inverted ? 2 * ESL_INVERT_PAD : 0;
    if (h < min_h + pad || w <= pad)
        return fail(c, "the box (%ux%u) is too small for its smallest font (%u px high%s)", w, h, min_h,
                    inverted ? " plus the border of an inverted field" : "");

    /* Lines this element can draw at most: the tag tries each font and uses
     * as many lines as the box holds (1..3), limited by "lines". */
    unsigned most = 1;
    for (int i = 0; i < 3 && out[ESL_LE_OFF_FONT + i] != ESL_FONT_NONE; i++) {
        unsigned n = h / font_height[out[ESL_LE_OFF_FONT + i]];
        if (lines && lines < n)
            n = lines;
        if (n > ESL_LE_MAX_LINES)
            n = ESL_LE_MAX_LINES;
        if (n > most)
            most = n;
    }
    lay->runs += most;
    lay->rects += inverted;
    return true;
}

/* Where base64_decode puts the bytes of an icon: row by row in, column by column out. With
 * col NULL it only counts them. */
typedef struct {
    uint8_t *col;
    size_t row, cb; /* bytes per row of the picture, per column of the file */
} icon_out_t;

static void icon_put(const icon_out_t *o, size_t i, uint8_t b)
{
    size_t y = i / o->row, x0 = (i % o->row) * 8;
    for (unsigned k = 0; k < 8; k++)
        if (b & (0x80 >> k))
            o->col[(x0 + k) * o->cb + y / 8] |= (uint8_t)(0x80 >> (y % 8));
}

/* Base64 (RFC 4648, with or without padding) of a JSON string as it is in the file (so "\/"
 * is "/"); the number of bytes, or -1. At most cap bytes go to o, which may be NULL. */
static int base64_decode(const char *s, size_t n, const icon_out_t *o, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t len = 0;

    while (n && s[n - 1] == '=')
        n--;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < n && s[i + 1] == '/')
            ch = s[++i];
        int v = ch >= 'A' && ch <= 'Z'   ? ch - 'A'
                : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26
                : ch >= '0' && ch <= '9' ? ch - '0' + 52
                : ch == '+'              ? 62
                : ch == '/'              ? 63
                                         : -1;
        if (v < 0)
            return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (len >= cap)
                return -1;
            if (o && o->col)
                icon_put(o, len, (uint8_t)(acc >> bits));
            len++;
        }
    }
    return (int)len;
}

/* "icons": {"name": {"w": .., "h": .., "data": "<base64>"}, ...}. data is the picture row
 * by row from the top, (w + 7) / 8 bytes per row, bit 7 of a row's first byte its leftmost
 * pixel, 1 = drawn. Only checked here; icons_write puts them in the file. */
static bool icons(cc_t *c, int obj)
{
    _Static_assert(ESL_ICON_MAX_W == ESL_ICON_MAX_H, "one limit in the message below");
    static const char *const keys[] = {"w", "h", "data", NULL};
    struct cc_icons *ic = c->ic;

    if (T(c, obj)->type != ESL_JSON_OBJECT || T(c, obj)->size > ESL_LBIN_ICON_MAX)
        return fail(c, "icons must be an object with up to %d icons by name", ESL_LBIN_ICON_MAX);
    int k = obj + 1;
    for (unsigned i = 0; i < T(c, obj)->size; i++, k = T(c, k + 1)->next) {
        char *name = ic->name[i];
        int v = k + 1;
        unsigned w, h;
        if (T(c, k)->end - T(c, k)->start > ESL_LAYOUT_FIELD_NAME_MAX)
            return fail(c, "icon names must be 1-16 of a-z 0-9 _, starting with a letter");
        str_copy(c, k, name, ESL_LAYOUT_FIELD_NAME_MAX + 1);
        if (!field_name_ok(name))
            return fail(c, "icon names must be 1-16 of a-z 0-9 _, starting with a letter");
        for (unsigned j = 0; j < i; j++)
            if (strcmp(ic->name[j], name) == 0)
                return fail(c, "the icon \"%s\" appears twice", name);
        if (T(c, v)->type != ESL_JSON_OBJECT || !keys_ok(c, v, keys))
            return T(c, v)->type != ESL_JSON_OBJECT ? fail(c, "icons.%s must be an object", name) : false;
        int tw = esl_json_obj_get(c->js, c->tok, v, "w"), th = esl_json_obj_get(c->js, c->tok, v, "h");
        if (tw < 0 || !get_uint(c, tw, &w) || w < 1 || w > ESL_ICON_MAX_W || th < 0 || !get_uint(c, th, &h) ||
            h < 1 || h > ESL_ICON_MAX_H)
            return fail(c, "icons.%s: w and h must be 1 to %d pixels", name, ESL_ICON_MAX_W);
        int td = esl_json_obj_get(c->js, c->tok, v, "data");
        size_t row = (w + 7) / 8, need = row * h;
        int got = td < 0 || T(c, td)->type != ESL_JSON_STRING
                      ? -2
                      : base64_decode(&c->js[T(c, td)->start], (size_t)(T(c, td)->end - T(c, td)->start), NULL, need);
        if (got != (int)need)
            return fail(c, "icons.%s: data must be base64 of %u bytes (%u rows of %u)", name, (unsigned)need, h,
                        (unsigned)row);
        size_t size = ESL_LBIN_ICON_HDR + w * ((h + 7) / 8);
        if (ic->size + size > esl_limits.file_bytes)
            return fail(c, "the icons are too big: at most %d bytes compiled", esl_limits.file_bytes);
        ic->size += size;
        ic->data[i] = td;
        ic->w[i] = (uint16_t)w;
        ic->h[i] = (uint8_t)h;
        ic->n = (uint8_t)(i + 1);
    }
    return true;
}

/* The icons checked by icons(), column by column as screen images are, at p */
static void icons_write(const cc_t *c, uint8_t *p)
{
    const struct cc_icons *ic = c->ic;
    for (unsigned i = 0; i < ic->n; i++) {
        unsigned w = ic->w[i], h = ic->h[i];
        icon_out_t o = {p + ESL_LBIN_ICON_HDR, (w + 7) / 8, (h + 7) / 8};
        const esl_jtok_t *t = T(c, ic->data[i]);
        p[0] = (uint8_t)w;
        p[1] = (uint8_t)(w >> 8);
        p[2] = (uint8_t)h;
        memset(o.col, 0, w * o.cb);
        base64_decode(&c->js[t->start], (size_t)(t->end - t->start), &o, o.row * h);
        p += ESL_LBIN_ICON_HDR + w * o.cb;
    }
}

static bool layout(cc_t *c, int l, esl_layout_bin_t *out, size_t *pos)
{
    static const char *const keys[] = {"name", "fields", "elements", "description", NULL};
    lay_t lay;
    memset(&lay, 0, sizeof(lay));
    lay.bc_field = -1;
    lay.qr_field = -1;

    if (T(c, l)->type != ESL_JSON_OBJECT)
        return fail(c, "must be an object");
    if (!keys_ok(c, l, keys))
        return false;

    int nm = esl_json_obj_get(c->js, c->tok, l, "name");
    if (nm < 0 || T(c, nm)->type != ESL_JSON_STRING ||
        !esl_layout_name_valid(&c->js[T(c, nm)->start], (size_t)(T(c, nm)->end - T(c, nm)->start)))
        return fail(c, "name must be 1-31 of a-z 0-9 . _ - (starting with a letter or digit), "
                       "and not price or text");
    char *name = out->names[c->li];
    str_copy(c, nm, name, ESL_LAYOUT_NAME_MAX + 1);
    for (int i = 0; i < c->li; i++)
        if (strcmp(out->names[i], name) == 0)
            return fail(c, "the name \"%s\" is used twice", name);

    int fl = esl_json_obj_get(c->js, c->tok, l, "fields");
    if (fl < 0 || T(c, fl)->type != ESL_JSON_ARRAY || T(c, fl)->size < 1 || T(c, fl)->size > ESL_MAX_FIELDS)
        return fail(c, "fields must list the layout's 1 to %d text fields by name, in the order clients send them",
                    ESL_MAX_FIELDS);
    int k = fl + 1;
    for (unsigned i = 0; i < T(c, fl)->size; i++, k = T(c, k)->next) {
        char *f = lay.fields[i];
        if (T(c, k)->type != ESL_JSON_STRING || T(c, k)->end - T(c, k)->start > ESL_LAYOUT_FIELD_NAME_MAX)
            return fail(c, "field names must be 1-16 of a-z 0-9 _, starting with a letter");
        str_copy(c, k, f, ESL_LAYOUT_FIELD_NAME_MAX + 1);
        if (!field_name_ok(f))
            return fail(c, "field names must be 1-16 of a-z 0-9 _, starting with a letter");
        for (unsigned j = 0; j < i; j++)
            if (strcmp(lay.fields[j], f) == 0)
                return fail(c, "the field \"%s\" is listed twice", f);
    }
    lay.nfields = (uint8_t)T(c, fl)->size;

    int el = esl_json_obj_get(c->js, c->tok, l, "elements");
    if (el < 0 || T(c, el)->type != ESL_JSON_ARRAY || T(c, el)->size < 1)
        return fail(c, "elements must be a list of at least one element");
    size_t need = 1 + (size_t)T(c, el)->size * ESL_LE_SIZE;
    if (T(c, el)->size > 255 || *pos + need > esl_limits.file_bytes)
        return fail(c, "the file is too big: at most %d bytes compiled, about %d elements in all",
                    esl_limits.file_bytes, (esl_limits.file_bytes - ESL_LBIN_HDR_LEN) / ESL_LE_SIZE);
    out->bin[(*pos)++] = (uint8_t)T(c, el)->size;
    k = el + 1;
    for (unsigned i = 0; i < T(c, el)->size; i++, k = T(c, k)->next) {
        c->ei = (int)i;
        if (!element(c, k, &lay, &out->bin[*pos]))
            return false;
        *pos += ESL_LE_SIZE;
    }
    c->ei = -1;

    if (lay.runs > esl_limits.runs)
        return fail(c, "needs up to %u lines of text, the AP draws at most %d per screen (lower lines or h)",
                    lay.runs, esl_limits.runs);
    if (lay.rects > esl_limits.rects)
        return fail(c, "needs %u rectangles (rect elements and inverted fields), the AP draws at most %d",
                    lay.rects, esl_limits.rects);
    out->nfields[c->li] = lay.nfields;
    out->bc_field[c->li] = lay.bc_field;
    out->bc_sym[c->li] = lay.bc_sym;
    out->bc_max[c->li] = lay.bc_max;
    out->qr_field[c->li] = lay.qr_field;
    out->qr_ecc[c->li] = lay.qr_ecc;
    out->qr_cap[c->li] = lay.qr_cap;
    out->clock[c->li] = lay.clock;
    return true;
}

bool esl_layout_compile(const char *json, size_t len, uint8_t model, esl_layout_bin_t *out, char *err,
                        size_t errlen)
{
    static const char *const keys[] = {"format", "version", "model", "layouts", "icons", "description", NULL};
    cc_t c;
    memset(&c, 0, sizeof(c));
    c.js = json;
    c.err = err;
    c.errlen = errlen;
    c.li = c.ei = -1;
    c.icon_names = -1;
    bool ok = false;
    esl_jtok_t *tok = NULL;

    memset(out, 0, sizeof(*out));
    if (!esl_model_size(model, &c.scr_w, &c.scr_h))
        return fail(&c, "unknown model");
    c.model = model;
    c.has_red = esl_model_has_red(model);
    if (len == 0 || len > esl_limits.json_bytes)
        return fail(&c, "the file must be 1 to %d bytes", esl_limits.json_bytes);

    /* Every token takes at least two bytes of input except the last. */
    unsigned max_tok = (unsigned)(len / 2 + 2);
    tok = malloc(sizeof(esl_jtok_t) * max_tok);
    c.text = malloc(sizeof(*c.text) * ESL_LBIN_TEXT_MAX);
    c.ic = calloc(1, sizeof(*c.ic));
    if (!tok || !c.text || !c.ic) {
        free(tok);
        free(c.text);
        free(c.ic);
        return fail(&c, "out of memory");
    }
    c.tok = tok;
    int n = esl_json_parse(json, len, tok, max_tok);
    if (n < 0) {
        fail(&c, n == ESL_JSON_ERR_DEPTH ? "JSON nested too deeply" : "invalid JSON");
        goto done;
    }
    if (tok[0].type != ESL_JSON_OBJECT) {
        fail(&c, "the layout file must be a JSON object");
        goto done;
    }
    if (!keys_ok(&c, 0, keys))
        goto done;

    int f = esl_json_obj_get(json, tok, 0, "format");
    if (f < 0 || !esl_json_str_eq(json, &tok[f], "esl-layouts")) {
        fail(&c, "format must be \"esl-layouts\"");
        goto done;
    }
    int v = esl_json_obj_get(json, tok, 0, "version");
    unsigned ver;
    if (v < 0 || !get_uint(&c, v, &ver) || ver != 1) {
        fail(&c, "version must be 1");
        goto done;
    }
    int m = esl_json_obj_get(json, tok, 0, "model");
    if (m < 0 || !esl_json_str_eq(json, &tok[m], esl_model_name(model))) {
        fail(&c, "model must be \"%s\": the file is checked against that screen", esl_model_name(model));
        goto done;
    }
    int ls = esl_json_obj_get(json, tok, 0, "layouts");
    if (ls < 0 || tok[ls].type != ESL_JSON_ARRAY || tok[ls].size < 1 || tok[ls].size > ESL_LAYOUT_FILE_MAX) {
        fail(&c, "layouts must be a list of 1 to %d layouts", ESL_LAYOUT_FILE_MAX);
        goto done;
    }

    int ic = esl_json_obj_get(json, tok, 0, "icons");
    if (ic >= 0 && !icons(&c, ic))
        goto done;

    size_t pos = ESL_LBIN_HDR_LEN;
    out->bin[0] = ESL_LBIN_FORMAT;
    out->bin[1] = model;
    out->bin[2] = (uint8_t)tok[ls].size;
    out->bin[ESL_LBIN_OFF_NTEXT] = 0;
    int k = ls + 1;
    for (unsigned i = 0; i < tok[ls].size; i++, k = tok[k].next) {
        c.li = (int)i;
        if (!layout(&c, k, out, &pos))
            goto done;
    }
    c.li = -1;
    for (unsigned i = 0; i < c.ntext; i++) {
        if (pos + 1 + c.tlen[i] > esl_limits.file_bytes) {
            fail(&c, "the file is too big: at most %d bytes compiled with the fixed texts", esl_limits.file_bytes);
            goto done;
        }
        out->bin[pos++] = c.tlen[i];
        memcpy(&out->bin[pos], c.text[i], c.tlen[i]);
        pos += c.tlen[i];
    }
    out->bin[ESL_LBIN_OFF_NTEXT] = c.ntext;
    if (pos + c.ic->size > esl_limits.file_bytes) {
        fail(&c, "the file is too big: at most %d bytes compiled with the fixed texts and icons", esl_limits.file_bytes);
        goto done;
    }
    icons_write(&c, &out->bin[pos]);
    pos += c.ic->size;
    out->bin[ESL_LBIN_OFF_NICON] = c.ic->n;
    out->count = (uint8_t)tok[ls].size;
    out->size = (uint16_t)pos;
    out->version = esl_crc32(out->bin, pos);
    if (!out->version)
        out->version = 1;
    ok = true;

done:
    free(tok);
    free(c.text);
    free(c.ic);
    if (!ok) {
        out->count = 0;
        out->size = 0;
        out->version = 0;
    }
    return ok;
}

int esl_layout_bin_find(const esl_layout_bin_t *b, const char *name, size_t len)
{
    if (len > ESL_LAYOUT_NAME_MAX)
        return -1;
    for (int i = 0; i < b->count; i++)
        if (strlen(b->names[i]) == len && memcmp(b->names[i], name, len) == 0)
            return i;
    return -1;
}

bool esl_layout_check_fields(const esl_layout_bin_t *b, int i, uint8_t model, const esl_content_t *c, char *msg,
                             size_t msglen)
{
    if (b->bc_field[i] >= 0) {
        uint8_t len;
        const uint8_t *t = esl_content_field(c, (unsigned)b->bc_field[i], &len);
        uint8_t sym = b->bc_sym[i];
        int m = len ? esl_barcode_modules(sym, t, len) : 0;
        if (m < 0) {
            snprintf(msg, msglen, "field %d of layout %s is a %s barcode: %s", b->bc_field[i], b->names[i],
                     esl_barcode_name(sym),
                     sym == ESL_BC_EAN13 ? "it takes 12 digits, or 13 with a correct check digit"
                                         : "it takes ASCII characters only");
            return false;
        }
        if (m > b->bc_max[i]) {
            snprintf(msg, msglen, "field %d of layout %s is too long for its barcode on model %s (%d of %u modules)",
                     b->bc_field[i], b->names[i], esl_model_name(model), m, (unsigned)b->bc_max[i]);
            return false;
        }
    }
    if (b->qr_field[i] >= 0) {
        uint8_t len;
        esl_content_field(c, (unsigned)b->qr_field[i], &len);
        if (len > b->qr_cap[i]) {
            snprintf(msg, msglen, "field %d of layout %s is too long for its QR code on model %s (%u of %u bytes)",
                     b->qr_field[i], b->names[i], esl_model_name(model), (unsigned)len, (unsigned)b->qr_cap[i]);
            return false;
        }
    }
    return true;
}
