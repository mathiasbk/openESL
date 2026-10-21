#include "font.h"

const font_t __code *font_get(uint8_t id)
{
    switch (id) {
    case FONT_SMALL:  return &font_small;
#ifndef FONT_TEXT_ONLY   /* the tag firmware has only small and medium (tools/build.py) */
    case FONT_LARGE:  return &font_large;
    case FONT_MID:    return &font_mid;
#endif
    default:          return &font_medium;
    }
}

static uint8_t raw_width(const font_t __code *f, uint8_t ch)
{
    if (ch < f->first || ch > f->last)
        return 0;
    return f->widths[ch - f->first];
}

uint8_t font_map(const font_t __code *f, uint8_t ch)
{
    if (raw_width(f, ch))
        return ch;
    if (raw_width(f, '?'))
        return '?';
    return 0;
}

uint8_t font_char_width(const font_t __code *f, uint8_t ch)
{
    ch = font_map(f, ch);
    return ch ? f->widths[ch - f->first] : 0;
}

uint16_t font_text_width(const font_t __code *f, const uint8_t *s, uint8_t len)
{
    uint16_t w = 0;
    uint8_t i;
    for (i = 0; i < len; i++)
        w += font_char_width(f, s[i]);
    return w;
}

bool font_has_all(const font_t __code *f, const uint8_t *s, uint8_t len)
{
    uint8_t i;
    for (i = 0; i < len; i++)
        if (!raw_width(f, s[i]))
            return false;
    return true;
}

const uint8_t __code *font_column(const font_t __code *f, uint8_t ch, uint8_t col)
{
    return f->bitmap + f->offsets[ch - f->first] + (uint16_t)col * f->bytes_per_col;
}
