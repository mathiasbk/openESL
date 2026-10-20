/* test_status.c - the status screens the tag draws itself (status.c): fitting and
   wrapping text, and the four lines of the start screen. */
#include <string.h>
#include "test.h"
#include "fake.h"
#include "helpers.h"
#include "status.h"
#include "font.h"

static draw_scene_t sc;

#if DRAW_W >= 200
#define MARGIN 6
#else
#define MARGIN 5
#endif
#define INNER_W (DRAW_W - 2 * MARGIN)

/* All text runs must lie within the screen and the margins. */
static void check_inside(void)
{
    int i;
    for (i = 0; i < sc.nruns; i++) {
        const draw_run_t *u = &sc.run[i];
        const font_t *f = font_get(u->font);
        CHECK(u->x >= MARGIN);
        CHECK(u->x + u->w <= DRAW_W - MARGIN);
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

static void build(const char *const *s, int n)
{
    const uint8_t *t[STATUS_LINES];
    uint8_t len[STATUS_LINES];
    int i;
    for (i = 0; i < n; i++)
        t[i] = L(s[i], &len[i]);
    status_build(&sc, t, len, (uint8_t)n);
}

static void test_fit(void)
{
    uint8_t len;
    const uint8_t *s = L("Wholegrain rye bread with sunflower seeds and oats, sliced 750 g", &len);
    uint8_t ell, n;
    uint16_t w;

    n = status_fit(FONT_MEDIUM, (const uint8_t *)"Kort", 4, INNER_W, &ell);
    CHECK_EQ(n, 4);
    CHECK_EQ(ell, 0);

    n = status_fit(FONT_MEDIUM, s, len, 150, &ell);
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

    l1 = status_wrap(FONT_MEDIUM, (const uint8_t *)"Lettmelk 1 L", 12, INNER_W, &next);
    CHECK_EQ(l1, 12);
    CHECK_EQ(next, 12);

    l1 = status_wrap(FONT_MEDIUM, s, len, 150, &next);
    CHECK(l1 < len);
    CHECK(font_text_width(&font_medium, s, l1) <= 150);
    CHECK(s[l1] == ' ');          /* broken at a space */
    CHECK(s[l1 - 1] != ' ');
    CHECK(s[next] != ' ');
    CHECK(next > l1);

    /* A lone unit at the end goes down together with the number before it */
    s = L("Gudbrandsdalsost skivet 130 g", &len);
    l1 = status_wrap(FONT_MEDIUM, s, len, font_text_width(&font_medium, s, (uint8_t)(len - 1)), &next);
    CHECK_EQ(len - next, 5);      /* "130 g" */

    /* No spaces: broken inside the word */
    l1 = status_wrap(FONT_MEDIUM, (const uint8_t *)"Superlangtordutenmellomrom", 26, 100, &next);
    CHECK(l1 > 0 && l1 < 26);
    CHECK_EQ(next, l1);
    CHECK(font_text_width(&font_medium, (const uint8_t *)"Superlangtordutenmellomrom", l1) <= 100);
}

static void test_start_screen(void)
{
    /* The start screen: every line whole (the MAC line must fit without "…"), in the
       medium font where it fits on one line (on the 1.54" the MAC line takes the small) */
    static const char *const s[] = { "OpenESL", "MAC 0000000197E5CB3B", "FW 0.1.0", "Waiting for content" };
    int i;
    build(s, 4);
    CHECK(sc.nruns >= 4);
    CHECK_EQ(sc.run[0].font, FONT_MEDIUM);
    CHECK_EQ(sc.run[sc.nruns - 2].font, FONT_MEDIUM);       /* "FW 0.1.0" */
    for (i = 0; i < sc.nruns; i++) {
        CHECK_EQ(sc.run[i].ellipsis, 0);
        CHECK_EQ(sc.run[i].x, MARGIN);
    }
    for (i = 1; i < sc.nruns; i++)
        CHECK(sc.run[i].y > sc.run[i - 1].y);
    CHECK_EQ(sc.nrects, 0);
    check_inside();
    check_no_overlap();
}

static void test_long_line(void)
{
    /* Too long for the medium font: the small one, wrapped and cut off with "…" if need be */
    static const char *const s[] = { "OpenESL", "", "", "Denne linjen er altfor lang til å få plass på skjermen, "
                                                       "selv med den minste fonten og flere linjer" };
    int i, last = 0;
    build(s, 4);
    CHECK(sc.nruns >= 2);
    for (i = 1; i < sc.nruns; i++) {
        CHECK_EQ(sc.run[i].font, FONT_SMALL);
        last = i;
    }
    CHECK_EQ(sc.run[last].ellipsis, 1);
    CHECK(sc.nruns <= DRAW_MAX_RUNS);
    check_inside();
    check_no_overlap();

    /* empty lines and lines past STATUS_LINES add nothing */
    build(s, 1);
    CHECK_EQ(sc.nruns, 1);
}

void test_status(void)
{
    RUN(test_fit);
    RUN(test_wrap);
    RUN(test_start_screen);
    RUN(test_long_line);
}
