#include "layout.h"
#include "font.h"
#include "barcode.h"

/* The downloaded layout files use the font numbers and screen sizes from shared/proto.h */
#if FONT_SMALL != ESL_FONT_SMALL || FONT_MEDIUM != ESL_FONT_MEDIUM || FONT_LARGE != ESL_FONT_LARGE || \
    FONT_MID != ESL_FONT_MID || FONT_COUNT != ESL_FONT_COUNT
#error "font numbers in font.h and shared/proto.h differ"
#endif
#if (MODEL_CODE == ESL_MODEL_29 && (DRAW_W != ESL_MODEL_29_W || DRAW_H != ESL_MODEL_29_H)) || \
    (MODEL_CODE == ESL_MODEL_154 && (DRAW_W != ESL_MODEL_154_W || DRAW_H != ESL_MODEL_154_H))
#error "screen size in model.h and shared/proto.h differ"
#endif
#if LAYOUT_INVERT_PAD != ESL_INVERT_PAD || DRAW_MAX_RUNS != ESL_DRAW_MAX_RUNS || \
    DRAW_MAX_RECTS != ESL_DRAW_MAX_RECTS || LAYOUT_MAX_LINES != ESL_LE_MAX_LINES || \
    DRAW_MAX_BARCODES != ESL_DRAW_MAX_BARCODES
#error "the limits the AP checks layout files against (shared/proto.h) differ from the tag's"
#endif

/* ---- Built-in layouts, one set per model ---- */

#define TEXT_LINES   4
#define TOP_Y        4
#define SUB_Y        (DRAW_H - LAYOUT_FRAME_W - 17)          /* subtitle: small font, above the frame */
#define INNER_W      (DRAW_W - 2 * LAYOUT_MARGIN)
#if DRAW_H >= 160
#define NAME_H       75                                     /* 3 lines medium or small */
#else
#define NAME_H       51                                     /* 2 lines medium, 3 lines small */
#endif
#define PRICE_Y      (TOP_Y + NAME_H + 1)
#define TEXT_PITCH   ((DRAW_H - 2 * TOP_Y) / TEXT_LINES)

#define T(field, f0, f1, f2, opts, lines, color, x, y, w, h) \
    { LE_TEXT, field, { f0, f1, f2 }, opts, lines, color, x, y, w, h }
#define R(color, x, y, w, h) \
    { LE_RECT, 0, { LE_NO_FONT, LE_NO_FONT, LE_NO_FONT }, 0, 0, color, x, y, w, h }
#define END \
    { LE_END, 0, { LE_NO_FONT, LE_NO_FONT, LE_NO_FONT }, 0, 0, 0, 0, 0, 0, 0 }
#define NO LE_NO_FONT

#define FRAME \
    R(LE_FRAME, 0, 0, DRAW_W, LAYOUT_FRAME_W), \
    R(LE_FRAME, 0, DRAW_H - LAYOUT_FRAME_W, DRAW_W, LAYOUT_FRAME_W), \
    R(LE_FRAME, 0, 0, LAYOUT_FRAME_W, DRAW_H), \
    R(LE_FRAME, DRAW_W - LAYOUT_FRAME_W, 0, LAYOUT_FRAME_W, DRAW_H)

/*
 * The same element list fits both models; only the box sizes follow from model.h.
 * The text lines use lines = 0: one line per field on the 2.9" (30 px boxes), two on
 * the 200 x 200 (48 px boxes).
 */
static const layout_elem_t __code lay_price[] = {
    T(0, FONT_MEDIUM, FONT_SMALL, NO, LE_LEFT | LE_TOP, 0, LE_BLACK,
      LAYOUT_MARGIN, TOP_Y, INNER_W, NAME_H),
    T(1, FONT_LARGE, FONT_MID, FONT_MEDIUM, LE_RIGHT | LE_BOTTOM, 1, LE_PRICE_HL,
      LAYOUT_MARGIN, PRICE_Y, INNER_W, SUB_Y - PRICE_Y),
    T(2, FONT_SMALL, NO, NO, LE_LEFT | LE_TOP, 1, LE_BLACK,
      LAYOUT_MARGIN, SUB_Y, INNER_W, 17),
    FRAME,
    END
};

static const layout_elem_t __code lay_text[] = {
    T(0, FONT_MEDIUM, FONT_SMALL, NO, LE_LEFT | LE_MIDDLE, 0, LE_BLACK,
      LAYOUT_MARGIN, TOP_Y, INNER_W, TEXT_PITCH),
    T(1, FONT_MEDIUM, FONT_SMALL, NO, LE_LEFT | LE_MIDDLE, 0, LE_BLACK,
      LAYOUT_MARGIN, TOP_Y + TEXT_PITCH, INNER_W, TEXT_PITCH),
    T(2, FONT_MEDIUM, FONT_SMALL, NO, LE_LEFT | LE_MIDDLE, 0, LE_BLACK,
      LAYOUT_MARGIN, TOP_Y + 2 * TEXT_PITCH, INNER_W, TEXT_PITCH),
    T(3, FONT_MEDIUM, FONT_SMALL, NO, LE_LEFT | LE_MIDDLE, 0, LE_BLACK,
      LAYOUT_MARGIN, TOP_Y + 3 * TEXT_PITCH, INNER_W, TEXT_PITCH),
    FRAME,
    END
};

const layout_elem_t __code *layout_builtin(uint8_t id)
{
    return id == ESL_LAYOUT_PRICE ? lay_price : lay_text;
}

/* ---- Fitting text ---- */

uint8_t layout_fit(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *ellipsis)
{
    const font_t __code *f = font_get(font);
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

uint8_t layout_wrap(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *next)
{
    const font_t __code *f = font_get(font);
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
    /* Last space that fits; the space itself does not need room. */
    for (k = n; k > 0; k--) {
        if (s[k] == ' ')
            break;
    }
    /* A short unit like "g" or "kg" left alone at the end goes down with the word before it */
    if (k > 0 && len - k <= 3) {
        for (end = k - 1; end > 0 && s[end] != ' '; end--)
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
    /* No space: break inside the word. */
    if (n == 0)
        n = 1;
    *next = n;
    return n;
}

/*
 * The lines of the text element being placed. Kept in XRAM, not on the stack:
 * the 8051 stack is in the small internal RAM.
 */
static uint8_t __xdata ln_start[LAYOUT_MAX_LINES];
static uint8_t __xdata ln_len[LAYOUT_MAX_LINES];
static uint8_t __xdata ln_count;
static uint8_t __xdata ln_ell;      /* 1 = the last line ends with "…" */

#define SPLIT_TRUNC   0x01
#define SPLIT_BROKEN  0x02

/* Splits text onto at most n lines of width w. Returns SPLIT_* flags (0 = fits cleanly). */
static uint8_t split(uint8_t font, const uint8_t *t, uint8_t len, uint16_t w, uint8_t n)
{
    uint8_t pos = 0, l, next, bad = 0;

    ln_count = 0;
    ln_ell = 0;
    while (pos < len && ln_count < n) {
        if (ln_count + 1 < n) {
            l = layout_wrap(font, t + pos, len - pos, w, &next);
            if (l == next && next < len - pos)
                bad |= SPLIT_BROKEN;
        } else {
            l = layout_fit(font, t + pos, len - pos, w, &next);
            ln_ell = next;
            if (ln_ell)
                bad |= SPLIT_TRUNC;
            next = len - pos;
        }
        ln_start[ln_count] = pos;
        ln_len[ln_count] = l;
        ln_count++;
        pos += next;
    }
    return bad;
}

/* How many lines of this font the element allows. */
static uint8_t max_lines(const layout_elem_t *e, const font_t __code *f)
{
    uint8_t n = e->h / f->height;
    if (e->lines && e->lines < n)
        n = e->lines;
    if (n == 0)
        n = 1;
    if (n > LAYOUT_MAX_LINES)
        n = LAYOUT_MAX_LINES;
    return n;
}

/* Rows below the baseline in the deepest font of the list (for LE_BOTTOM). */
static uint8_t max_descent(const layout_elem_t *e)
{
    const font_t __code *f;
    uint8_t i, d = 0;
    for (i = 0; i < LAYOUT_MAX_FONTS && e->font[i] != LE_NO_FONT; i++) {
        f = font_get(e->font[i]);
        if (f->height - f->baseline > d)
            d = f->height - f->baseline;
    }
    return d;
}

#define COLOR_INVERT  2     /* internal: white text in a black box */
#define COLOR_NONE    0xFF  /* do not draw */

/* DRAW_BLACK, DRAW_WHITE, COLOR_INVERT or COLOR_NONE. */
static uint8_t resolve_color(uint8_t c, uint8_t flags)
{
    switch (c) {
    case LE_WHITE:    return DRAW_WHITE;
    case LE_INVERT:   return COLOR_INVERT;
    case LE_PRICE_HL: return (flags & ESL_CF_PRICE_HIGHLIGHT) ? COLOR_INVERT : DRAW_BLACK;
    case LE_FRAME:    return (flags & ESL_CF_FRAME) ? DRAW_BLACK : COLOR_NONE;
    default:          return DRAW_BLACK;
    }
}

static void place_text(draw_scene_t *s, const layout_elem_t *e, const uint8_t *t, uint8_t len,
                       uint8_t color)
{
    const font_t __code *f;
    uint8_t i, font = FONT_MEDIUM, found = 0, ell, pad = 0, n;
    int16_t y, y0 = -1, y1 = 0;
    uint16_t lw, x, w, bx0 = 0xFFFF, bx1 = 0;
    uint8_t ey, eh;

    /* An inverted field keeps LAYOUT_INVERT_PAD of black around the text inside the box */
    if (color == COLOR_INVERT) {
        pad = LAYOUT_INVERT_PAD;
        if (e->w <= 2 * pad || e->h <= 2 * pad)
            return;
        color = DRAW_WHITE;
    }
    x = e->x + pad;
    w = e->w - 2 * pad;
    ey = e->y + pad;
    eh = e->h - 2 * pad;

    for (i = 0; i < LAYOUT_MAX_FONTS && e->font[i] != LE_NO_FONT; i++) {
        font = e->font[i];
        f = font_get(font);
        n = max_lines(e, f);
        if (pad && n * f->height > eh)          /* the box must hold the whole font cell */
            n = eh / f->height;
        if (n && font_has_all(f, t, len) && !split(font, t, len, w, n)) {
            found = 1;
            break;
        }
    }
    f = font_get(font);
    if (!found)     /* none fitted: the last font, wrapped and truncated */
        split(font, t, len, w, max_lines(e, f));

    switch (e->opts & LE_VMASK) {
    case LE_MIDDLE:
        y = (int16_t)ey + ((int16_t)eh - (int16_t)ln_count * f->height) / 2;
        break;
    case LE_BOTTOM:
        y = (int16_t)ey + eh - max_descent(e) - f->baseline
            - (int16_t)(ln_count - 1) * f->height;
        break;
    default:
        y = ey;
        break;
    }
    if (y < ey)
        y = ey;

    for (i = 0; i < ln_count; i++, y += f->height) {
        uint16_t lx;
        ell = (i + 1 == ln_count) ? ln_ell : 0;
        if (!ln_len[i] && !ell)
            continue;
        lw = font_text_width(f, t + ln_start[i], ln_len[i]);
        if (ell)
            lw += font_char_width(f, FONT_ELLIPSIS);
        lx = x;
        if (lw < w) {
            if ((e->opts & LE_HMASK) == LE_RIGHT)
                lx += w - lw;
            else if ((e->opts & LE_HMASK) == LE_CENTER)
                lx += (w - lw) / 2;
        }
        if (y + f->height > DRAW_H)
            break;
        if (pad) {
            if (!draw_add_text(s, font, color, lx, (uint8_t)y, t + ln_start[i], ln_len[i], ell))
                break;
            if (y0 < 0)
                y0 = y;
            y1 = y + f->height - 1;
            if (lx < bx0)
                bx0 = lx;
            if (lx + lw > bx1)
                bx1 = lx + lw;
            continue;
        }
        draw_add_text(s, font, color, lx, (uint8_t)y, t + ln_start[i], ln_len[i], ell);
    }

    /* The black box behind the white text: the font cells plus the padding. draw_column
       fills every rectangle before it draws any text, so adding it last is fine. */
    if (pad && y0 >= 0)
        draw_add_rect(s, bx0 - pad, (uint8_t)(y0 - pad), bx1 - 1 + pad, (uint8_t)(y1 + pad),
                      DRAW_BLACK);
}

/* A barcode element: the field encoded with symbology font[0], module width lines (0 = the
   widest that fits, at most ESL_BC_MODULE_MAX), quiet zones inside the box. Draws nothing if
   the text cannot be encoded or does not fit. */
static void place_barcode(draw_scene_t *s, const layout_elem_t *e, const uint8_t *t, uint8_t len)
{
    draw_bars_t *b = &s->bars;
    uint8_t sym = e->font[0], mw = e->lines;
    uint16_t n, total, used;

    if (s->nbars >= DRAW_MAX_BARCODES || !e->h)
        return;
    n = barcode_encode(sym, t, len, b->bits, DRAW_W);
    if (!n)
        return;
    total = n + barcode_quiet_left(sym) + barcode_quiet_right(sym);
    if (!mw) {
        mw = (uint8_t)(e->w / total > ESL_BC_MODULE_MAX ? ESL_BC_MODULE_MAX : e->w / total);
    }
    if (!mw || mw > ESL_BC_MODULE_MAX || total * mw > e->w)
        return;
    used = total * mw;
    b->x0 = e->x + barcode_quiet_left(sym) * mw;
    if ((e->opts & LE_HMASK) == LE_RIGHT)
        b->x0 += e->w - used;
    else if ((e->opts & LE_HMASK) == LE_CENTER)
        b->x0 += (e->w - used) / 2;
    b->nmod = n;
    b->mw = mw;
    b->y0 = e->y;
    b->y1 = (uint8_t)(e->y + e->h - 1);
    s->nbars = 1;
}

/* The downloaded file, and one element decoded from it (layout_apply_bin). In XRAM: the
   8051 stack is in the small internal RAM. */
uint8_t __xdata layout_file[ESL_LBIN_MAX];
uint16_t __xdata layout_file_len;
uint32_t __xdata layout_file_ver;
static layout_elem_t __xdata wire_elem;

static void decode(const uint8_t *p)
{
    wire_elem.kind = p[ESL_LE_OFF_KIND];
    wire_elem.field = p[ESL_LE_OFF_FIELD];
    wire_elem.font[0] = p[ESL_LE_OFF_FONT];
    wire_elem.font[1] = p[ESL_LE_OFF_FONT + 1];
    wire_elem.font[2] = p[ESL_LE_OFF_FONT + 2];
    wire_elem.opts = p[ESL_LE_OFF_OPTS];
    wire_elem.lines = p[ESL_LE_OFF_LINES];
    wire_elem.color = p[ESL_LE_OFF_COLOR];
    wire_elem.x = ESL_GET_U16(p + ESL_LE_OFF_X);
    wire_elem.y = p[ESL_LE_OFF_Y];
    wire_elem.w = ESL_GET_U16(p + ESL_LE_OFF_W);
    wire_elem.h = p[ESL_LE_OFF_H];
}

/*
 * Adds the elements of one layout: the struct list e (built-in, up to LE_END), or, when
 * e is 0, n elements in the wire format at wire. One function for both, so drawing a
 * downloaded layout is no deeper on the stack than drawing a built-in one.
 */
static void apply_list(draw_scene_t *s, const layout_elem_t *e, const uint8_t *wire, uint8_t n,
                       const uint8_t *const *text, const uint8_t *lens, uint8_t nfields, uint8_t flags)
{
    uint8_t color;

    draw_clear(s);
    for (;;) {
        if (!e) {
            if (!n--)
                break;
            decode(wire);
            wire += ESL_LE_SIZE;
            e = &wire_elem;
        } else if (e->kind == LE_END) {
            break;
        }
        color = resolve_color(e->color, flags);
        if (color != COLOR_NONE) {
            if (e->kind == LE_RECT) {
                if (color == COLOR_INVERT)
                    color = DRAW_BLACK;
                if (e->w && e->h)
                    draw_add_rect(s, e->x, e->y, e->x + e->w - 1, (uint8_t)(e->y + e->h - 1), color);
            } else if (e->kind == LE_TEXT) {
                if (e->field < nfields && lens[e->field])
                    place_text(s, e, text[e->field], lens[e->field], color);
            } else if (e->kind == LE_BARCODE) {
                if (e->field < nfields && lens[e->field])
                    place_barcode(s, e, text[e->field], lens[e->field]);
            }
        }
        if (wire)
            e = 0;
        else
            e++;
    }
}

void layout_apply(draw_scene_t *s, const layout_elem_t *e, const uint8_t *const *text,
                  const uint8_t *lens, uint8_t nfields, uint8_t flags)
{
    apply_list(s, e, 0, 0, text, lens, nfields, flags);
}

void layout_apply_bin(draw_scene_t *s, const uint8_t *wire, uint8_t nelem, const uint8_t *const *text,
                      const uint8_t *lens, uint8_t nfields, uint8_t flags)
{
    apply_list(s, 0, wire, nelem, text, lens, nfields, flags);
}

/* ---- Downloaded layout file ---- */

/* The checks the AP also makes, so a bad file can never index past a table or the screen. */
static bool elem_ok(void)
{
    uint8_t i;
    const layout_elem_t __xdata *e = &wire_elem;

    if (e->kind != LE_TEXT && e->kind != LE_RECT && e->kind != LE_BARCODE)
        return false;
    if (e->color > LE_FRAME || (uint32_t)e->x + e->w > DRAW_W || (uint16_t)e->y + e->h > DRAW_H)
        return false;
    if (e->kind == LE_RECT)
        return true;
    if (e->kind == LE_BARCODE)
        return e->field < ESL_MAX_FIELDS && e->font[0] < ESL_BC_COUNT && e->lines <= ESL_BC_MODULE_MAX &&
               e->color == LE_BLACK;
    if (e->field >= ESL_MAX_FIELDS || e->font[0] >= FONT_COUNT || e->lines > LAYOUT_MAX_LINES)
        return false;
    for (i = 1; i < LAYOUT_MAX_FONTS; i++)
        if (e->font[i] >= FONT_COUNT && e->font[i] != LE_NO_FONT)
            return false;
    return true;
}

static bool file_ok(uint16_t len)
{
    uint16_t pos = ESL_LBIN_HDR_LEN;
    uint8_t n, k;

    if (len < ESL_LBIN_HDR_LEN || layout_file[0] != ESL_LBIN_FORMAT || layout_file[1] != MODEL_CODE ||
        layout_file[2] == 0 || layout_file[2] > ESL_LAYOUT_FILE_MAX)
        return false;
    for (k = layout_file[2]; k; k--) {
        if (pos >= len)
            return false;
        n = layout_file[pos++];
        if ((uint16_t)n * ESL_LE_SIZE > len - pos)
            return false;
        for (; n; n--, pos += ESL_LE_SIZE) {
            decode(layout_file + pos);
            if (!elem_ok())
                return false;
        }
    }
    return pos == len;
}

bool layout_file_install(uint16_t len, uint32_t ver)
{
    uint32_t crc;

    layout_file_ver = 0;
    if (len > ESL_LBIN_MAX || ver == 0)
        return false;
    crc = proto_crc32(0, layout_file, len);
    if ((crc ? crc : 1) != ver || !file_ok(len)) {
        layout_file_len = 0;
        return false;
    }
    layout_file_len = len;
    layout_file_ver = ver;
    return true;
}

bool layout_file_restore(void)
{
    uint16_t pos = ESL_LBIN_HDR_LEN;
    uint32_t crc;
    uint8_t k;

    layout_file_clear();
    if (layout_file[0] != ESL_LBIN_FORMAT || layout_file[2] > ESL_LAYOUT_FILE_MAX)
        return false;               /* also erased flash (0xFF) */
    for (k = layout_file[2]; k; k--) {
        if (pos >= ESL_LBIN_MAX)
            return false;
        pos += 1 + (uint16_t)layout_file[pos] * ESL_LE_SIZE;
    }
    if (pos > ESL_LBIN_MAX)
        return false;
    crc = proto_crc32(0, layout_file, pos);
    return layout_file_install(pos, crc ? crc : 1);
}

void layout_file_clear(void)
{
    layout_file_ver = 0;
    layout_file_len = 0;
}

const uint8_t *layout_file_find(uint8_t n, uint8_t *nelem)
{
    uint16_t pos = ESL_LBIN_HDR_LEN;

    if (!layout_file_ver || n >= layout_file[2])
        return 0;
    for (;;) {
        *nelem = layout_file[pos++];
        if (!n--)
            return layout_file + pos;
        pos += (uint16_t)*nelem * ESL_LE_SIZE;
    }
}

/* ---- Entry points ---- */

static const uint8_t *__xdata fld_text[ESL_MAX_FIELDS];
static uint8_t __xdata fld_len[ESL_MAX_FIELDS];

void layout_build(draw_scene_t *s, const proto_reply_t *c, const uint8_t *buf)
{
    uint8_t i;

    for (i = 0; i < ESL_MAX_FIELDS; i++) {
        fld_text[i] = buf + c->field_off[i];
        fld_len[i] = c->field_len[i];
    }
    if (c->layout >= ESL_LAYOUT_FILE) {
        uint8_t n;
        const uint8_t *e = layout_file_find(c->layout - ESL_LAYOUT_FILE, &n);
        if (e) {
            apply_list(s, 0, e, n, fld_text, fld_len, ESL_MAX_FIELDS, c->flags);
            return;
        }
    }
    apply_list(s, layout_builtin(c->layout), 0, 0, fld_text, fld_len, ESL_MAX_FIELDS, c->flags);
}

void layout_build_lines(draw_scene_t *s, const uint8_t *const *lines, const uint8_t *lens, uint8_t n)
{
    layout_apply(s, lay_text, lines, lens, n < TEXT_LINES ? n : TEXT_LINES, 0);
}
