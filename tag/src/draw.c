#include "draw.h"
#include "font.h"

void draw_clear(draw_scene_t *s)
{
    s->nruns = 0;
    s->nrects = 0;
    s->nbars = 0;
    s->last_x = 0;
}

bool draw_add_text(draw_scene_t *s, uint8_t font, uint8_t color, uint16_t x, uint8_t y,
                   const uint8_t *text, uint8_t len, uint8_t ellipsis)
{
    const font_t __code *f = font_get(font);
    draw_run_t *r;

    if (s->nruns >= DRAW_MAX_RUNS)
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
    r->ci = 0;
    r->cx = x;
    return true;
}

bool draw_add_rect(draw_scene_t *s, uint16_t x0, uint8_t y0, uint16_t x1, uint8_t y1, uint8_t color)
{
    draw_rect_t *r;
    if (s->nrects >= DRAW_MAX_RECTS)
        return false;
    r = &s->rect[s->nrects++];
    r->x0 = x0;
    r->x1 = x1;
    r->y0 = y0;
    r->y1 = y1;
    r->color = color;
    return true;
}

void draw_begin(draw_scene_t *s)
{
    uint8_t i;
    for (i = 0; i < s->nruns; i++) {
        s->run[i].ci = 0;
        s->run[i].cx = s->run[i].x;
    }
    s->last_x = 0;
}

/* Sets (black) or clears (white) bits y0..y1 (inclusive) in a column. */
static void fill_span(uint8_t *col, uint8_t y0, uint8_t y1, uint8_t color)
{
    uint8_t y, m;
    for (y = y0; ; y++) {
        m = (uint8_t)(0x80 >> (y & 7));
        if (color == DRAW_WHITE)
            col[y >> 3] &= (uint8_t)~m;
        else
            col[y >> 3] |= m;
        if (y == y1)
            break;
    }
}

/* Puts a font column into the screen column from row y downwards: ORs it in (black)
   or clears its pixels (white). Clips at the bottom. */
static void blit_column(uint8_t *col, const uint8_t __code *src, uint8_t nbytes, uint8_t y,
                        uint8_t color)
{
    uint8_t shift = y & 7;
    uint8_t idx = y >> 3;
    uint8_t b, v, hi, lo;

    for (b = 0; b < nbytes && idx < DRAW_COL_BYTES; b++, idx++) {
        v = src[b];
        if (!v)
            continue;
        hi = (uint8_t)(v >> shift);
        lo = shift ? (uint8_t)(v << (8 - shift)) : 0;
        if (color == DRAW_WHITE) {
            col[idx] &= (uint8_t)~hi;
            if (lo && idx + 1 < DRAW_COL_BYTES)
                col[idx + 1] &= (uint8_t)~lo;
        } else {
            col[idx] |= hi;
            if (lo && idx + 1 < DRAW_COL_BYTES)
                col[idx + 1] |= lo;
        }
    }
}

static uint8_t run_char(const draw_run_t *r, uint8_t i)
{
    return i < r->len ? r->text[i] : FONT_ELLIPSIS;
}

static void draw_run(draw_run_t *r, uint16_t x, uint8_t *col)
{
    const font_t __code *f;
    uint8_t n = r->len + r->ellipsis;
    uint8_t ch, w;

    if (x < r->x || x >= r->x + r->w)
        return;
    f = font_get(r->font);
    /* move the cursor forward to the character that covers x */
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
    blit_column(col, font_column(f, ch, (uint8_t)(x - r->cx)), f->bytes_per_col, r->y, r->color);
}

void draw_column(draw_scene_t *s, uint16_t x, uint8_t *black, uint8_t *red)
{
    uint8_t i;

    if (x < s->last_x)
        draw_begin(s);
    s->last_x = x;

    for (i = 0; i < DRAW_COL_BYTES; i++) {
        black[i] = 0;
        red[i] = 0;
    }

    for (i = 0; i < s->nrects; i++) {
        const draw_rect_t *r = &s->rect[i];
        if (x >= r->x0 && x <= r->x1 && r->y0 <= r->y1 && r->y0 < DRAW_H)
            fill_span(black, r->y0, r->y1 < DRAW_H ? r->y1 : DRAW_H - 1, r->color);
    }

    if (s->nbars && x >= s->bars.x0) {
        const draw_bars_t *b = &s->bars;
        uint16_t m = (x - b->x0) / b->mw;
        if (m < b->nmod && (b->bits[m >> 3] & (0x80 >> (m & 7))))
            fill_span(black, b->y0, b->y1, DRAW_BLACK);
    }

    for (i = 0; i < s->nruns; i++)
        draw_run(&s->run[i], x, black);

#if DRAW_H % 8
    /* text is clipped per byte; clear the bits below the last row */
    black[DRAW_COL_BYTES - 1] &= (uint8_t)(0xFF << (8 - DRAW_H % 8));
#endif
}
