#include "esl_barcode.h"
#include "esl_crc32.h"
#include "esl_layout.h"
#include "esl_text.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout file (JSON) -> the binary the tags download. The format is in
 * docs/protokoll.md part 3; the limits come from shared/proto.h and match
 * what tag/src/layout.c can draw. Pure C, no ESP-IDF: also used by the tag's
 * preview tool (tag/tools/render_host.py). */

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
} cc_t;

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

static const char *const font_names[] = {"small", "medium", "large", "mid", NULL};
static const uint8_t font_height[] = {ESL_FONT_SMALL_H, ESL_FONT_MEDIUM_H, ESL_FONT_LARGE_H, ESL_FONT_MID_H};
_Static_assert(ESL_FONT_SMALL == 0 && ESL_FONT_MEDIUM == 1 && ESL_FONT_LARGE == 2 && ESL_FONT_MID == 3,
               "font_names order");

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
    unsigned runs, rects; /* what the layout needs at most from the tag's scene */
    unsigned barcodes;
    int8_t bc_field;      /* the barcode element, for checking content (esl_layout_bin_t) */
    uint8_t bc_sym;
    uint16_t bc_max;
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
        return fail(c, "the tag draws at most %d barcode per screen", ESL_DRAW_MAX_BARCODES);
    lay->bc_field = (int8_t)fi;
    lay->bc_sym = (uint8_t)si;
    lay->bc_max = (uint16_t)max;
    return true;
}

/* One element into 14 bytes at out. */
static bool element(cc_t *c, int e, lay_t *lay, uint8_t *out)
{
    static const char *const keys[] = {"type", "x", "y", "w", "h", "field", "fonts", "align", "valign",
                                       "lines", "color", "symbology", "module", "text", NULL};
    static const char *const types[] = {"text", "rect", "barcode", NULL};
    static const char *const box[] = {"x", "y", "w", "h"};
    unsigned v[4];

    if (T(c, e)->type != ESL_JSON_OBJECT)
        return fail(c, "must be an object");
    if (!keys_ok(c, e, keys))
        return false;
    int ty = esl_json_obj_get(c->js, c->tok, e, "type");
    int kind = ty < 0 ? -1 : pick(c, ty, types);
    if (kind < 0)
        return fail(c, "type must be \"text\", \"rect\" or \"barcode\"");
    bool text = kind == 0, barcode = kind == 2;

    for (int i = 0; i < 4; i++) {
        int t = esl_json_obj_get(c->js, c->tok, e, box[i]);
        if (t < 0 || !get_uint(c, t, &v[i]))
            return fail(c, "%s must be a whole number of pixels, 0 or more", box[i]);
    }
    unsigned x = v[0], y = v[1], w = v[2], h = v[3];
    if (w == 0 || h == 0)
        return fail(c, "w and h must be at least 1");
    if (x + w > c->scr_w)
        return fail(c, "x + w = %u is past the right edge of the screen (%u)", x + w, (unsigned)c->scr_w);
    if (y + h > c->scr_h)
        return fail(c, "y + h = %u is past the bottom of the screen (%u)", y + h, (unsigned)c->scr_h);
    if (y > 255 || h > 255)
        /* ESL_LE_OFF_Y and ESL_LE_OFF_H are one byte each in this format: a screen taller
           than 255 pixels needs a new layout format, not a silently cut coordinate */
        return fail(c, "y and h must be at most 255 (the layout format has one byte for each)");

    memset(out, 0, ESL_LE_SIZE);
    out[ESL_LE_OFF_KIND] = text ? ESL_LE_TEXT : barcode ? ESL_LE_BARCODE : ESL_LE_RECT;
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

    if (!barcode) {
        static const char *const barcode_only[] = {"symbology", "module"};
        for (int i = 0; i < 2; i++)
            if (esl_json_obj_get(c->js, c->tok, e, barcode_only[i]) >= 0)
                return fail(c, "%s is only for barcode elements", barcode_only[i]);
    }

    if (!text && !barcode) {
        static const char *const text_only[] = {"field", "fonts", "align", "valign", "lines", "text"};
        for (int i = 0; i < 6; i++)
            if (esl_json_obj_get(c->js, c->tok, e, text_only[i]) >= 0)
                return fail(c, "%s is only for text elements", text_only[i]);
        if (color == ESL_LE_INVERT || color == ESL_LE_PRICE_HL || color == ESL_LE_RED_INVERT)
            return fail(c, "a rect can be black, white, red or frame");
        lay->rects++;
        return true;
    }

    /* field: a name from the layout's fields, or its index; or a fixed text */
    int f = esl_json_obj_get(c->js, c->tok, e, "field");
    int fx = esl_json_obj_get(c->js, c->tok, e, "text");
    unsigned fi = 0;
    if (fx >= 0 && barcode)
        return fail(c, "a barcode takes a field, not a fixed text");
    if (fx >= 0 && f >= 0)
        return fail(c, "give either field or text, not both");
    if (fx >= 0) {
        int fb = fixed_text(c, fx);
        if (fb < 0)
            return false;
        out[ESL_LE_OFF_FIELD] = (uint8_t)fb;
    } else if (f < 0)
        return fail(c, barcode ? "a barcode element needs a field"
                               : "a text element needs a field or a fixed text");
    if (fx >= 0) {
        /* the fixed text is in place */
    } else if (T(c, f)->type == ESL_JSON_STRING) {
        char name[ESL_LAYOUT_FIELD_NAME_MAX + 2];
        str_copy(c, f, name, sizeof(name));
        for (fi = 0; fi < lay->nfields && strcmp(lay->fields[fi], name) != 0; fi++)
            ;
        if (fi == lay->nfields)
            return fail(c, "field \"%s\" is not in the layout's fields", name);
    } else if (!get_uint(c, f, &fi) || fi >= lay->nfields) {
        return fail(c, "field must be one of the layout's fields (a name, or 0..%u)", (unsigned)lay->nfields - 1);
    }
    if (fx < 0)
        out[ESL_LE_OFF_FIELD] = (uint8_t)fi;

    if (barcode)
        return barcode_element(c, e, lay, out, fi, w, h, color);

    int fo = esl_json_obj_get(c->js, c->tok, e, "fonts");
    if (fo < 0 || T(c, fo)->type != ESL_JSON_ARRAY || T(c, fo)->size < 1 || T(c, fo)->size > 3)
        return fail(c, "fonts must be a list of 1 to 3 fonts (small, medium, mid, large), tried in order");
    unsigned min_h = 255;
    int k = fo + 1;
    for (unsigned i = 0; i < T(c, fo)->size; i++, k = T(c, k)->next) {
        int font = pick(c, k, font_names);
        if (font < 0)
            return fail(c, "unknown font in fonts (use small, medium, mid or large)");
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

    unsigned lines = 0;
    int ln = esl_json_obj_get(c->js, c->tok, e, "lines");
    if (ln >= 0 && (!get_uint(c, ln, &lines) || lines > ESL_LE_MAX_LINES))
        return fail(c, "lines must be 0 (as many as fit) to %d", ESL_LE_MAX_LINES);
    out[ESL_LE_OFF_LINES] = (uint8_t)lines;

    bool inverted = color == ESL_LE_INVERT || color == ESL_LE_PRICE_HL || color == ESL_LE_RED_INVERT;
    unsigned pad = inverted ? 2 * ESL_INVERT_PAD : 0;
    if (h < min_h + pad || w <= pad)
        return fail(c, "the box (%ux%u) is too small for its smallest font (%u px high%s)", w, h, min_h,
                    inverted ? " plus the black border of an inverted field" : "");

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

static bool layout(cc_t *c, int l, esl_layout_bin_t *out, size_t *pos)
{
    static const char *const keys[] = {"name", "fields", "elements", "description", NULL};
    lay_t lay;
    memset(&lay, 0, sizeof(lay));
    lay.bc_field = -1;

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
    if (T(c, el)->size > 255 || *pos + need > ESL_LBIN_MAX)
        return fail(c, "the file is too big for the tags: at most %d bytes, about %d elements in all",
                    ESL_LBIN_MAX, (ESL_LBIN_MAX - ESL_LBIN_HDR_LEN) / ESL_LE_SIZE);
    out->bin[(*pos)++] = (uint8_t)T(c, el)->size;
    k = el + 1;
    for (unsigned i = 0; i < T(c, el)->size; i++, k = T(c, k)->next) {
        c->ei = (int)i;
        if (!element(c, k, &lay, &out->bin[*pos]))
            return false;
        *pos += ESL_LE_SIZE;
    }
    c->ei = -1;

    if (lay.runs > ESL_DRAW_MAX_RUNS)
        return fail(c, "needs up to %u lines of text, the tag draws at most %d per screen (lower lines or h)",
                    lay.runs, ESL_DRAW_MAX_RUNS);
    if (lay.rects > ESL_DRAW_MAX_RECTS)
        return fail(c, "needs %u rectangles (rect elements and inverted fields), the tag draws at most %d",
                    lay.rects, ESL_DRAW_MAX_RECTS);
    out->nfields[c->li] = lay.nfields;
    out->bc_field[c->li] = lay.bc_field;
    out->bc_sym[c->li] = lay.bc_sym;
    out->bc_max[c->li] = lay.bc_max;
    return true;
}

bool esl_layout_compile(const char *json, size_t len, uint8_t model, esl_layout_bin_t *out, char *err,
                        size_t errlen)
{
    static const char *const keys[] = {"format", "version", "model", "layouts", "description", NULL};
    cc_t c;
    memset(&c, 0, sizeof(c));
    c.js = json;
    c.err = err;
    c.errlen = errlen;
    c.li = c.ei = -1;
    bool ok = false;
    esl_jtok_t *tok = NULL;

    memset(out, 0, sizeof(*out));
    if (!esl_model_size(model, &c.scr_w, &c.scr_h))
        return fail(&c, "unknown model");
    c.model = model;
    c.has_red = esl_model_has_red(model);
    if (len == 0 || len > ESL_LAYOUT_MAX_SIZE)
        return fail(&c, "the file must be 1 to %d bytes", ESL_LAYOUT_MAX_SIZE);

    /* Every token takes at least two bytes of input except the last. */
    unsigned max_tok = (unsigned)(len / 2 + 2);
    tok = malloc(sizeof(esl_jtok_t) * max_tok);
    c.text = malloc(sizeof(*c.text) * ESL_LBIN_TEXT_MAX);
    if (!tok || !c.text) {
        free(tok);
        free(c.text);
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
        if (pos + 1 + c.tlen[i] > ESL_LBIN_MAX) {
            fail(&c, "the file is too big for the tags: at most %d bytes with the fixed texts", ESL_LBIN_MAX);
            goto done;
        }
        out->bin[pos++] = c.tlen[i];
        memcpy(&out->bin[pos], c.text[i], c.tlen[i]);
        pos += c.tlen[i];
    }
    out->bin[ESL_LBIN_OFF_NTEXT] = c.ntext;
    out->count = (uint8_t)tok[ls].size;
    out->size = (uint16_t)pos;
    out->version = esl_crc32(out->bin, pos);
    if (!out->version)
        out->version = 1;
    ok = true;

done:
    free(tok);
    free(c.text);
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
