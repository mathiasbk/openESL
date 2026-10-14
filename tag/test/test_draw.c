#include <string.h>
#include "test.h"
#include "fake.h"
#include "draw.h"
#include "font.h"

static draw_scene_t sc;

/* Avgrensende boks rundt alt blekk av en farge. */
static void ink_box(int color, int *x0, int *y0, int *x1, int *y1)
{
    int x, y;
    *x0 = DRAW_W; *y0 = DRAW_H; *x1 = -1; *y1 = -1;
    for (y = 0; y < DRAW_H; y++)
        for (x = 0; x < DRAW_W; x++)
            if (fake_fb[y][x] == color) {
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
                if (y < *y0) *y0 = y;
                if (y > *y1) *y1 = y;
            }
}

static void test_empty(void)
{
    draw_clear(&sc);
    fake_render(&sc);
    CHECK_EQ(fake_ink_count(1), 0);
    CHECK_EQ(fake_ink_count(2), 0);
}

static void test_rect_exact(void)
{
    int x0, y0, x1, y1;
    draw_clear(&sc);
    CHECK(draw_add_rect(&sc, 10, 3, 20, 13, DRAW_BLACK));
    fake_render(&sc);
    CHECK_EQ(fake_ink_count(1), 11 * 11);
    ink_box(1, &x0, &y0, &x1, &y1);
    CHECK_EQ(x0, 10); CHECK_EQ(x1, 20);
    CHECK_EQ(y0, 3);  CHECK_EQ(y1, 13);
}

static void test_red_wins(void)
{
    draw_clear(&sc);
    draw_add_rect(&sc, 0, 0, 99, 99, DRAW_BLACK);
    draw_add_rect(&sc, 50, 50, 149, 127, DRAW_RED);
    fake_render(&sc);
#if MODEL_HAS_RED
    CHECK_EQ(fake_ink_count(2), 100 * 78);
    CHECK_EQ(fake_ink_count(1), 100 * 100 - 50 * 50);
    CHECK_EQ(fake_fb[60][60], 2);
#else
    /* black/white model: red is drawn in black, and the union of both is black */
    CHECK_EQ(fake_ink_count(2), 0);
    CHECK_EQ(fake_ink_count(1), 100 * 100 + 100 * 78 - 50 * 50);
#endif
}

static void test_text_bounds(void)
{
    int x0, y0, x1, y1;
    uint8_t s[2] = { 'H', 0xC5 };   /* "HÅ" in Latin-1 */
    draw_clear(&sc);
    CHECK(draw_add_text(&sc, FONT_MEDIUM, DRAW_BLACK, 30, 17, s, 2, 0));
    CHECK_EQ(sc.run[0].w, font_text_width(&font_medium, s, 2));
    fake_render(&sc);
    ink_box(1, &x0, &y0, &x1, &y1);
    CHECK(fake_ink_count(1) > 50);
    CHECK(x0 >= 30);
    CHECK(x1 < 30 + sc.run[0].w);
    CHECK(y0 >= 17);
    CHECK(y1 < 17 + font_medium.height);
    /* Å has a ring above: ink higher up than the top of H */
    CHECK(y0 < 17 + font_medium.height / 2);
}

static void test_shift_alignment(void)
{
    /* The same text at y and y+1 gives the same image moved one row down (checks bit shifts across byte boundaries) */
    static uint8_t a[DRAW_H][DRAW_W];
    uint8_t s[] = "Ag\xF8j";
    int y, x, same = 1;
    int start;

    for (start = 0; start < 9; start++) {
        draw_clear(&sc);
        draw_add_text(&sc, FONT_SMALL, DRAW_BLACK, 5, (uint8_t)start, s, 4, 0);
        fake_render(&sc);
        memcpy(a, fake_fb, sizeof(a));
        draw_clear(&sc);
        draw_add_text(&sc, FONT_SMALL, DRAW_BLACK, 5, (uint8_t)(start + 1), s, 4, 0);
        fake_render(&sc);
        for (y = 0; y < DRAW_H - 1; y++)
            for (x = 0; x < DRAW_W; x++)
                if (a[y][x] != fake_fb[y + 1][x])
                    same = 0;
    }
    CHECK(same);
}

static void test_clip_bottom_right(void)
{
    uint8_t s[] = "Clip at the bottom and right edge of the screen";
    draw_clear(&sc);
    draw_add_text(&sc, FONT_LARGE, DRAW_RED, DRAW_W - 46, DRAW_H - 18, (const uint8_t *)"88", 2, 0);
    draw_add_text(&sc, FONT_MEDIUM, DRAW_BLACK, DRAW_W - 96, DRAW_H - 8, s, (uint8_t)(sizeof(s) - 1), 0);
    fake_render(&sc);   /* must not write outside the buffers */
    CHECK(fake_ink_count(FAKE_RED) > 0);
}

static void test_order_independent(void)
{
    /* Columns in random order give the same result as left to right */
    uint8_t b1[DRAW_COL_BYTES], r1[DRAW_COL_BYTES];
    static const uint16_t xs[] = { DRAW_W - 96, 7, 50, 51, 52, DRAW_W - 1, 0, 120, 119, 121 };
    uint8_t s[] = "Brunost 500 g";
    unsigned i;
    int ok = 1;

    draw_clear(&sc);
    draw_add_text(&sc, FONT_MEDIUM, DRAW_BLACK, 6, 4, s, (uint8_t)(sizeof(s) - 1), 1);
    draw_add_text(&sc, FONT_LARGE, DRAW_RED, DRAW_W - 146, 56, (const uint8_t *)"59,90", 5, 0);
    fake_render(&sc);
    for (i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        int y;
        draw_column(&sc, xs[i], b1, r1);
        for (y = 0; y < DRAW_H; y++) {
            uint8_t m = (uint8_t)(0x80 >> (y & 7));
            int v = (r1[y >> 3] & m) ? 2 : (b1[y >> 3] & m) ? 1 : 0;
            if (v != fake_fb[y][xs[i]])
                ok = 0;
        }
    }
    CHECK(ok);
}

static void test_scene_limits(void)
{
    int i;
    draw_clear(&sc);
    for (i = 0; i < DRAW_MAX_RUNS; i++)
        CHECK(draw_add_text(&sc, FONT_SMALL, DRAW_BLACK, 0, 0, (const uint8_t *)"x", 1, 0));
    CHECK(!draw_add_text(&sc, FONT_SMALL, DRAW_BLACK, 0, 0, (const uint8_t *)"x", 1, 0));
    for (i = 0; i < DRAW_MAX_RECTS; i++)
        CHECK(draw_add_rect(&sc, 0, 0, 1, 1, DRAW_RED));
    CHECK(!draw_add_rect(&sc, 0, 0, 1, 1, DRAW_RED));
}

void test_draw(void)
{
    RUN(test_empty);
    RUN(test_rect_exact);
    RUN(test_red_wins);
    RUN(test_text_bounds);
    RUN(test_shift_alignment);
    RUN(test_clip_bottom_right);
    RUN(test_order_independent);
    RUN(test_scene_limits);
}
