#include "status.h"
#include "font.h"

#if DRAW_W >= 200
#define MARGIN   6                                   /* left/right margin */
#else
#define MARGIN   5
#endif
#define TOP_Y    4
#define INNER_W  (DRAW_W - 2 * MARGIN)
#define PITCH    ((DRAW_H - 2 * TOP_Y) / STATUS_LINES) /* one box per line: 30 px on the 2.9", 48 on the 1.54" */

/* The scene must hold every line a box can wrap onto */
#if DRAW_MAX_RUNS < STATUS_LINES * STATUS_MAX_WRAP
#error "DRAW_MAX_RUNS is too small for the status screen"
#endif

/* ---- Fitting text ---- */

uint8_t status_fit(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *ellipsis)
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

uint8_t status_wrap(uint8_t font, const uint8_t *s, uint8_t len, uint16_t maxw, uint8_t *next)
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
 * The lines of the text being placed. Kept in XRAM, not on the stack: the 8051 stack is
 * in the small internal RAM.
 */
static uint8_t __xdata ln_start[STATUS_MAX_WRAP];
static uint8_t __xdata ln_len[STATUS_MAX_WRAP];
static uint8_t __xdata ln_count;
static uint8_t __xdata ln_ell;      /* 1 = the last line ends with "…" */

/* Splits text onto at most n lines of width w. Returns true if it fits cleanly (no word
   broken, nothing cut off). */
static bool split(uint8_t font, const uint8_t *t, uint8_t len, uint16_t w, uint8_t n)
{
    uint8_t pos = 0, l, next;
    bool clean = true;

    ln_count = 0;
    ln_ell = 0;
    while (pos < len && ln_count < n) {
        if (ln_count + 1 < n) {
            l = status_wrap(font, t + pos, len - pos, w, &next);
            if (l == next && next < len - pos)
                clean = false;
        } else {
            l = status_fit(font, t + pos, len - pos, w, &next);
            ln_ell = next;
            if (ln_ell)
                clean = false;
            next = len - pos;
        }
        ln_start[ln_count] = pos;
        ln_len[ln_count] = l;
        ln_count++;
        pos += next;
    }
    return clean;
}

/* How many lines of a font one box holds. */
static uint8_t box_lines(const font_t __code *f)
{
    uint8_t n = PITCH / f->height;
    if (n == 0)
        n = 1;
    if (n > STATUS_MAX_WRAP)
        n = STATUS_MAX_WRAP;
    return n;
}

/* One line of status text in box i, left-aligned and centred vertically. */
static void place(draw_scene_t *s, uint8_t box, const uint8_t *t, uint8_t len)
{
    const font_t __code *f = font_get(FONT_MEDIUM);
    uint8_t i, font = FONT_MEDIUM, ell;
    int16_t y, ey = TOP_Y + box * PITCH;

    if (!font_has_all(f, t, len) || !split(FONT_MEDIUM, t, len, INNER_W, box_lines(f))) {
        font = FONT_SMALL;       /* the medium font does not fit: the small one, wrapped and cut if need be */
        f = font_get(FONT_SMALL);
        split(FONT_SMALL, t, len, INNER_W, box_lines(f));
    }
    y = ey + ((int16_t)PITCH - (int16_t)ln_count * f->height) / 2;
    if (y < ey)
        y = ey;
    for (i = 0; i < ln_count; i++, y += f->height) {
        ell = (i + 1 == ln_count) ? ln_ell : 0;
        if (!ln_len[i] && !ell)
            continue;
        if (y + f->height > DRAW_H)
            break;
        draw_add_text(s, font, DRAW_BLACK, MARGIN, (uint8_t)y, t + ln_start[i], ln_len[i], ell);
    }
}

void status_build(draw_scene_t *s, const uint8_t *const *lines, const uint8_t *lens, uint8_t n)
{
    uint8_t i;

    draw_clear(s);
    for (i = 0; i < n && i < STATUS_LINES; i++)
        if (lens[i])
            place(s, i, lines[i], lens[i]);
}

/* ---- The "no AP" mark (#98) ---- */

#define MARK_PAD 2                   /* white around the symbol */

/* The symbol, one u16 per column, bit 15 = its top row: a cross, then four bars */
static const uint16_t __code mark[STATUS_MARK_W - 2 * MARK_PAD] = {
    0xC603, 0x6C03, 0x3803, 0x6C00, 0xC61F, 0x001F, 0x001F, 0x0000,
    0x00FF, 0x00FF, 0x00FF, 0x0000, 0x07FF, 0x07FF, 0x07FF,
};

#if STATUS_MARK_H - 2 * MARK_PAD != 16 || STATUS_MARK_H > DRAW_H || STATUS_MARK_W > DRAW_W
#error "the no-AP mark does not fit"
#endif

void status_mark_column(uint16_t x, uint8_t *black, uint8_t *red)
{
    uint16_t c = 0;
    uint8_t y, i, m;

    if (x < DRAW_W - STATUS_MARK_W)
        return;
    x -= DRAW_W - STATUS_MARK_W;
    if (x >= MARK_PAD && x < STATUS_MARK_W - MARK_PAD)
        c = mark[x - MARK_PAD];
    for (y = 0; y < STATUS_MARK_H; y++) {
        i = y >> 3;
        m = (uint8_t)(0x80 >> (y & 7));
        red[i] &= (uint8_t)~m;
        if (y >= MARK_PAD && y < STATUS_MARK_H - MARK_PAD && (c & (0x8000u >> (y - MARK_PAD))))
            black[i] |= m;
        else
            black[i] &= (uint8_t)~m;
    }
}
