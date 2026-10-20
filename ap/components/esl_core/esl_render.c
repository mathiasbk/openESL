/* esl_render.c - the tag's drawing (tag/src/layout.c and draw.c) for the AP, with the
 * screen size as a run-time parameter instead of a build option. Keep the fitting,
 * wrapping and placement identical to the tag's: sim/ compares the two screens
 * for every model. The fonts (tag/src/font.c, tag/fonts) and the barcodes
 * (tag/src/barcode.c) are the tag's own files, compiled into esl_core as they are. */
#include "esl_render.h"
#include <string.h>
#include "barcode.h" /* tag/src */
#include "font.h"    /* tag/src */
#include "models_gen.h"

/* ---- the model ------------------------------------------------------------- */

typedef struct {
    uint16_t w, h;
    uint8_t col_bytes;
    bool has_red;
    uint8_t margin;    /* LAYOUT_MARGIN in tag/src/layout.h */
} rd_model_t;

bool esl_render_model(uint8_t model, uint16_t *w, uint16_t *h, bool *has_red)
{
#define X(code, name, id, mw, mh, red) \
    if (model == (code)) {             \
        *w = (mw);                     \
        *h = (mh);                     \
        *has_red = (red) != 0;         \
        return true;                   \
    }
    ESL_MODEL_LIST(X)
#undef X
    return false;
}

static bool model_get(uint8_t model, rd_model_t *m)
{
    if (!esl_render_model(model, &m->w, &m->h, &m->has_red))
        return false;
    m->col_bytes = (uint8_t)ESL_RENDER_COL_BYTES(m->h);
    m->margin = m->w >= 200 ? 6 : 5;
    return true;
}

size_t esl_render_size(uint8_t model)
{
    rd_model_t m;
    if (!model_get(model, &m))
        return 0;
    return (size_t)m.w * m.col_bytes * (m.has_red ? 2 : 1);
}

/* ---- the scene (tag/src/draw.h) ----------------------------------------------- */

#define RD_MAX_RUNS     ESL_DRAW_MAX_RUNS
#define RD_MAX_RECTS    ESL_DRAW_MAX_RECTS
#define RD_MAX_BARCODES ESL_DRAW_MAX_BARCODES
#define RD_BC_MAX_W     1024 /* modules a barcode may have: at most the widest screen */

#define RD_BLACK 0
#define RD_WHITE 1
#define RD_RED   2

typedef struct {
    const uint8_t *text;
    uint8_t len;
    uint8_t ellipsis;
    uint8_t font;
    uint8_t color;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint8_t ci;
    uint16_t cx;
} rd_run_t;

typedef struct {
    uint16_t x0, x1;
    uint16_t y0, y1;
    uint8_t color;
} rd_rect_t;

typedef struct {
    uint16_t x0;
    uint16_t nmod;
    uint8_t mw;
    uint16_t y0, y1;
    uint8_t bits[(RD_BC_MAX_W + 7) / 8];
} rd_bars_t;

typedef struct {
    rd_model_t m;
    uint8_t nruns, nrects, nbars;
    rd_run_t run[RD_MAX_RUNS];
    rd_rect_t rect[RD_MAX_RECTS];
    rd_bars_t bars;
} rd_scene_t;

static void scene_clear(rd_scene_t *s)
{
    s->nruns = 0;
    s->nrects = 0;
    s->nbars = 0;
}

static bool add_text(rd_scene_t *s, uint8_t font, uint8_t color, uint16_t x, uint16_t y, const uint8_t *text,
                     uint8_t len, uint8_t ellipsis)
{
    const font_t *f = font_get(font);
    rd_run_t *r;

    if (s->nruns >= RD_MAX_RUNS)
        return false;
    r = &s->run[s->nruns++];
    r->text = text;
    r->len = len;
    r->ellipsis = ellipsis ? 1 : 0;
    r->font = font;
    r->color = color;
    r->x = x;
    r->y = y;
    r->w = font_text_width(f, text, len);
    if (r->ellipsis)
        r->w += font_char_width(f, FONT_ELLIPSIS);
    return true;
}

static bool add_rect(rd_scene_t *s, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint8_t color)
{
    rd_rect_t *r;
    if (s->nrects >= RD_MAX_RECTS)
        return false;
    r = &s->rect[s->nrects++];
    r->x0 = x0;
    r->x1 = x1;
    r->y0 = y0;
    r->y1 = y1;
    r->color = color;
    return true;
}

/* ---- one column (tag/src/draw.c) ------------------------------------------------ */

static void ink(uint8_t *black, uint8_t *red, uint16_t idx, uint8_t m, uint8_t color)
{
    if (color == RD_WHITE) {
        black[idx] &= (uint8_t)~m;
        red[idx] &= (uint8_t)~m;
    } else if (color == RD_RED) {
        red[idx] |= m;
        black[idx] &= (uint8_t)~m;
    } else {
        black[idx] |= m;
        red[idx] &= (uint8_t)~m;
    }
}

static void fill_span(uint8_t *black, uint8_t *red, uint16_t y0, uint16_t y1, uint8_t color)
{
    uint16_t i = y0 >> 3, last = y1 >> 3;
    uint8_t m = (uint8_t)(0xFF >> (y0 & 7));
    for (;;) {
        if (i == last)
            m &= (uint8_t)(0xFF << (7 - (y1 & 7)));
        ink(black, red, i, m, color);
        if (i == last)
            break;
        i++;
        m = 0xFF;
    }
}

static void blit_column(const rd_model_t *mo, uint8_t *black, uint8_t *red, const uint8_t *src, uint8_t nbytes,
                        uint16_t y, uint8_t color)
{
    uint8_t shift = y & 7;
    uint16_t idx = y >> 3;
    uint8_t b, v, lo;

    for (b = 0; b < nbytes && idx < mo->col_bytes; b++, idx++) {
        v = src[b];
        if (!v)
            continue;
        ink(black, red, idx, (uint8_t)(v >> shift), color);
        lo = shift ? (uint8_t)(v << (8 - shift)) : 0;
        if (lo && idx + 1 < mo->col_bytes)
            ink(black, red, (uint16_t)(idx + 1), lo, color);
    }
}

static uint8_t run_char(const rd_run_t *r, uint8_t i)
{
    return i < r->len ? r->text[i] : FONT_ELLIPSIS;
}

static void draw_run(const rd_model_t *mo, rd_run_t *r, uint16_t x, uint8_t *black, uint8_t *red)
{
    const font_t *f;
    uint8_t n = (uint8_t)(r->len + r->ellipsis);
    uint8_t ch, w;

    if (x < r->x || x >= r->x + r->w)
        return;
    f = font_get(r->font);
    for (;;) {
        if (r->ci >= n)
            return;
        ch = font_map(f, run_char(r, r->ci));
        w = ch ? f->widths[ch - f->first] : 0;
        if (x < r->cx + w)
            break;
        r->cx += w;
        r->ci++;
    }
    blit_column(mo, black, red, font_column(f, ch, (uint8_t)(x - r->cx)), f->bytes_per_col, r->y, r->color);
}

static void draw_begin(rd_scene_t *s)
{
    uint8_t i;
    for (i = 0; i < s->nruns; i++) {
        s->run[i].ci = 0;
        s->run[i].cx = s->run[i].x;
    }
}

/* Column x into black and red (col_bytes each); x must increase by 1 per call after draw_begin */
static void draw_column(rd_scene_t *s, uint16_t x, uint8_t *black, uint8_t *red)
{
    const rd_model_t *mo = &s->m;
    uint8_t i;

    memset(black, 0, mo->col_bytes);
    memset(red, 0, mo->col_bytes);

    for (i = 0; i < s->nrects; i++) {
        const rd_rect_t *r = &s->rect[i];
        if (x >= r->x0 && x <= r->x1 && r->y0 <= r->y1 && r->y0 < mo->h)
            fill_span(black, red, r->y0, r->y1 < mo->h ? r->y1 : (uint16_t)(mo->h - 1), r->color);
    }

    if (s->nbars && x >= s->bars.x0) {
        const rd_bars_t *b = &s->bars;
        uint16_t m = (uint16_t)((x - b->x0) / b->mw);
        if (m < b->nmod && (b->bits[m >> 3] & (0x80 >> (m & 7))))
            fill_span(black, red, b->y0, b->y1, RD_BLACK);
    }

    for (i = 0; i < s->nruns; i++)
        draw_run(mo, &s->run[i], x, black, red);

    if (mo->h % 8) {
        black[mo->col_bytes - 1] &= (uint8_t)(0xFF << (8 - mo->h % 8));
        red[mo->col_bytes - 1] &= (uint8_t)(0xFF << (8 - mo->h % 8));
    }
}

/* ---- the elements (tag/src/layout.h) ---------------------------------------------- */

#define RD_MAX_FONTS 3
#define RD_MAX_LINES ESL_LE_MAX_LINES
#define RD_FRAME_W   3
#define RD_INVERT_PAD ESL_INVERT_PAD

#define LE_END ESL_LE_FIELD_FIXED /* never a kind in a file: ends a built-in list */

typedef struct {
    uint8_t kind;
    uint8_t field;
    uint8_t font[RD_MAX_FONTS];
    uint8_t opts;
    uint8_t lines;
    uint8_t color;
    uint16_t x, y, w, h;
} rd_elem_t;

/* ---- fitting text (tag/src/layout.c) ---- */

static uint8_t layout_fit(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *ellipsis)
{
    const font_t *f = font_get(font);
    uint16_t w = 0, ew;
    uint8_t i, cw;

    for (i = 0; i < len; i++) {
        cw = font_char_width(f, s[i]);
        if (w + cw > maxw)
            break;
        w += cw;
    }
    if (i == len) {
        *ellipsis = 0;
        return len;
    }
    ew = font_char_width(f, FONT_ELLIPSIS);
    while (i > 0 && w + ew > maxw) {
        i--;
        w -= font_char_width(f, s[i]);
    }
    while (i > 0 && s[i - 1] == ' ')
        i--;
    *ellipsis = 1;
    return i;
}

static uint8_t layout_wrap(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *next)
{
    const font_t *f = font_get(font);
    uint16_t w = 0;
    uint8_t n, k, cw, end;

    for (n = 0; n < len; n++) {
        cw = font_char_width(f, s[n]);
        if (w + cw > maxw)
            break;
        w += cw;
    }
    if (n == len) {
        *next = len;
        return len;
    }
    for (k = n; k > 0; k--) {
        if (s[k] == ' ')
            break;
    }
    if (k > 0 && len - k <= 3) {
        for (end = (uint8_t)(k - 1); end > 0 && s[end] != ' '; end--)
            ;
        if (end > 0)
            k = end;
    }
    if (k > 0) {
        end = k;
        while (end > 0 && s[end - 1] == ' ')
            end--;
        while (k < len && s[k] == ' ')
            k++;
        *next = k;
        return end;
    }
    if (n == 0)
        n = 1;
    *next = n;
    return n;
}

typedef struct {
    uint8_t start[RD_MAX_LINES];
    uint8_t len[RD_MAX_LINES];
    uint8_t count;
    uint8_t ell;
} rd_lines_t;

#define SPLIT_TRUNC  0x01
#define SPLIT_BROKEN 0x02

static uint8_t split(rd_lines_t *ln, uint8_t font, const uint8_t *t, uint8_t len, uint16_t w, uint8_t n)
{
    uint8_t pos = 0, l, next, bad = 0;

    ln->count = 0;
    ln->ell = 0;
    while (pos < len && ln->count < n) {
        if (ln->count + 1 < n) {
            l = layout_wrap(font, t + pos, (uint8_t)(len - pos), w, &next);
            if (l == next && next < len - pos)
                bad |= SPLIT_BROKEN;
        } else {
            l = layout_fit(font, t + pos, (uint8_t)(len - pos), w, &next);
            ln->ell = next;
            if (ln->ell)
                bad |= SPLIT_TRUNC;
            next = (uint8_t)(len - pos);
        }
        ln->start[ln->count] = pos;
        ln->len[ln->count] = l;
        ln->count++;
        pos = (uint8_t)(pos + next);
    }
    return bad;
}

static uint8_t max_lines(const rd_elem_t *e, const font_t *f)
{
    uint16_t n = e->h / f->height;
    if (e->lines && e->lines < n)
        n = e->lines;
    if (n == 0)
        n = 1;
    if (n > RD_MAX_LINES)
        n = RD_MAX_LINES;
    return (uint8_t)n;
}

static uint8_t max_descent(const rd_elem_t *e)
{
    const font_t *f;
    uint8_t i, d = 0;
    for (i = 0; i < RD_MAX_FONTS && e->font[i] != ESL_FONT_NONE; i++) {
        f = font_get(e->font[i]);
        if (f->height - f->baseline > d)
            d = (uint8_t)(f->height - f->baseline);
    }
    return d;
}

#define COLOR_INVERT     0x80
#define COLOR_RED_INVERT 0x81
#define COLOR_NONE       0xFF

static uint8_t resolve_color(uint8_t c, uint8_t flags)
{
    switch (c) {
    case ESL_LE_WHITE:      return RD_WHITE;
    case ESL_LE_INVERT:     return COLOR_INVERT;
    case ESL_LE_PRICE_HL:   return (flags & ESL_CF_PRICE_HIGHLIGHT) ? COLOR_INVERT : RD_BLACK;
    case ESL_LE_FRAME:      return (flags & ESL_CF_FRAME) ? RD_BLACK : COLOR_NONE;
    case ESL_LE_RED:        return RD_RED;
    case ESL_LE_RED_INVERT: return COLOR_RED_INVERT;
    default:                return RD_BLACK;
    }
}

static void place_text(rd_scene_t *s, const rd_elem_t *e, const uint8_t *t, uint8_t len, uint8_t color)
{
    const rd_model_t *mo = &s->m;
    const font_t *f;
    rd_lines_t ln;
    uint8_t i, font = ESL_FONT_MEDIUM, found = 0, ell, pad = 0, n, box = RD_BLACK;
    int32_t y, y0 = -1, y1 = 0;
    uint16_t lw, x, w, bx0 = 0xFFFF, bx1 = 0;
    uint16_t ey, eh;

    if (color == COLOR_INVERT || color == COLOR_RED_INVERT) {
        pad = RD_INVERT_PAD;
        if (e->w <= 2 * pad || e->h <= 2 * pad)
            return;
        if (color == COLOR_RED_INVERT)
            box = RD_RED;
        color = RD_WHITE;
    }
    x = (uint16_t)(e->x + pad);
    w = (uint16_t)(e->w - 2 * pad);
    ey = (uint16_t)(e->y + pad);
    eh = (uint16_t)(e->h - 2 * pad);

    ln.count = 0;
    ln.ell = 0;
    for (i = 0; i < RD_MAX_FONTS && e->font[i] != ESL_FONT_NONE; i++) {
        font = e->font[i];
        f = font_get(font);
        n = max_lines(e, f);
        if (pad && n * f->height > eh)
            n = (uint8_t)(eh / f->height);
        if (n && font_has_all(f, t, len) && !split(&ln, font, t, len, w, n)) {
            found = 1;
            break;
        }
    }
    f = font_get(font);
    if (!found)
        split(&ln, font, t, len, w, max_lines(e, f));

    switch (e->opts & 0x0C) {
    case ESL_LE_MIDDLE:
        y = (int32_t)ey + ((int32_t)eh - (int32_t)ln.count * f->height) / 2;
        break;
    case ESL_LE_BOTTOM:
        y = (int32_t)ey + eh - max_descent(e) - f->baseline - (int32_t)(ln.count - 1) * f->height;
        break;
    default:
        y = ey;
        break;
    }
    if (y < ey)
        y = ey;

    for (i = 0; i < ln.count; i++, y += f->height) {
        uint16_t lx;
        ell = (i + 1 == ln.count) ? ln.ell : 0;
        if (!ln.len[i] && !ell)
            continue;
        lw = font_text_width(f, t + ln.start[i], ln.len[i]);
        if (ell)
            lw += font_char_width(f, FONT_ELLIPSIS);
        lx = x;
        if (lw < w) {
            if ((e->opts & 0x03) == ESL_LE_RIGHT)
                lx += w - lw;
            else if ((e->opts & 0x03) == ESL_LE_CENTER)
                lx += (w - lw) / 2;
        }
        if (y + f->height > mo->h)
            break;
        if (pad) {
            if (!add_text(s, font, color, lx, (uint16_t)y, t + ln.start[i], ln.len[i], ell))
                break;
            if (y0 < 0)
                y0 = y;
            y1 = y + f->height - 1;
            if (lx < bx0)
                bx0 = lx;
            if (lx + lw > bx1)
                bx1 = (uint16_t)(lx + lw);
            continue;
        }
        add_text(s, font, color, lx, (uint16_t)y, t + ln.start[i], ln.len[i], ell);
    }

    if (pad && y0 >= 0)
        add_rect(s, (uint16_t)(bx0 - pad), (uint16_t)(y0 - pad), (uint16_t)(bx1 - 1 + pad), (uint16_t)(y1 + pad), box);
}

static void place_barcode(rd_scene_t *s, const rd_elem_t *e, const uint8_t *t, uint8_t len)
{
    rd_bars_t *b = &s->bars;
    uint8_t sym = e->font[0], mw = e->lines;
    uint16_t n, total, used;

    if (s->nbars >= RD_MAX_BARCODES || !e->h)
        return;
    n = barcode_encode(sym, t, len, b->bits, s->m.w < RD_BC_MAX_W ? s->m.w : RD_BC_MAX_W);
    if (!n)
        return;
    total = (uint16_t)(n + barcode_quiet_left(sym) + barcode_quiet_right(sym));
    if (!mw)
        mw = (uint8_t)(e->w / total > ESL_BC_MODULE_MAX ? ESL_BC_MODULE_MAX : e->w / total);
    if (!mw || mw > ESL_BC_MODULE_MAX || total * mw > e->w)
        return;
    used = (uint16_t)(total * mw);
    b->x0 = (uint16_t)(e->x + barcode_quiet_left(sym) * mw);
    if ((e->opts & 0x03) == ESL_LE_RIGHT)
        b->x0 += (uint16_t)(e->w - used);
    else if ((e->opts & 0x03) == ESL_LE_CENTER)
        b->x0 += (uint16_t)((e->w - used) / 2);
    b->nmod = n;
    b->mw = mw;
    b->y0 = e->y;
    b->y1 = (uint16_t)(e->y + e->h - 1);
    s->nbars = 1;
}

/* ---- the layout file (esl_layout_bin_t) ---- */

typedef struct {
    const uint8_t *bin;
    uint16_t size;
    uint16_t text_pos; /* where the fixed texts start, 0 = no file */
} rd_file_t;

/* Where the fixed texts start: after the header and every layout's elements */
static void file_open(rd_file_t *fl, const esl_layout_bin_t *file)
{
    uint16_t pos = ESL_LBIN_HDR_LEN;
    uint8_t k;

    fl->bin = file ? file->bin : 0;
    fl->size = file ? file->size : 0;
    fl->text_pos = 0;
    if (!file || file->size < ESL_LBIN_HDR_LEN)
        return;
    for (k = file->bin[2]; k; k--) {
        if (pos >= file->size)
            return;
        pos = (uint16_t)(pos + 1 + (uint16_t)file->bin[pos] * ESL_LE_SIZE);
    }
    fl->text_pos = pos;
}

static const uint8_t *fixed_text(const rd_file_t *fl, uint8_t i, uint8_t *len)
{
    uint16_t pos = fl->text_pos;

    *len = 0;
    if (!pos || i >= fl->bin[ESL_LBIN_OFF_NTEXT])
        return fl->bin;
    while (i--)
        pos = (uint16_t)(pos + 1 + fl->bin[pos]);
    *len = fl->bin[pos];
    return fl->bin + pos + 1;
}

/* Elements of layout n (0-based), or NULL */
static const uint8_t *file_find(const rd_file_t *fl, uint8_t n, uint8_t *nelem)
{
    uint16_t pos = ESL_LBIN_HDR_LEN;

    if (!fl->text_pos || n >= fl->bin[2])
        return 0;
    for (;;) {
        *nelem = fl->bin[pos++];
        if (!n--)
            return fl->bin + pos;
        pos = (uint16_t)(pos + (uint16_t)*nelem * ESL_LE_SIZE);
    }
}

static void decode(rd_elem_t *e, const uint8_t *p)
{
    e->kind = p[ESL_LE_OFF_KIND];
    e->field = p[ESL_LE_OFF_FIELD];
    e->font[0] = p[ESL_LE_OFF_FONT];
    e->font[1] = p[ESL_LE_OFF_FONT + 1];
    e->font[2] = p[ESL_LE_OFF_FONT + 2];
    e->opts = p[ESL_LE_OFF_OPTS];
    e->lines = p[ESL_LE_OFF_LINES];
    e->color = p[ESL_LE_OFF_COLOR];
    e->x = ESL_GET_U16(p + ESL_LE_OFF_X);
    e->y = p[ESL_LE_OFF_Y];
    e->w = ESL_GET_U16(p + ESL_LE_OFF_W);
    e->h = p[ESL_LE_OFF_H];
}

/* ---- the built-in layouts, one set per screen size (tag/src/layout.c) ---- */

#define RD_TEXT_LINES 4
#define RD_TOP_Y      4

typedef struct {
    rd_elem_t e[RD_TEXT_LINES + 4 + 1];
} rd_builtin_t;

static rd_elem_t T(uint8_t field, uint8_t f0, uint8_t f1, uint8_t f2, uint8_t opts, uint8_t lines, uint8_t color,
                   uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    rd_elem_t e = { ESL_LE_TEXT, field, { f0, f1, f2 }, opts, lines, color, x, y, w, h };
    return e;
}

static rd_elem_t R(uint8_t color, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    rd_elem_t e = { ESL_LE_RECT, 0, { ESL_FONT_NONE, ESL_FONT_NONE, ESL_FONT_NONE }, 0, 0, color, x, y, w, h };
    return e;
}

static void frame_and_end(const rd_model_t *m, rd_elem_t *e)
{
    e[0] = R(ESL_LE_FRAME, 0, 0, m->w, RD_FRAME_W);
    e[1] = R(ESL_LE_FRAME, 0, (uint16_t)(m->h - RD_FRAME_W), m->w, RD_FRAME_W);
    e[2] = R(ESL_LE_FRAME, 0, 0, RD_FRAME_W, m->h);
    e[3] = R(ESL_LE_FRAME, (uint16_t)(m->w - RD_FRAME_W), 0, RD_FRAME_W, m->h);
    memset(&e[4], 0, sizeof(e[4]));
    e[4].kind = LE_END;
}

static void builtin(const rd_model_t *m, uint8_t id, rd_builtin_t *b)
{
    const uint8_t NO = ESL_FONT_NONE;
    uint16_t inner_w = (uint16_t)(m->w - 2 * m->margin);
    uint16_t sub_y = (uint16_t)(m->h - RD_FRAME_W - 17);
    uint16_t name_h = m->h >= 160 ? 75 : 51;
    uint16_t price_y = (uint16_t)(RD_TOP_Y + name_h + 1);
    uint16_t pitch = (uint16_t)((m->h - 2 * RD_TOP_Y) / RD_TEXT_LINES);
    rd_elem_t *e = b->e;

    if (id == ESL_LAYOUT_PRICE) {
        e[0] = T(0, ESL_FONT_MEDIUM, ESL_FONT_SMALL, NO, ESL_LE_LEFT, 0, ESL_LE_BLACK, m->margin, RD_TOP_Y, inner_w, name_h);
        e[1] = T(1, ESL_FONT_LARGE, ESL_FONT_MID, ESL_FONT_MEDIUM, ESL_LE_RIGHT | ESL_LE_BOTTOM, 1, ESL_LE_PRICE_HL,
                 m->margin, price_y, inner_w, (uint16_t)(sub_y - price_y));
        e[2] = T(2, ESL_FONT_SMALL, NO, NO, ESL_LE_LEFT, 1, ESL_LE_BLACK, m->margin, sub_y, inner_w, 17);
        frame_and_end(m, e + 3);
    } else {
        uint8_t i;
        for (i = 0; i < RD_TEXT_LINES; i++)
            e[i] = T(i, ESL_FONT_MEDIUM, ESL_FONT_SMALL, NO, ESL_LE_LEFT | ESL_LE_MIDDLE, 0, ESL_LE_BLACK, m->margin,
                     (uint16_t)(RD_TOP_Y + i * pitch), inner_w, pitch);
        frame_and_end(m, e + RD_TEXT_LINES);
    }
}

/* ---- putting a layout together ---- */

static void apply_list(rd_scene_t *s, const rd_file_t *fl, const rd_elem_t *e, const uint8_t *wire, uint8_t n,
                       const uint8_t *const *text, const uint8_t *lens, uint8_t nfields, uint8_t flags)
{
    rd_elem_t wire_elem;
    uint8_t color;

    scene_clear(s);
    for (;;) {
        if (!e) {
            if (!n--)
                break;
            decode(&wire_elem, wire);
            wire += ESL_LE_SIZE;
            e = &wire_elem;
        } else if (e->kind == LE_END) {
            break;
        }
        color = resolve_color(e->color, flags);
        if (color != COLOR_NONE) {
            if (e->kind == ESL_LE_RECT) {
                if (color == COLOR_INVERT)
                    color = RD_BLACK;
                else if (color == COLOR_RED_INVERT)
                    color = RD_RED;
                if (e->w && e->h)
                    add_rect(s, e->x, e->y, (uint16_t)(e->x + e->w - 1), (uint16_t)(e->y + e->h - 1), color);
            } else {
                const uint8_t *t = 0;
                uint8_t len = 0;
                if (e->field & ESL_LE_FIELD_FIXED) {
                    t = fixed_text(fl, (uint8_t)(e->field & ~ESL_LE_FIELD_FIXED), &len);
                } else if (e->field < nfields) {
                    t = text[e->field];
                    len = lens[e->field];
                }
                if (len && e->kind == ESL_LE_TEXT)
                    place_text(s, e, t, len, color);
                else if (len && e->kind == ESL_LE_BARCODE)
                    place_barcode(s, e, t, len);
            }
        }
        if (wire)
            e = 0;
        else
            e++;
    }
}

size_t esl_render(uint8_t model, const esl_layout_bin_t *file, const esl_content_t *c, uint8_t *out, size_t cap)
{
    static rd_scene_t scene; /* 1 KB; not reentrant, see esl_render.h */
    static rd_builtin_t bi;
    rd_model_t m;
    rd_file_t fl;
    const uint8_t *text[ESL_MAX_FIELDS];
    uint8_t lens[ESL_MAX_FIELDS];
    const uint8_t *p = c->text;
    uint8_t black[32], red[32];
    size_t size, plane, i;
    uint16_t x;
    int index = -1;

    if (!model_get(model, &m) || m.col_bytes > sizeof(black))
        return 0;
    size = esl_render_size(model);
    if (cap < size)
        return 0;

    for (i = 0; i < ESL_MAX_FIELDS; i++) {
        text[i] = p;
        lens[i] = i < c->nfields ? c->len[i] : 0;
        p += lens[i];
    }

    file_open(&fl, file);
    if (c->layout == ESL_LAYOUT_NAMED) {
        /* A name the file does not have is not drawn: the AP holds such content back
           (esl_store content_err) and the tag keeps what it shows */
        if (file)
            index = esl_layout_bin_find(file, c->name, strlen(c->name));
        if (index < 0)
            return 0;
    } else if (c->layout >= ESL_LAYOUT_FILE) {
        index = c->layout - ESL_LAYOUT_FILE;
    }

    scene.m = m;
    if (index >= 0) {
        uint8_t n;
        const uint8_t *e = file_find(&fl, (uint8_t)index, &n);
        if (e) {
            apply_list(&scene, &fl, 0, e, n, text, lens, ESL_MAX_FIELDS, c->flags);
            index = -2; /* drawn */
        }
    }
    if (index != -2) {
        builtin(&m, c->layout == ESL_LAYOUT_PRICE ? ESL_LAYOUT_PRICE : ESL_LAYOUT_TEXT, &bi);
        apply_list(&scene, &fl, bi.e, 0, 0, text, lens, ESL_MAX_FIELDS, c->flags);
    }

    plane = (size_t)m.w * m.col_bytes;
    draw_begin(&scene);
    for (x = 0; x < m.w; x++) {
        draw_column(&scene, x, black, red);
        memcpy(out + (size_t)x * m.col_bytes, black, m.col_bytes);
        if (m.has_red)
            memcpy(out + plane + (size_t)x * m.col_bytes, red, m.col_bytes);
    }
    return size;
}
