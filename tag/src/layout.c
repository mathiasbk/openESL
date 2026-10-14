#include "layout.h"
#include "font.h"

#define TOP_Y        4
#define BOTTOM_PAD   3    /* room for the red frame at the bottom */
#define INNER_W      (DRAW_W - 2 * LAYOUT_MARGIN)
#define TEXT_LINES   4

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

static void add_fitted(draw_scene_t *s, uint8_t font, uint8_t color, uint16_t x, uint8_t y,
                       const uint8_t *text, uint8_t len, uint16_t maxw)
{
    uint8_t ell, n;
    if (!len)
        return;
    n = layout_fit(font, text, len, maxw, &ell);
    draw_add_text(s, font, color, x, y, text, n, ell);
}

static void build_price(draw_scene_t *s, const proto_reply_t *c, const uint8_t *buf)
{
    const font_t __code *fs = font_get(FONT_SMALL);
    const font_t __code *fm = font_get(FONT_MEDIUM);
    const font_t __code *fl = font_get(FONT_LARGE);
    uint8_t sub_y = DRAW_H - BOTTOM_PAD - fs->height;
    uint8_t color = (c->flags & ESL_CF_PRICE_RED) ? DRAW_RED : DRAW_BLACK;
    const uint8_t *name = buf + c->field_off[0];
    const uint8_t *price = buf + c->field_off[1];
    uint8_t name_len = c->field_len[0];
    uint8_t price_len = c->field_len[1];
    uint8_t l1, next;
    uint16_t pw;

    /* Name: up to two lines */
    if (name_len) {
        l1 = layout_wrap(FONT_MEDIUM, name, name_len, INNER_W, &next);
        draw_add_text(s, FONT_MEDIUM, DRAW_BLACK, LAYOUT_MARGIN, TOP_Y, name, l1, 0);
        if (next < name_len)
            add_fitted(s, FONT_MEDIUM, DRAW_BLACK, LAYOUT_MARGIN, TOP_Y + fm->height,
                       name + next, name_len - next, INNER_W);
    }

    /* Price: large font if every character exists and it fits, otherwise medium */
    if (price_len) {
        pw = font_text_width(fl, price, price_len);
        if (font_has_all(fl, price, price_len) && pw <= INNER_W) {
            draw_add_text(s, FONT_LARGE, color, DRAW_W - LAYOUT_MARGIN - pw,
                          sub_y - fl->height, price, price_len, 0);
        } else {
            uint8_t ell;
            uint8_t n = layout_fit(FONT_MEDIUM, price, price_len, INNER_W, &ell);
            pw = font_text_width(fm, price, n);
            if (ell)
                pw += font_char_width(fm, FONT_ELLIPSIS);
            draw_add_text(s, FONT_MEDIUM, color, DRAW_W - LAYOUT_MARGIN - pw,
                          sub_y - fm->height - 4, price, n, ell);
        }
    }

    /* Subtitle */
    add_fitted(s, FONT_SMALL, DRAW_BLACK, LAYOUT_MARGIN, sub_y,
               buf + c->field_off[2], c->field_len[2], INNER_W);
}

static void build_text(draw_scene_t *s, const uint8_t *const *lines, const uint8_t *lens, uint8_t n)
{
    const font_t __code *fm = font_get(FONT_MEDIUM);
    uint8_t pitch = (DRAW_H - 2 * TOP_Y) / TEXT_LINES;
    uint8_t i;

    for (i = 0; i < n && i < TEXT_LINES; i++)
        add_fitted(s, FONT_MEDIUM, DRAW_BLACK, LAYOUT_MARGIN,
                   TOP_Y + i * pitch + (pitch - fm->height) / 2, lines[i], lens[i], INNER_W);
}

void layout_build(draw_scene_t *s, const proto_reply_t *c, const uint8_t *buf)
{
    const uint8_t *lines[TEXT_LINES];
    uint8_t lens[TEXT_LINES];
    uint8_t i;

    draw_clear(s);
    if (c->layout == ESL_LAYOUT_PRICE) {
        build_price(s, c, buf);
    } else {
        for (i = 0; i < TEXT_LINES; i++) {
            lines[i] = buf + c->field_off[i];
            lens[i] = c->field_len[i];
        }
        build_text(s, lines, lens, TEXT_LINES);
    }

    if (c->flags & ESL_CF_RED_FRAME) {
        draw_add_rect(s, 0, 0, DRAW_W - 1, LAYOUT_FRAME_W - 1, DRAW_RED);
        draw_add_rect(s, 0, DRAW_H - LAYOUT_FRAME_W, DRAW_W - 1, DRAW_H - 1, DRAW_RED);
        draw_add_rect(s, 0, 0, LAYOUT_FRAME_W - 1, DRAW_H - 1, DRAW_RED);
        draw_add_rect(s, DRAW_W - LAYOUT_FRAME_W, 0, DRAW_W - 1, DRAW_H - 1, DRAW_RED);
    }
}

void layout_build_lines(draw_scene_t *s, const uint8_t *const *lines, const uint8_t *lens, uint8_t n)
{
    draw_clear(s);
    build_text(s, lines, lens, n);
}
