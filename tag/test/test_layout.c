#include <string.h>
#include "test.h"
#include "fake.h"
#include "helpers.h"
#include "layout.h"
#include "font.h"

static draw_scene_t sc;
static uint8_t buf[ESL_MAX_PAYLOAD];
static proto_reply_t r;

#define INNER_W (DRAW_W - 2 * LAYOUT_MARGIN)

static void build(uint8_t layout, uint8_t flags, int n, const char *const *f)
{
    uint8_t len = make_content(buf, 0, 1, layout, flags, n, f);
    CHECK_EQ(proto_parse_reply(buf, len, &r), PROTO_OK);
    layout_build(&sc, &r, buf);
}

/* All text runs must lie within the screen and the margins. */
static void check_inside(void)
{
    int i;
    for (i = 0; i < sc.nruns; i++) {
        const draw_run_t *u = &sc.run[i];
        const font_t *f = font_get(u->font);
        CHECK(u->x >= LAYOUT_MARGIN);
        CHECK(u->x + u->w <= DRAW_W - LAYOUT_MARGIN);
        CHECK(u->y + f->height <= DRAW_H);
    }
}

/* Two text runs must not overlap (rectangle against rectangle). */
static void check_no_overlap(void)
{
    int i, j;
    for (i = 0; i < sc.nruns; i++)
        for (j = i + 1; j < sc.nruns; j++) {
            const draw_run_t *a = &sc.run[i], *b = &sc.run[j];
            int ah = font_get(a->font)->height, bh = font_get(b->font)->height;
            int sep = a->x + a->w <= b->x || b->x + b->w <= a->x ||
                      a->y + ah <= b->y || b->y + bh <= a->y;
            CHECK(sep);
        }
}

static void test_fit(void)
{
    uint8_t len;
    const uint8_t *s = L("Wholegrain rye bread with sunflower seeds and oats, sliced 750 g", &len);
    uint8_t ell, n;
    uint16_t w;

    n = layout_fit(FONT_MEDIUM, (const uint8_t *)"Kort", 4, INNER_W, &ell);
    CHECK_EQ(n, 4);
    CHECK_EQ(ell, 0);

    n = layout_fit(FONT_MEDIUM, s, len, 150, &ell);
    CHECK_EQ(ell, 1);
    CHECK(n > 0 && n < len);
    w = font_text_width(&font_medium, s, n) + font_char_width(&font_medium, FONT_ELLIPSIS);
    CHECK(w <= 150);
    CHECK(s[n - 1] != ' ');
    /* one more character would not have fitted together with '…' */
    CHECK(font_text_width(&font_medium, s, (uint8_t)(n + 1)) +
          font_char_width(&font_medium, FONT_ELLIPSIS) > 150 || s[n] == ' ');
}

static void test_wrap(void)
{
    uint8_t len;
    const uint8_t *s = L("Minced beef 14 % fat, 400 g", &len);
    uint8_t next, l1;

    l1 = layout_wrap(FONT_MEDIUM, (const uint8_t *)"Lettmelk 1 L", 12, INNER_W, &next);
    CHECK_EQ(l1, 12);
    CHECK_EQ(next, 12);

    l1 = layout_wrap(FONT_MEDIUM, s, len, 200, &next);
    CHECK(l1 < len);
    CHECK(font_text_width(&font_medium, s, l1) <= 200);
    CHECK(s[l1] == ' ');          /* broken at a space */
    CHECK(s[l1 - 1] != ' ');
    CHECK(s[next] != ' ');
    CHECK(next > l1);

    /* No spaces: broken inside the word */
    l1 = layout_wrap(FONT_MEDIUM, (const uint8_t *)"Superlangtordutenmellomrom", 26, 100, &next);
    CHECK(l1 > 0 && l1 < 26);
    CHECK_EQ(next, l1);
    CHECK(font_text_width(&font_medium, (const uint8_t *)"Superlangtordutenmellomrom", l1) <= 100);
}

static void test_price_layout(void)
{
    static const char *const f[] = { "Lettmelk 1 L", "24,90", "kr/l 24,90" };
    int i, price = -1;

    build(ESL_LAYOUT_PRICE, ESL_CF_PRICE_RED, 3, f);
    CHECK_EQ(sc.nruns, 3);
    CHECK_EQ(sc.nrects, 0);
    for (i = 0; i < sc.nruns; i++)
        if (sc.run[i].font == FONT_LARGE)
            price = i;
    CHECK(price >= 0);
    if (price >= 0) {
        CHECK_EQ(sc.run[price].color, DRAW_RED);
        CHECK_EQ(sc.run[price].x + sc.run[price].w, DRAW_W - LAYOUT_MARGIN);   /* right-aligned */
    }
    check_inside();
    check_no_overlap();
    fake_render(&sc);
    CHECK(fake_ink_count(2) > 200);
    CHECK(fake_ink_count(1) > 200);
}

static void test_price_black_and_long_name(void)
{
    static const char *const f[] = {
        "Minced beef 14 % fat, extra large family pack, 1.2 kg, freshly ground", "129,90", "per kg 108,25"
    };
    int i;
    build(ESL_LAYOUT_PRICE, 0, 3, f);
    CHECK_EQ(sc.nruns, 4);       /* name on two lines + price + subtitle */
    for (i = 0; i < sc.nruns; i++)
        CHECK_EQ(sc.run[i].color, DRAW_BLACK);
    CHECK(sc.run[1].ellipsis);   /* line 2 of the name is truncated */
    check_inside();
    check_no_overlap();
}

static void test_price_falls_back_to_medium(void)
{
    static const char *const f[] = { "Oranges", "2 for 30", "" };
    build(ESL_LAYOUT_PRICE, 0, 3, f);
    CHECK_EQ(sc.nruns, 2);       /* an empty subtitle gives no text run */
    CHECK_EQ(sc.run[1].font, FONT_MEDIUM);
    check_inside();
    check_no_overlap();
}

static void test_text_layout(void)
{
    static const char *const f[] = { "Welcome", "Open 9-21", "", "Saturday 9-18" };
    int i;
    build(ESL_LAYOUT_TEXT, 0, 4, f);
    CHECK_EQ(sc.nruns, 3);
    for (i = 1; i < sc.nruns; i++)
        CHECK(sc.run[i].y > sc.run[i - 1].y);
    check_inside();
    check_no_overlap();
}

static void test_red_frame(void)
{
    static const char *const f[] = { "Tilbud", "", "", "" };
    build(ESL_LAYOUT_TEXT, ESL_CF_RED_FRAME, 4, f);
    CHECK_EQ(sc.nrects, 4);
    fake_render(&sc);
    CHECK_EQ(fake_fb[0][0], 2);
    CHECK_EQ(fake_fb[DRAW_H - 1][DRAW_W - 1], 2);
    CHECK_EQ(fake_fb[DRAW_H / 2][DRAW_W / 2], 0);
}

static void test_max_length_fields(void)
{
    /* 92 bytes of text (the maximum from the AP) spread over fields, including long words without spaces */
    static char a[40], b[20], c[32];
    const char *f[3] = { a, b, c };
    memset(a, 'W', sizeof(a) - 1);
    memset(b, '8', sizeof(b) - 1);
    memset(c, 'm', sizeof(c) - 1);
    build(ESL_LAYOUT_PRICE, ESL_CF_PRICE_RED | ESL_CF_RED_FRAME, 3, f);
    check_inside();
    check_no_overlap();
    fake_render(&sc);
}

void test_layout(void)
{
    RUN(test_fit);
    RUN(test_wrap);
    RUN(test_price_layout);
    RUN(test_price_black_and_long_name);
    RUN(test_price_falls_back_to_medium);
    RUN(test_text_layout);
    RUN(test_red_frame);
    RUN(test_max_length_fields);
}
