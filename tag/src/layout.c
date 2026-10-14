#include "layout.h"
#include "font.h"

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

void layout_apply(draw_scene_t *s, const layout_elem_t *e, const uint8_t *const *text,
                  const uint8_t *lens, uint8_t nfields, uint8_t flags)
{
    uint8_t color;

    draw_clear(s);
    for (; e->kind != LE_END; e++) {
        color = resolve_color(e->color, flags);
        if (color == COLOR_NONE)
            continue;
        if (e->kind == LE_RECT) {
            if (color == COLOR_INVERT)
                color = DRAW_BLACK;
            if (e->w && e->h)
                draw_add_rect(s, e->x, e->y, e->x + e->w - 1, (uint8_t)(e->y + e->h - 1), color);
        } else if (e->kind == LE_TEXT) {
            if (e->field < nfields && lens[e->field])
                place_text(s, e, text[e->field], lens[e->field], color);
        }
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
    layout_apply(s, layout_builtin(c->layout), fld_text, fld_len, ESL_MAX_FIELDS, c->flags);
}

void layout_build_lines(draw_scene_t *s, const uint8_t *const *lines, const uint8_t *lens, uint8_t n)
{
    layout_apply(s, lay_text, lines, lens, n < TEXT_LINES ? n : TEXT_LINES, 0);
}
