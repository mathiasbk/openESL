#include "test.h"
#include "font.h"

static int column_ink(const font_t *f, uint8_t ch, uint8_t col)
{
    const uint8_t *c = font_column(f, ch, col);
    int i, n = 0;
    for (i = 0; i < f->bytes_per_col; i++)
        n += c[i] != 0;
    return n;
}

static int glyph_ink(const font_t *f, uint8_t ch)
{
    int x, n = 0;
    uint8_t w = font_char_width(f, ch);
    for (x = 0; x < w; x++)
        n += column_ink(f, ch, (uint8_t)x);
    return n;
}

static void test_font_sizes(void)
{
    CHECK(font_small.height <= 17);
    CHECK(font_medium.height <= 24);
    CHECK(font_large.height <= 52);
    CHECK_EQ(font_small.bytes_per_col, (font_small.height + 7) / 8);
    CHECK_EQ(font_large.bytes_per_col, (font_large.height + 7) / 8);
    CHECK(font_get(FONT_SMALL) == &font_small);
    CHECK(font_get(FONT_MEDIUM) == &font_medium);
    CHECK(font_get(FONT_LARGE) == &font_large);
}

static void test_norwegian_letters(void)
{
    /* æ ø å Æ Ø Å in Latin-1 (requirement T3) */
    static const uint8_t nor[] = { 0xE6, 0xF8, 0xE5, 0xC6, 0xD8, 0xC5 };
    int i;
    for (i = 0; i < 6; i++) {
        CHECK(font_map(&font_medium, nor[i]) == nor[i]);
        CHECK(font_map(&font_small, nor[i]) == nor[i]);
        CHECK(glyph_ink(&font_medium, nor[i]) > 0);
    }
    CHECK(font_char_width(&font_medium, FONT_ELLIPSIS) > 0);
    CHECK(glyph_ink(&font_medium, FONT_ELLIPSIS) > 0);
}

static void test_missing_chars(void)
{
    /* 0x80-0x9F are control characters in Latin-1 and do not exist: they become '?' */
    CHECK_EQ(font_map(&font_medium, 0x81), '?');
    CHECK_EQ(font_char_width(&font_medium, 0x81), font_char_width(&font_medium, '?'));
    CHECK_EQ(font_map(&font_medium, 0x05), '?');
    /* The large font only has digits and punctuation, and no '?': not drawn */
    CHECK_EQ(font_map(&font_large, 'A'), 0);
    CHECK_EQ(font_char_width(&font_large, 'A'), 0);
    CHECK(font_has_all(&font_large, (const uint8_t *)"24,90", 5));
    CHECK(font_has_all(&font_large, (const uint8_t *)"1 299,-", 7));
    CHECK(!font_has_all(&font_large, (const uint8_t *)"2 for 30", 8));
}

static void test_widths_and_ink(void)
{
    const uint8_t *s = (const uint8_t *)"Lettmelk";
    int i;
    uint16_t sum = 0;
    for (i = 0; i < 8; i++)
        sum += font_char_width(&font_medium, s[i]);
    CHECK_EQ(font_text_width(&font_medium, s, 8), sum);
    CHECK_EQ(glyph_ink(&font_medium, ' '), 0);
    CHECK(glyph_ink(&font_medium, 'W') > glyph_ink(&font_medium, 'i'));
    CHECK(font_char_width(&font_large, '0') > 20);
}

void test_font(void)
{
    RUN(test_font_sizes);
    RUN(test_norwegian_letters);
    RUN(test_missing_chars);
    RUN(test_widths_and_ink);
}
