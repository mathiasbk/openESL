/* esl_render.c - draws a tag's screen on the AP, with the screen size as a run-time
 * parameter. It began as the tag's own drawing (layout.c and draw.c, before the tags
 * only showed images); sim/reference/ holds the CRC of every screen, so a change to the
 * fitting, wrapping or placement shows up there. The fonts (tag/src/font.c, tag/fonts)
 * and the barcodes (tag/src/barcode.c) are the tag's files, compiled into esl_core. */
#include "esl_render.h"
#include <string.h>
#include "barcode.h" /* tag/src */
#include "esl_clock.h"
#include "esl_qr.h"
#include "font.h"    /* tag/src */
#include "esl_text.h"
#include "models_gen.h"

/* The fonts it draws with: the built-in ones (tag/fonts) and the font files (esl_font.c) */
const font_t *esl_font_get(uint8_t id);

/* ---- the model ------------------------------------------------------------- */

typedef struct {
    uint16_t w, h;
    uint8_t col_bytes;
    bool has_red;
    uint8_t margin;    /* left and right of the built-in layouts */
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

/* ---- the scene ------------------------------------------------------------- */

#define RD_MAX_RUNS     ESL_DRAW_CAP_RUNS
#define RD_MAX_RECTS    ESL_DRAW_CAP_RECTS
#define RD_MAX_BARCODES ESL_DRAW_MAX_BARCODES
#define RD_MAX_NUMS     ESL_DRAW_MAX_NUMS
#define RD_MAX_LINES_DRAWN ESL_DRAW_CAP_LINES
#define RD_MAX_ICONS    ESL_DRAW_CAP_ICONS
#define RD_MAX_SHAPES   ESL_DRAW_CAP_SHAPES
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
    /* A turned run (ESL_LE_ROT): rot 1..3 = 90, 180, 270 degrees clockwise, x and y are in the
     * element's box turned with it (its top-left corner is 0, 0), and bx0..by1 is the box on
     * the screen. 0 = not turned: x and y are on the screen. */
    uint8_t rot;
    uint16_t bx0, by0, bx1, by1;
} rd_run_t;

typedef struct {
    uint16_t x0, x1;
    uint16_t y0, y1;
    uint8_t color;
    uint8_t radius;    /* rounded corners, 0 = square */
    uint8_t border;    /* 0 = filled, else only a border this wide */
} rd_rect_t;

/* A line from (ax, ay) to (bx, by), width w. A slanted one is the pixels whose centre is at
 * most w / 2 from the segment, drawn between columns x0 and x1, rows y0 and y1 (the box
 * around it). A straight one is exactly that box: w pixels thick, with square ends. */
typedef struct {
    int16_t ax, ay, bx, by;
    uint8_t w;
    uint8_t color;
    bool straight;
    int16_t x0, x1, y0, y1;
} rd_line_t;

/* An ellipse in the box x0..x1, y0..y1, or a polygon with n corners (on the screen) */
typedef struct {
    uint8_t kind;      /* ESL_LE_ELLIPSE or ESL_LE_POLYGON */
    uint8_t color;
    uint8_t border;    /* 0 = filled, else only a border this wide */
    uint8_t n;
    uint16_t x0, x1;
    uint16_t y0, y1;
    int16_t px[ESL_POLY_MAX], py[ESL_POLY_MAX];
} rd_shape_t;

/* A chart (ESL_LE_CHART): n points, point i at column x0 + px(i) with its line's centre on row y[i] */
typedef struct {
    uint16_t x0, w;        /* the box */
    uint16_t y0, y1;
    uint8_t n;
    uint8_t lw;            /* line width */
    uint8_t opts;          /* ESL_LE_CHART_FILL, _LIGHT */
    uint8_t color;
    uint16_t y[ESL_CHART_MAX];
} rd_chart_t;

typedef struct {
    const uint8_t *data;   /* the icon's columns in the layout file */
    uint16_t x, w;
    uint16_t y;
    uint8_t col_bytes;
    uint8_t color;
} rd_icon_t;

typedef struct {
    uint16_t x0;
    uint16_t nmod;
    uint8_t mw;
    uint16_t y0, y1;
    uint8_t bits[(RD_BC_MAX_W + 7) / 8];
} rd_bars_t;

typedef struct {
    uint16_t x0, y0;     /* the top-left of the quiet zone; the code starts ESL_QR_QUIET modules in */
    uint8_t size;        /* modules per side, without the quiet zone */
    uint8_t m;           /* pixels per module */
    uint8_t bits[ESL_QR_BYTES];
} rd_qr_t;

typedef struct {
    rd_model_t m;
    uint8_t nruns, nrects, nbars, nlines, nicons, nqr, nshapes, ncharts;
    rd_run_t run[RD_MAX_RUNS];
    rd_rect_t rect[RD_MAX_RECTS];
    rd_line_t line[RD_MAX_LINES_DRAWN];
    rd_icon_t icon[RD_MAX_ICONS];
    rd_shape_t shape[RD_MAX_SHAPES];
    rd_chart_t chart[ESL_DRAW_MAX_CHARTS];
    rd_bars_t bars;
    /* Formatted numbers (ESL_LE_NUM_*); the runs point into them */
    uint8_t nnums;
    uint8_t num[RD_MAX_NUMS][ESL_LE_NUM_MAX_LEN];
    /* Fixed texts with their date fields filled in (esl_clock.h); the runs point into them */
    uint8_t nclocks;
    uint8_t clock[ESL_CLOCK_TEXTS][ESL_CLOCK_TEXT_MAX];
    /* place_text is placing a formatted number; one fitted in none of its fonts */
    uint8_t num_now, num_cut;
    rd_qr_t qr;
} rd_scene_t;

static void scene_clear(rd_scene_t *s)
{
    s->nruns = 0;
    s->nrects = 0;
    s->nbars = 0;
    s->nnums = 0;
    s->nclocks = 0;
    s->num_now = 0;
    s->num_cut = 0;
    s->nlines = 0;
    s->nicons = 0;
    s->nqr = 0;
    s->nshapes = 0;
    s->ncharts = 0;
}

static bool add_text(rd_scene_t *s, uint8_t font, uint8_t color, uint16_t x, uint16_t y, const uint8_t *text,
                     uint8_t len, uint8_t ellipsis)
{
    const font_t *f = esl_font_get(font);
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
    r->rot = 0;
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
    r->radius = 0;
    r->border = 0;
    return true;
}

static void add_line(rd_scene_t *s, int16_t ax, int16_t ay, int16_t bx, int16_t by, uint8_t w, uint8_t color)
{
    rd_line_t *l;
    int16_t r = (int16_t)((w + 1) / 2);
    if (s->nlines >= RD_MAX_LINES_DRAWN)
        return;
    l = &s->line[s->nlines++];
    l->ax = ax;
    l->ay = ay;
    l->bx = bx;
    l->by = by;
    l->w = w;
    l->color = color;
    l->straight = ax == bx || ay == by;
    l->x0 = (int16_t)((ax < bx ? ax : bx) - r);
    l->x1 = (int16_t)((ax > bx ? ax : bx) + r);
    l->y0 = (int16_t)((ay < by ? ay : by) - r);
    l->y1 = (int16_t)((ay > by ? ay : by) + r);
    if (l->straight) {
        /* the box itself along the line, w pixels across it, (w - 1) / 2 of them before it */
        int16_t before = (int16_t)((w - 1) / 2), after = (int16_t)(w / 2);
        if (ay == by) {
            l->x0 = ax < bx ? ax : bx;
            l->x1 = ax > bx ? ax : bx;
            l->y0 = (int16_t)(ay - before);
            l->y1 = (int16_t)(ay + after);
        }
        if (ax == bx) {
            l->x0 = (int16_t)(ax - before);
            l->x1 = (int16_t)(ax + after);
            if (ay != by) {
                l->y0 = ay < by ? ay : by;
                l->y1 = ay > by ? ay : by;
            }
        }
    }
    if (l->y0 < 0)
        l->y0 = 0;
    if (l->y1 >= (int16_t)s->m.h)
        l->y1 = (int16_t)(s->m.h - 1);
}

static rd_shape_t *add_shape(rd_scene_t *s, uint8_t kind, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             uint8_t border, uint8_t color)
{
    rd_shape_t *sh;
    if (s->nshapes >= RD_MAX_SHAPES)
        return 0;
    sh = &s->shape[s->nshapes++];
    sh->kind = kind;
    sh->color = color;
    sh->border = border;
    sh->n = 0;
    sh->x0 = x;
    sh->x1 = (uint16_t)(x + w - 1);
    sh->y0 = y;
    sh->y1 = (uint16_t)(y + h - 1);
    return sh;
}

static void add_icon(rd_scene_t *s, const uint8_t *data, uint16_t x, uint16_t y, uint16_t w, uint8_t h, uint8_t color)
{
    rd_icon_t *ic;
    if (s->nicons >= RD_MAX_ICONS)
        return;
    ic = &s->icon[s->nicons++];
    ic->data = data;
    ic->x = x;
    ic->y = y;
    ic->w = w;
    ic->col_bytes = (uint8_t)((h + 7) / 8);
    ic->color = color;
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

/* Rows left out at the top (and bottom) of column i of a corner with radius r, i counted
 * from the rect's nearer side: the pixels whose centre is outside the quarter circle. */
static uint16_t corner_inset(uint8_t r, uint16_t i)
{
    int32_t dx, dy;
    uint16_t j;

    if (i >= r)
        return 0;
    dx = 2 * (int32_t)r - 2 * (int32_t)i - 1;
    for (j = 0; j < r; j++) {
        dy = 2 * (int32_t)r - 2 * (int32_t)j - 1;
        if (dx * dx + dy * dy <= 4 * (int32_t)r * r)
            break;
    }
    return j;
}

/* The rows of column x inside a rect with rounded corners; false if none */
static bool rect_span(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1, uint8_t r, uint16_t x, uint16_t *top,
                      uint16_t *bottom)
{
    uint16_t i = (uint16_t)(x - x0 < x1 - x ? x - x0 : x1 - x);
    uint16_t in = corner_inset(r, i);
    if (y0 + in > y1 - in || in > y1)
        return false;
    *top = (uint16_t)(y0 + in);
    *bottom = (uint16_t)(y1 - in);
    return true;
}

static void draw_rect(const rd_model_t *mo, const rd_rect_t *r, uint16_t x, uint8_t *black, uint8_t *red)
{
    uint16_t t, b, it, ib, last = (uint16_t)(mo->h - 1);
    uint8_t ri;

    if (x < r->x0 || x > r->x1 || r->y0 > r->y1 || r->y0 >= mo->h)
        return;
    if (!rect_span(r->x0, r->x1, r->y0, r->y1, r->radius, x, &t, &b) || t > last)
        return;
    if (b > last)
        b = last;
    if (r->border && r->x0 + r->border <= r->x1 - r->border && r->y0 + r->border <= r->y1 - r->border &&
        x >= r->x0 + r->border && x <= r->x1 - r->border) {
        ri = r->radius > r->border ? (uint8_t)(r->radius - r->border) : 0;
        if (rect_span((uint16_t)(r->x0 + r->border), (uint16_t)(r->x1 - r->border), (uint16_t)(r->y0 + r->border),
                      (uint16_t)(r->y1 - r->border), ri, x, &it, &ib)) {
            if (it > t)
                fill_span(black, red, t, (uint16_t)(it - 1 < b ? it - 1 : b), r->color);
            if (ib < b)
                fill_span(black, red, (uint16_t)(ib + 1), b, r->color);
            return;
        }
    }
    fill_span(black, red, t, b, r->color);
}

/* Is the centre of pixel (x, y) at most w / 2 from the segment (ax, ay)-(bx, by)? In whole
 * numbers, so every platform draws the same pixels: 4 d^2 <= w^2 with d the distance to it. */
static bool seg_near(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t x, int32_t y, int32_t w)
{
    int32_t dx = bx - ax, dy = by - ay;
    int32_t px = x - ax, py = y - ay;
    int64_t len2 = (int64_t)dx * dx + (int64_t)dy * dy;
    int64_t dot = (int64_t)px * dx + (int64_t)py * dy;
    int64_t w2 = (int64_t)w * w;

    if (dot <= 0 || len2 == 0)
        return 4 * ((int64_t)px * px + (int64_t)py * py) <= w2;
    if (dot >= len2) {
        int64_t qx = x - bx, qy = y - by;
        return 4 * (qx * qx + qy * qy) <= w2;
    }
    {
        int64_t cross = (int64_t)px * dy - (int64_t)py * dx;
        return 4 * cross * cross <= w2 * len2;
    }
}

static bool line_hit(const rd_line_t *l, int32_t x, int32_t y)
{
    return seg_near(l->ax, l->ay, l->bx, l->by, x, y, l->w);
}

static void draw_line(const rd_line_t *l, uint16_t x, uint8_t *black, uint8_t *red)
{
    int16_t y, top = -1, bottom = -1;

    if ((int16_t)x < l->x0 || (int16_t)x > l->x1 || l->y0 > l->y1)
        return;
    if (l->straight) {
        fill_span(black, red, (uint16_t)l->y0, (uint16_t)l->y1, l->color);
        return;
    }
    /* the pixels of a line in one column are one run (the shape is convex) */
    for (y = l->y0; y <= l->y1; y++) {
        if (line_hit(l, x, y)) {
            if (top < 0)
                top = y;
            bottom = y;
        } else if (top >= 0) {
            break;
        }
    }
    if (top >= 0)
        fill_span(black, red, (uint16_t)top, (uint16_t)bottom, l->color);
}

/* Is the centre of pixel (x, y) in the ellipse with doubled semi-axes a and b (the box's
 * width and height) round the box's centre? dx and dy are 2x and 2y less the doubled centre.
 * strict: on the edge counts as outside. */
static bool in_ellipse(int64_t dx, int64_t dy, int64_t a, int64_t b, bool strict)
{
    int64_t l = dx * dx * b * b + dy * dy * a * a, r = a * a * b * b;
    return strict ? l < r : l <= r;
}

/* The first row from the top of sh's box whose pixel in the column dx (doubled, from the
 * centre) is in the ellipse a, b; the rows down to its mirror (y0 + y1 - row) are all in it.
 * Past the middle when there is none. */
static int32_t ellipse_top(const rd_shape_t *sh, int64_t dx, int64_t a, int64_t b, int32_t from, bool strict)
{
    int32_t y, mid = (sh->y0 + sh->y1) / 2;
    for (y = from; y <= mid && !in_ellipse(dx, 2 * (int64_t)y - sh->y0 - sh->y1, a, b, strict); y++)
        ;
    return y;
}

/* Rows a..b of a column bitmap (bit 7 of byte 0 = row 0) */
static void rows_set(uint8_t *m, int32_t a, int32_t b)
{
    for (; a <= b; a++)
        m[a >> 3] |= (uint8_t)(0x80 >> (a & 7));
}

/* The rows lo..hi of column x at most w / 2 from the segment: one run (a capsule is convex)
 * round the row nearest to it, which is in it when any is */
static void seg_rows(uint8_t *m, int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t x, int32_t w, int32_t lo,
                     int32_t hi)
{
    int32_t y, t, b;
    if (ax == bx) {
        y = (ay + by) / 2;
    } else {
        /* the segment at the column (or its nearer end), rounded: whole numbers below 2^26 */
        int32_t cx = x < (ax < bx ? ax : bx) ? (ax < bx ? ax : bx) : x > (ax > bx ? ax : bx) ? (ax > bx ? ax : bx) : x;
        int32_t den = bx - ax, num = ay * den + (cx - ax) * (by - ay);
        if (den < 0) {
            den = -den;
            num = -num;
        }
        y = (2 * num + den) / (2 * den);
    }
    if (y < lo || y > hi || !seg_near(ax, ay, bx, by, x, y, w))
        return;
    for (t = y; t > lo && seg_near(ax, ay, bx, by, x, t - 1, w); t--)
        ;
    for (b = y; b < hi && seg_near(ax, ay, bx, by, x, b + 1, w); b++)
        ;
    rows_set(m, t, b);
}

/* An ellipse or polygon in column x. Worked out per column, not per pixel: an ellipse from its
 * top row (it is the same mirrored), a polygon from where its edges cross the column and the
 * rows near each edge. The same pixels as the definition in shared/proto.h. */
static void draw_shape(const rd_model_t *mo, const rd_shape_t *sh, uint16_t x, uint8_t *black, uint8_t *red)
{
    if (x < sh->x0 || x > sh->x1 || sh->y1 >= mo->h)
        return;
    if (sh->kind == ESL_LE_ELLIPSE) {
        int64_t a = sh->x1 - sh->x0 + 1, b = sh->y1 - sh->y0 + 1, dx = 2 * (int64_t)x - sh->x0 - sh->x1;
        int32_t top = ellipse_top(sh, dx, a, b, sh->y0, false), bottom = sh->y0 + sh->y1 - top, in, bd = sh->border;
        if (top > bottom)
            return;
        /* a border: not inside the ellipse border pixels in from the box on every side */
        in = bd && a > 2 * bd && b > 2 * bd ? ellipse_top(sh, dx, a - 2 * bd, b - 2 * bd, top, true) : bottom + 1;
        if (in > (sh->y0 + sh->y1) / 2) {
            fill_span(black, red, (uint16_t)top, (uint16_t)bottom, sh->color);
        } else if (in > top) {
            fill_span(black, red, (uint16_t)top, (uint16_t)(in - 1), sh->color);
            fill_span(black, red, (uint16_t)(sh->y0 + sh->y1 - in + 1), (uint16_t)bottom, sh->color);
        }
        return;
    }
    /* polygon: inside (even-odd along the column), near an edge (1/2 px), near an edge (border) */
    uint8_t ins[32] = {0}, edge[32] = {0}, band[32] = {0};
    int32_t ys[ESL_POLY_MAX], n = 0, k;
    for (uint8_t i = 0, j = (uint8_t)(sh->n - 1); i < sh->n; j = i++) {
        int32_t xi = sh->px[i], yi = sh->py[i], xj = sh->px[j], yj = sh->py[j];
        seg_rows(edge, xi, yi, xj, yj, x, 1, sh->y0, sh->y1);
        if (sh->border)
            seg_rows(band, xi, yi, xj, yj, x, 2 * (int32_t)sh->border - 1, sh->y0, sh->y1);
        if ((xi > (int32_t)x) != (xj > (int32_t)x)) {
            /* the first row below where the edge crosses the column */
            int32_t den = xj - xi, num = yi * den + ((int32_t)x - xi) * (yj - yi);
            if (den < 0) {
                den = -den;
                num = -num;
            }
            int32_t y = num / den + 1;
            for (k = n++; k > 0 && ys[k - 1] > y; k--)
                ys[k] = ys[k - 1];
            ys[k] = y;
        }
    }
    for (k = 0; k + 1 < n; k += 2)
        rows_set(ins, ys[k], ys[k + 1] - 1);
    for (k = 0; k < mo->col_bytes; k++) {
        uint8_t m = (uint8_t)(edge[k] | (sh->border ? ins[k] & band[k] : ins[k]));
        if (m)
            ink(black, red, (uint16_t)k, m, sh->color);
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
    f = esl_font_get(r->font);
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

/* Pixel y of a column, if it is on the screen */
static void ink_px(const rd_model_t *mo, uint8_t *black, uint8_t *red, int32_t y, uint8_t color)
{
    if (y >= 0 && y < mo->h)
        ink(black, red, (uint16_t)(y >> 3), (uint8_t)(0x80 >> (y & 7)), color);
}

/* ---- charts ---- */

/* The column of point i of chart c, from its left edge */
static uint16_t chart_px(const rd_chart_t *c, uint8_t i)
{
    return (uint16_t)((uint32_t)i * (c->w - 1u) / (c->n - 1u));
}

/* The row of the line's centre in column col (from the left edge): between the points either
   side of it, rounded half away from the first */
static int32_t chart_row(const rd_chart_t *c, uint16_t col)
{
    uint8_t i = 0;
    while (i + 2 < c->n && chart_px(c, (uint8_t)(i + 1)) <= col)
        i++;
    int32_t a = chart_px(c, i), b = chart_px(c, (uint8_t)(i + 1)), ya = c->y[i], yb = c->y[i + 1];
    if (b == a)
        return yb;
    int32_t num = (yb - ya) * ((int32_t)col - a), den = b - a;
    return ya + (num >= 0 ? (2 * num + den) / (2 * den) : -((-2 * num + den) / (2 * den)));
}

static void draw_chart(const rd_model_t *mo, const rd_chart_t *c, uint16_t x, uint8_t *black, uint8_t *red)
{
    if (x < c->x0 || x >= c->x0 + c->w)
        return;
    uint16_t col = (uint16_t)(x - c->x0);
    int32_t y = chart_row(c, col), lo = y, hi = y;
    /* halfway to the rows next to it, so a steep line has no gaps */
    for (int d = -1; d <= 1; d += 2) {
        if ((d < 0 && col == 0) || (d > 0 && col + 1u >= c->w))
            continue;
        int32_t m = chart_row(c, (uint16_t)(col + d));
        int32_t half = m > y ? (y + m) / 2 : (y + m + 1) / 2;
        if (half < lo)
            lo = half;
        if (half > hi)
            hi = half;
    }
    /* every point in this column (more points than columns) */
    for (uint8_t i = 0; i < c->n; i++)
        if (chart_px(c, i) == col) {
            if (c->y[i] < lo)
                lo = c->y[i];
            if (c->y[i] > hi)
                hi = c->y[i];
        }
    lo -= (c->lw - 1) / 2;
    hi += c->lw / 2;
    if (lo < c->y0)
        lo = c->y0;
    if (hi > c->y1)
        hi = c->y1;
    if (c->opts & ESL_LE_CHART_FILL) {
        int32_t from = y + c->lw / 2 + 1;
        if (!(c->opts & ESL_LE_CHART_LIGHT)) {
            if (from <= c->y1)
                fill_span(black, red, (uint16_t)from, c->y1, c->color);
        } else {
            for (int32_t r = from; r <= c->y1; r++)
                if (((uint32_t)r + x) % 2 == 0)
                    ink_px(mo, black, red, r, c->color);
        }
    }
    fill_span(black, red, (uint16_t)lo, (uint16_t)hi, c->color);
}

/* Numbers apart by spaces or ';' (shared/proto.h ESL_LE_CHART) as whole numbers times
   10^ESL_CHART_DECIMALS into v (ESL_CHART_MAX); how many, 0 when the text is not such a list */
static uint8_t chart_numbers(const uint8_t *t, uint8_t len, int64_t *v)
{
    uint8_t n = 0, i = 0;
    while (i < len && n < ESL_CHART_MAX) {
        if (t[i] == ' ' || t[i] == ';') {
            i++;
            continue;
        }
        bool neg = t[i] == '-';
        if (neg)
            i++;
        int64_t a = 0;
        unsigned digits = 0, dec = 0;
        while (i < len && t[i] >= '0' && t[i] <= '9' && digits < 10) {
            a = a * 10 + (t[i++] - '0');
            digits++;
        }
        if (!digits)
            return 0;
        if (i < len && (t[i] == '.' || t[i] == ',')) {
            i++;
            while (i < len && t[i] >= '0' && t[i] <= '9' && dec < ESL_CHART_DECIMALS) {
                a = a * 10 + (t[i++] - '0');
                dec++;
            }
            if (!dec)
                return 0;
        }
        if (i < len && t[i] != ' ' && t[i] != ';')
            return 0; /* a letter, an 11th digit, a 5th decimal */
        for (; dec < ESL_CHART_DECIMALS; dec++)
            a *= 10;
        v[n++] = neg ? -a : a;
    }
    return n;
}

/* A chart of the numbers in t in the box of e */
static void add_chart(rd_scene_t *s, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t lw, uint8_t opts,
                      uint8_t color, const uint8_t *t, uint8_t len)
{
    int64_t v[ESL_CHART_MAX], lo, hi;
    uint8_t n = chart_numbers(t, len, v);
    if (n < 2 || s->ncharts >= ESL_DRAW_MAX_CHARTS || !w || !h)
        return;
    rd_chart_t *c = &s->chart[s->ncharts++];
    c->x0 = x;
    c->w = w;
    c->y0 = y;
    c->y1 = (uint16_t)(y + h - 1);
    c->n = n;
    c->lw = lw < 1 ? 1 : lw > ESL_CHART_W_MAX ? ESL_CHART_W_MAX : lw;
    c->opts = opts;
    c->color = color;
    lo = hi = v[0];
    for (uint8_t i = 1; i < n; i++) {
        if (v[i] < lo)
            lo = v[i];
        if (v[i] > hi)
            hi = v[i];
    }
    /* the rows the line's centre can take: its whole width stays in the box */
    int32_t top = y + (c->lw - 1) / 2, bottom = (int32_t)y + h - 1 - c->lw / 2;
    if (bottom < top)
        bottom = top;
    for (uint8_t i = 0; i < n; i++) {
        if (hi == lo)
            c->y[i] = (uint16_t)((top + bottom) / 2);
        else
            c->y[i] = (uint16_t)(top + ((hi - v[i]) * (bottom - top) * 2 + (hi - lo)) / (2 * (hi - lo)));
    }
}

/* Column x of a turned run. At 90 and 270 degrees a screen column is one row of every glyph,
 * at 180 one glyph column upside down. */
static void draw_run_rot(const rd_model_t *mo, const rd_run_t *r, uint16_t x, uint8_t *black, uint8_t *red)
{
    const font_t *f = esl_font_get(r->font);
    uint8_t n = (uint8_t)(r->len + r->ellipsis), i, ch = 0, w = 0, c;
    int32_t u = 0;

    if (r->rot == 2) {
        int32_t at = (int32_t)r->bx1 - x - r->x; /* along the run */
        uint8_t row;
        const uint8_t *col;
        if (at < 0 || at >= r->w)
            return;
        for (i = 0; i < n; i++, u += w) {
            ch = font_map(f, run_char(r, i));
            w = ch ? f->widths[ch - f->first] : 0;
            if (at < u + w)
                break;
        }
        if (i == n)
            return;
        col = font_column(f, ch, (uint8_t)(at - u));
        for (row = 0; row < f->height; row++)
            if (col[row >> 3] & (0x80 >> (row & 7)))
                ink_px(mo, black, red, (int32_t)r->by1 - r->y - row, r->color);
        return;
    }

    {
        /* the row of the glyphs in this column */
        int32_t v = r->rot == 1 ? (int32_t)r->bx1 - x - r->y : (int32_t)x - r->bx0 - r->y;
        uint8_t b, m;
        if (v < 0 || v >= f->height)
            return;
        b = (uint8_t)(v >> 3);
        m = (uint8_t)(0x80 >> (v & 7));
        for (i = 0; i < n; i++, u += w) {
            ch = font_map(f, run_char(r, i));
            w = ch ? f->widths[ch - f->first] : 0;
            for (c = 0; c < w; c++) {
                int32_t at = (int32_t)r->x + u + c;
                if (font_column(f, ch, c)[b] & m)
                    ink_px(mo, black, red, r->rot == 1 ? (int32_t)r->by0 + at : (int32_t)r->by1 - at, r->color);
            }
        }
    }
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

    for (i = 0; i < s->nrects; i++)
        draw_rect(mo, &s->rect[i], x, black, red);

    for (i = 0; i < s->nshapes; i++)
        draw_shape(mo, &s->shape[i], x, black, red);

    for (i = 0; i < s->nlines; i++)
        draw_line(&s->line[i], x, black, red);

    for (i = 0; i < s->ncharts; i++)
        draw_chart(mo, &s->chart[i], x, black, red);

    for (i = 0; i < s->nicons; i++) {
        const rd_icon_t *ic = &s->icon[i];
        if (x >= ic->x && x < ic->x + ic->w)
            blit_column(mo, black, red, ic->data + (size_t)(x - ic->x) * ic->col_bytes, ic->col_bytes, ic->y,
                        ic->color);
    }

    if (s->nbars && x >= s->bars.x0) {
        const rd_bars_t *b = &s->bars;
        uint16_t m = (uint16_t)((x - b->x0) / b->mw);
        if (m < b->nmod && (b->bits[m >> 3] & (0x80 >> (m & 7))))
            fill_span(black, red, b->y0, b->y1, RD_BLACK);
    }

    if (s->nqr && x >= s->qr.x0 && x < s->qr.x0 + (s->qr.size + 2 * ESL_QR_QUIET) * s->qr.m) {
        const rd_qr_t *q = &s->qr;
        unsigned col = (unsigned)(x - q->x0) / q->m, r, top;
        uint16_t y0 = (uint16_t)(q->y0 + ESL_QR_QUIET * q->m);
        /* the quiet zone and the light modules are white, whatever is under them */
        fill_span(black, red, q->y0, (uint16_t)(q->y0 + (q->size + 2 * ESL_QR_QUIET) * q->m - 1), RD_WHITE);
        col = col < ESL_QR_QUIET || col >= q->size + ESL_QR_QUIET ? q->size : col - ESL_QR_QUIET; /* past the code: none */
        for (r = 0; col < q->size && r < q->size; r = top) {
            /* a run of dark modules down this column is one span */
            top = r + 1;
            if (!esl_qr_module(q->bits, q->size, col, r))
                continue;
            while (top < q->size && esl_qr_module(q->bits, q->size, col, top))
                top++;
            fill_span(black, red, (uint16_t)(y0 + r * q->m), (uint16_t)(y0 + top * q->m - 1), RD_BLACK);
        }
    }

    for (i = 0; i < s->nruns; i++) {
        if (s->run[i].rot)
            draw_run_rot(mo, &s->run[i], x, black, red);
        else
            draw_run(mo, &s->run[i], x, black, red);
    }

    if (mo->h % 8) {
        black[mo->col_bytes - 1] &= (uint8_t)(0xFF << (8 - mo->h % 8));
        red[mo->col_bytes - 1] &= (uint8_t)(0xFF << (8 - mo->h % 8));
    }
}

/* ---- the elements (shared/proto.h) ----------------------------------------- */

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
    uint8_t rot;   /* text: ESL_LE_ROT, 0..3 quarter turns clockwise */
} rd_elem_t;

/* ---- fitting text ---- */

static uint8_t layout_fit(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *ellipsis)
{
    const font_t *f = esl_font_get(font);
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
    const font_t *f = esl_font_get(font);
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
        f = esl_font_get(e->font[i]);
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

/* A run of a turned element e (on the screen), at x, y in its turned box */
static bool add_text_rot(rd_scene_t *s, const rd_elem_t *e, uint8_t font, uint8_t color, uint16_t x, uint16_t y,
                         const uint8_t *text, uint8_t len, uint8_t ellipsis)
{
    rd_run_t *r;
    if (!add_text(s, font, color, x, y, text, len, ellipsis))
        return false;
    r = &s->run[s->nruns - 1];
    r->rot = e->rot;
    r->bx0 = e->x;
    r->by0 = e->y;
    r->bx1 = (uint16_t)(e->x + e->w - 1);
    r->by1 = (uint16_t)(e->y + e->h - 1);
    return true;
}

static uint16_t clamp_px(int32_t v, uint16_t n)
{
    return (uint16_t)(v < 0 ? 0 : v >= n ? n - 1 : v);
}

/* The box behind an inverted turned element e: u0..u1 along its lines, v0..v1 across them */
static void add_rect_rot(rd_scene_t *s, const rd_elem_t *e, int32_t u0, int32_t v0, int32_t u1, int32_t v1,
                         uint8_t color)
{
    const rd_model_t *mo = &s->m;
    int32_t bx1 = e->x + e->w - 1, by1 = e->y + e->h - 1, x0, x1, y0, y1;

    if (e->rot == 1) {
        x0 = bx1 - v1, x1 = bx1 - v0, y0 = e->y + u0, y1 = e->y + u1;
    } else if (e->rot == 3) {
        x0 = e->x + v0, x1 = e->x + v1, y0 = by1 - u1, y1 = by1 - u0;
    } else {
        x0 = bx1 - u1, x1 = bx1 - u0, y0 = by1 - v1, y1 = by1 - v0;
    }
    if (x1 < 0 || y1 < 0 || x0 >= mo->w || y0 >= mo->h)
        return;
    add_rect(s, clamp_px(x0, mo->w), clamp_px(y0, mo->h), clamp_px(x1, mo->w), clamp_px(y1, mo->h), color);
}

static void place_text(rd_scene_t *s, const rd_elem_t *se, const uint8_t *t, uint8_t len, uint8_t color)
{
    const rd_model_t *mo = &s->m;
    /* A turned element is laid out in its box turned with it, from 0, 0; vmax is where the
       screen ends across its lines */
    rd_elem_t turned;
    const rd_elem_t *e = se;
    int32_t vmax = mo->h;
    const font_t *f;
    rd_lines_t ln;
    uint8_t i, font = ESL_FONT_MEDIUM, found = 0, ell, pad = 0, n, box = RD_BLACK;
    int32_t y, y0 = -1, y1 = 0;
    uint16_t lw, x, w, bx0 = 0xFFFF, bx1 = 0;
    uint16_t ey, eh;

    if (se->rot) {
        turned = *se;
        turned.x = 0;
        turned.y = 0;
        if (se->rot & 1) {
            turned.w = se->h;
            turned.h = se->w;
        }
        vmax = se->rot == 1 ? se->x + se->w : se->rot == 3 ? mo->w - se->x : se->y + se->h;
        e = &turned;
    }

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
        f = esl_font_get(font);
        n = max_lines(e, f);
        if (pad && n * f->height > eh)
            n = (uint8_t)(eh / f->height);
        if (n && font_has_all(f, t, len) && !split(&ln, font, t, len, w, n)) {
            found = 1;
            break;
        }
    }
    f = esl_font_get(font);
    if (!found && s->num_now) {
        /* a number cut short is a wrong price: it is not drawn at all, and the API
           refuses such content (esl_render_number_cut) */
        s->num_cut = 1;
        return;
    }
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
        if (y + f->height > vmax)
            break;
        if (pad) {
            if (se->rot ? !add_text_rot(s, se, font, color, lx, (uint16_t)y, t + ln.start[i], ln.len[i], ell)
                        : !add_text(s, font, color, lx, (uint16_t)y, t + ln.start[i], ln.len[i], ell))
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
        if (se->rot)
            add_text_rot(s, se, font, color, lx, (uint16_t)y, t + ln.start[i], ln.len[i], ell);
        else
            add_text(s, font, color, lx, (uint16_t)y, t + ln.start[i], ln.len[i], ell);
    }

    if (pad && y0 >= 0 && se->rot)
        add_rect_rot(s, se, bx0 - pad, y0 - pad, bx1 - 1 + pad, y1 + pad, box);
    else if (pad && y0 >= 0)
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

static void place_qr(rd_scene_t *s, const rd_elem_t *e, const uint8_t *t, uint8_t len)
{
    rd_qr_t *q = &s->qr;
    uint16_t side = e->w < e->h ? e->w : e->h, used;
    uint8_t mw = e->lines, n;

    if (s->nqr || e->font[0] > ESL_QR_LEVEL_H)
        return;
    n = esl_qr_encode(t, len, e->font[0], ESL_QR_VERSION_MAX, q->bits);
    if (!n || n + 2 * ESL_QR_QUIET > side)
        return;
    used = (uint16_t)(n + 2 * ESL_QR_QUIET); /* modules, with the quiet zone */
    if (!mw)
        mw = (uint8_t)(side / used > ESL_QR_MODULE_MAX ? ESL_QR_MODULE_MAX : side / used);
    if (!mw || used * mw > side)
        return;
    used = (uint16_t)(used * mw);
    q->size = n;
    q->m = mw;
    q->x0 = e->x;
    if ((e->opts & 0x03) == ESL_LE_RIGHT)
        q->x0 += (uint16_t)(e->w - used);
    else if ((e->opts & 0x03) == ESL_LE_CENTER)
        q->x0 += (uint16_t)((e->w - used) / 2);
    q->y0 = e->y;
    if ((e->opts & 0x0C) == ESL_LE_BOTTOM)
        q->y0 += (uint16_t)(e->h - used);
    else if ((e->opts & 0x0C) == ESL_LE_MIDDLE)
        q->y0 += (uint16_t)((e->h - used) / 2);
    s->nqr = 1;
}

/* ---- the layout file (esl_layout_bin_t) ---- */

typedef struct {
    const uint8_t *bin;
    uint16_t size;
    uint16_t text_pos; /* where the fixed texts start, 0 = no file */
    uint16_t icon_pos; /* where the icons start */
} rd_file_t;

/* Where the fixed texts start: after the header and every layout's elements */
static void file_open(rd_file_t *fl, const esl_layout_bin_t *file)
{
    uint16_t pos = ESL_LBIN_HDR_LEN;
    uint8_t k;

    fl->bin = file ? file->bin : 0;
    fl->size = file ? file->size : 0;
    fl->text_pos = 0;
    fl->icon_pos = 0;
    if (!file || file->size < ESL_LBIN_HDR_LEN)
        return;
    for (k = file->bin[2]; k; k--) {
        if (pos >= file->size)
            return;
        pos = (uint16_t)(pos + 1 + (uint16_t)file->bin[pos] * ESL_LE_SIZE);
    }
    fl->text_pos = pos;
    for (k = file->bin[ESL_LBIN_OFF_NTEXT]; k; k--) {
        if (pos >= file->size)
            return;
        pos = (uint16_t)(pos + 1 + file->bin[pos]);
    }
    fl->icon_pos = pos;
}

/* Icon i of the file: its size, and its columns (NULL if the file has no such icon) */
static const uint8_t *file_icon(const rd_file_t *fl, uint8_t i, uint16_t *w, uint8_t *h)
{
    uint16_t pos = fl->icon_pos;

    if (!pos || i >= fl->bin[ESL_LBIN_OFF_NICON])
        return 0;
    for (;;) {
        if (pos + ESL_LBIN_ICON_HDR > fl->size)
            return 0;
        *w = ESL_GET_U16(fl->bin + pos);
        *h = fl->bin[pos + 2];
        pos += ESL_LBIN_ICON_HDR;
        if (!i--)
            break;
        pos = (uint16_t)(pos + (uint16_t)(*w * ((*h + 7) / 8)));
    }
    if (pos + (uint32_t)*w * ((*h + 7) / 8) > fl->size)
        return 0;
    return fl->bin + pos;
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

/* The icon whose name (fixed text names + i, ESL_LE_OFF_NAMES) is t, or -1 */
static int icon_by_name(const rd_file_t *fl, uint8_t names, const uint8_t *t, uint8_t len)
{
    if (!len || !fl->icon_pos)
        return -1;
    for (uint8_t i = 0; i < fl->bin[ESL_LBIN_OFF_NICON] && names + i < fl->bin[ESL_LBIN_OFF_NTEXT]; i++) {
        uint8_t n;
        const uint8_t *name = fixed_text(fl, (uint8_t)(names + i), &n);
        if (n == len && memcmp(name, t, len) == 0)
            return i;
    }
    return -1;
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
    e->rot = 0;
    if (e->kind == ESL_LE_TEXT) {
        e->rot = (uint8_t)(e->lines >> ESL_LE_ROT_SHIFT);
        e->lines &= ESL_LE_LINES_MASK;
    }
    e->color = p[ESL_LE_OFF_COLOR];
    e->x = ESL_GET_U16(p + ESL_LE_OFF_X);
    e->y = p[ESL_LE_OFF_Y];
    e->w = ESL_GET_U16(p + ESL_LE_OFF_W);
    e->h = p[ESL_LE_OFF_H];
}

/* ---- the built-in layouts, one set per screen size ---- */

#define RD_TEXT_LINES 4
#define RD_TOP_Y      4

typedef struct {
    rd_elem_t e[RD_TEXT_LINES + 4 + 1];
} rd_builtin_t;

static rd_elem_t T(uint8_t field, uint8_t f0, uint8_t f1, uint8_t f2, uint8_t opts, uint8_t lines, uint8_t color,
                   uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    rd_elem_t e = { ESL_LE_TEXT, field, { f0, f1, f2 }, opts, lines, color, x, y, w, h, 0 };
    return e;
}

static rd_elem_t R(uint8_t color, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    rd_elem_t e = { ESL_LE_RECT, 0, { ESL_FONT_NONE, ESL_FONT_NONE, ESL_FONT_NONE }, 0, 0, color, x, y, w, h, 0 };
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
        if (color == COLOR_INVERT)
            color = e->kind == ESL_LE_TEXT ? color : RD_BLACK;
        else if (color == COLOR_RED_INVERT)
            color = e->kind == ESL_LE_TEXT ? color : RD_RED;
        if (color != COLOR_NONE && e->w && e->h) {
            if (e->kind == ESL_LE_RECT) {
                if (add_rect(s, e->x, e->y, (uint16_t)(e->x + e->w - 1), (uint16_t)(e->y + e->h - 1), color)) {
                    s->rect[s->nrects - 1].radius = e->lines; /* ESL_LE_OFF_RADIUS */
                    s->rect[s->nrects - 1].border = e->opts;  /* ESL_LE_OFF_BORDER */
                }
            } else if (e->kind == ESL_LE_LINE) {
                int16_t x0 = (int16_t)e->x, x1 = (int16_t)(e->x + e->w - 1);
                int16_t y0 = (int16_t)e->y, y1 = (int16_t)(e->y + e->h - 1);
                if (e->opts & ESL_LE_RISING)
                    add_line(s, x0, y1, x1, y0, e->lines ? e->lines : 1, color);
                else
                    add_line(s, x0, y0, x1, y1, e->lines ? e->lines : 1, color);
            } else if (e->kind == ESL_LE_ELLIPSE) {
                add_shape(s, ESL_LE_ELLIPSE, e->x, e->y, e->w, e->h, e->opts /* ESL_LE_OFF_BORDER */, color);
            } else if (e->kind == ESL_LE_POLYGON) {
                uint8_t len;
                const uint8_t *p = fixed_text(fl, e->field, &len);
                uint8_t n = (uint8_t)(len / ESL_POLY_PT_LEN);
                rd_shape_t *sh = n >= ESL_POLY_MIN && n <= ESL_POLY_MAX
                                     ? add_shape(s, ESL_LE_POLYGON, e->x, e->y, e->w, e->h, e->opts, color)
                                     : 0;
                for (uint8_t i = 0; sh && i < n; i++, p += ESL_POLY_PT_LEN) {
                    sh->px[i] = (int16_t)(e->x + ESL_GET_U16(p));
                    sh->py[i] = (int16_t)(e->y + p[2]);
                    /* a corner outside the box (only in a damaged file) draws nothing */
                    if (ESL_GET_U16(p) >= e->w || p[2] >= e->h) {
                        s->nshapes--;
                        sh = 0;
                    }
                }
                if (sh)
                    sh->n = n;
            } else if (e->kind == ESL_LE_CHART) {
                if (e->field < nfields && lens[e->field])
                    add_chart(s, e->x, e->y, e->w, e->h, e->lines /* ESL_LE_OFF_WIDTH */, e->opts, color,
                              text[e->field], lens[e->field]);
            } else if (e->kind == ESL_LE_ICON && (e->field & ESL_LE_ICON_FIELD)) {
                uint8_t f = (uint8_t)(e->field & ~ESL_LE_ICON_FIELD);
                int i = f < nfields ? icon_by_name(fl, e->font[0], text[f], lens[f]) : -1;
                uint16_t iw;
                uint8_t ih;
                const uint8_t *d = i < 0 ? 0 : file_icon(fl, (uint8_t)i, &iw, &ih);
                if (d && iw <= e->w && ih <= e->h)
                    add_icon(s, d, (uint16_t)(e->x + (e->w - iw) / 2), (uint16_t)(e->y + (e->h - ih) / 2), iw, ih,
                             color);
            } else if (e->kind == ESL_LE_ICON) {
                uint16_t iw;
                uint8_t ih;
                const uint8_t *d = file_icon(fl, e->field, &iw, &ih);
                if (d && iw == e->w && ih == e->h)
                    add_icon(s, d, e->x, e->y, iw, ih, color);
            } else {
                const uint8_t *t = 0;
                uint8_t len = 0;
                if (e->field & ESL_LE_FIELD_FIXED) {
                    t = fixed_text(fl, (uint8_t)(e->field & ~ESL_LE_FIELD_FIXED), &len);
                    if (len && e->kind == ESL_LE_TEXT && s->nclocks < ESL_CLOCK_TEXTS && esl_clock_res(t, len)) {
                        int32_t days;
                        uint8_t *buf = s->clock[s->nclocks++];
                        if (!esl_clock_today(&days))
                            days = -1;
                        len = (uint8_t)esl_clock_fill(t, len, days, buf, ESL_CLOCK_TEXT_MAX);
                        t = buf;
                    }
                } else if (e->field < nfields) {
                    t = text[e->field];
                    len = lens[e->field];
                }
                rd_elem_t one;
                const rd_elem_t *te = e;
                if (len && e->kind == ESL_LE_TEXT && (e->opts & ESL_LE_NUM_MASK) && s->nnums < RD_MAX_NUMS) {
                    size_t n = esl_format_number(t, len, e->opts, s->num[s->nnums], ESL_LE_NUM_MAX_LEN);
                    if (n) {
                        t = s->num[s->nnums++];
                        len = (uint8_t)n;
                        /* a number on one line: its thousands spaces are not places to wrap,
                           so a smaller font is used when it does not fit */
                        one = *e;
                        one.lines = 1;
                        te = &one;
                    }
                }
                if (len && e->kind == ESL_LE_TEXT) {
                    s->num_now = te == &one;
                    place_text(s, te, t, len, color);
                    s->num_now = 0;
                }
                else if (len && e->kind == ESL_LE_BARCODE)
                    place_barcode(s, e, t, len);
                else if (len && e->kind == ESL_LE_QR)
                    place_qr(s, e, t, len);
            }
        }
        if (wire)
            e = 0;
        else
            e++;
    }
}

static rd_scene_t scene; /* about 2 KB; not reentrant, see esl_render.h */

/* Lays out content c in scene; false for an unknown model or a missing named layout */
static bool build(uint8_t model, const esl_layout_bin_t *file, const esl_content_t *c)
{
    static rd_builtin_t bi;
    rd_model_t m;
    rd_file_t fl;
    const uint8_t *text[ESL_MAX_FIELDS];
    uint8_t lens[ESL_MAX_FIELDS];
    const uint8_t *p = c->text;
    size_t i;
    int index = -1;

    if (!model_get(model, &m) || m.col_bytes > 32)
        return false;

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
            return false;
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
    return true;
}

bool esl_render_number_cut(uint8_t model, const esl_layout_bin_t *file, const esl_content_t *c)
{
    return build(model, file, c) && scene.num_cut;
}

size_t esl_render(uint8_t model, const esl_layout_bin_t *file, const esl_content_t *c, uint8_t *out, size_t cap)
{
    uint8_t black[32], red[32];
    size_t size, plane;
    uint16_t x;

    size = esl_render_size(model);
    if (!size || cap < size || !build(model, file, c))
        return 0;

    plane = (size_t)scene.m.w * scene.m.col_bytes;
    draw_begin(&scene);
    for (x = 0; x < scene.m.w; x++) {
        draw_column(&scene, x, black, red);
        memcpy(out + (size_t)x * scene.m.col_bytes, black, scene.m.col_bytes);
        if (scene.m.has_red)
            memcpy(out + plane + (size_t)x * scene.m.col_bytes, red, scene.m.col_bytes);
    }
    return size;
}
