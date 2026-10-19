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
        CHECK(u->y + f->height <= DRAW_H - LAYOUT_FRAME_W);
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

/* The text of run i, as a C string (for comparing). */
static const char *run_text(int i)
{
    static char t[128];
    memcpy(t, sc.run[i].text, sc.run[i].len);
    t[sc.run[i].len] = 0;
    return t;
}

static int find_font(uint8_t font)
{
    int i;
    for (i = 0; i < sc.nruns; i++)
        if (sc.run[i].font == font)
            return i;
    return -1;
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

    l1 = layout_wrap(FONT_MEDIUM, s, len, 150, &next);
    CHECK(l1 < len);
    CHECK(font_text_width(&font_medium, s, l1) <= 150);
    CHECK(s[l1] == ' ');          /* broken at a space */
    CHECK(s[l1 - 1] != ' ');
    CHECK(s[next] != ' ');
    CHECK(next > l1);

    /* A lone unit at the end goes down together with the number before it */
    s = L("Gudbrandsdalsost skivet 130 g", &len);
    l1 = layout_wrap(FONT_MEDIUM, s, len, font_text_width(&font_medium, s, (uint8_t)(len - 1)), &next);
    CHECK_EQ(len - next, 5);      /* "130 g" */

    /* No spaces: broken inside the word */
    l1 = layout_wrap(FONT_MEDIUM, (const uint8_t *)"Superlangtordutenmellomrom", 26, 100, &next);
    CHECK(l1 > 0 && l1 < 26);
    CHECK_EQ(next, l1);
    CHECK(font_text_width(&font_medium, (const uint8_t *)"Superlangtordutenmellomrom", l1) <= 100);
}

static void test_price_layout(void)
{
    static const char *const f[] = { "Lettmelk 1 L", "24.90", "kr 24,90/l" };
    int price;

    build(ESL_LAYOUT_PRICE, 0, 3, f);
    CHECK_EQ(sc.nruns, 3);
    CHECK_EQ(sc.nrects, 0);
    price = find_font(FONT_LARGE);
    CHECK(price >= 0);
    if (price >= 0) {
        CHECK_EQ(sc.run[price].color, DRAW_BLACK);
        CHECK_EQ(sc.run[price].x + sc.run[price].w, DRAW_W - LAYOUT_MARGIN);   /* right-aligned */
        CHECK(strcmp(run_text(price), "24.90") == 0);
    }
    check_inside();
    check_no_overlap();
    fake_render(&sc);
    CHECK(fake_ink_count(1) > 200);
}

/* The index of the run with this text, or -1. */
static int find_text(const char *s)
{
    int i;
    for (i = 0; i < sc.nruns; i++)
        if (strcmp(run_text(i), s) == 0)
            return i;
    return -1;
}

static void test_price_highlight(void)
{
    /* Flag bit0: white price in a black box with LAYOUT_INVERT_PAD around the font cell */
    static const char *const f[] = { "Lettmelk 1 L", "24,90", "kr 24,90/l" };
    const draw_rect_t *b;
    const font_t *pf;
    int p, sub, i, x, y, white = 0;

    build(ESL_LAYOUT_PRICE, ESL_CF_PRICE_HIGHLIGHT, 3, f);
    CHECK_EQ(sc.nruns, 3);
    CHECK_EQ(sc.nrects, 1);
    p = find_text("24,90");
    sub = find_text("kr 24,90/l");
    CHECK(p >= 0 && sub >= 0);
    if (p < 0 || sub < 0)
        return;
    pf = font_get(sc.run[p].font);
    b = &sc.rect[0];
    CHECK_EQ(sc.run[p].color, DRAW_WHITE);
    CHECK_EQ(b->color, DRAW_BLACK);
    CHECK(sc.run[p].font == FONT_LARGE || sc.run[p].font == FONT_MID);
    /* the box hugs the text, right-aligned at the margin */
    CHECK_EQ(b->x0, sc.run[p].x - LAYOUT_INVERT_PAD);
    CHECK_EQ(b->x1, sc.run[p].x + sc.run[p].w - 1 + LAYOUT_INVERT_PAD);
    CHECK_EQ(b->x1, DRAW_W - LAYOUT_MARGIN - 1);
    CHECK_EQ(b->y0, sc.run[p].y - LAYOUT_INVERT_PAD);
    CHECK_EQ(b->y1, sc.run[p].y + pf->height - 1 + LAYOUT_INVERT_PAD);
    /* it stays clear of the name and the subtitle */
    for (i = 0; i < sc.nruns; i++)
        if (i != p)
            CHECK(sc.run[i].y + font_get(sc.run[i].font)->height <= b->y0 || sc.run[i].y > b->y1);
    check_inside();
    check_no_overlap();

    /* The digits are white inside the black box */
    fake_render(&sc);
    for (y = b->y0; y <= b->y1; y++)
        for (x = b->x0; x <= b->x1; x++)
            white += fake_fb[y][x] == 0;
    CHECK(white > 100);
    CHECK_EQ(fake_fb[b->y0][b->x0], 1);
    CHECK_EQ(fake_fb[b->y1][b->x1], 1);

    /* The same price without the flag: black text, no box */
    build(ESL_LAYOUT_PRICE, 0, 3, f);
    CHECK_EQ(sc.nrects, 0);
    p = find_text("24,90");
    CHECK(p >= 0 && sc.run[p].color == DRAW_BLACK);
}

static void test_long_name_shrinks(void)
{
    /* Too long for the medium font on the lines it has: the small font is used, not truncated */
    static const char *const f[] = {
        "Minced beef 14 % fat, extra large family pack, freshly ground", "129,90", "per kg 108,25"
    };
    int i, n = 0;
    build(ESL_LAYOUT_PRICE, 0, 3, f);
    for (i = 0; i < sc.nruns; i++) {
        CHECK_EQ(sc.run[i].color, DRAW_BLACK);
        if (sc.run[i].y < 40) {
            CHECK_EQ(sc.run[i].font, FONT_SMALL);
            CHECK_EQ(sc.run[i].ellipsis, 0);
            n += sc.run[i].len;
        }
    }
    CHECK(n >= (int)strlen(f[0]) - 3);   /* everything shown, apart from the spaces at line breaks */
    check_inside();
    check_no_overlap();
}

static void test_too_long_name_truncated(void)
{
    /* 89 bytes of wide letters: does not fit even in the small font; the last line ends with "…" */
    static const char *const f[] = {
        "WWWWWWWW MMMMMMMM WWWWWWWW MMMMMMMM WWWWWWWW MMMMMMMM WWWWWWWW MMMMMMMM WWWWWWWW MMMMMMMM", "", ""
    };
    int i, last = -1;
    build(ESL_LAYOUT_PRICE, 0, 3, f);
    for (i = 0; i < sc.nruns; i++) {
        CHECK_EQ(sc.run[i].font, FONT_SMALL);
        last = i;
    }
    CHECK_EQ(sc.nruns, 3);
    CHECK(last >= 0 && sc.run[last].ellipsis);
    check_inside();
    check_no_overlap();
}

static void test_price_font_chain(void)
{
    /* Large font if it fits, otherwise mid, otherwise medium (also for letters) */
    static const char *const a[] = { "x", "1 299,90", "" };
    static const char *const b[] = { "x", "12 345 678,90", "" };
    static const char *const c[] = { "Oranges", "2 for 30", "" };
    int p;

    build(ESL_LAYOUT_PRICE, 0, 3, a);
    p = sc.nruns - 1;
    CHECK(font_text_width(&font_large, sc.run[p].text, sc.run[p].len) <= INNER_W ?
          sc.run[p].font == FONT_LARGE : sc.run[p].font == FONT_MID);
    CHECK_EQ(sc.run[p].ellipsis, 0);

    build(ESL_LAYOUT_PRICE, 0, 3, b);
    p = sc.nruns - 1;
    CHECK(sc.run[p].font == FONT_MID || sc.run[p].font == FONT_MEDIUM);
    CHECK(sc.run[p].font != FONT_LARGE);
    CHECK_EQ(sc.run[p].len, 13);
    check_inside();

    build(ESL_LAYOUT_PRICE, 0, 3, c);
    CHECK_EQ(sc.nruns, 2);       /* an empty subtitle gives no text run */
    CHECK_EQ(sc.run[1].font, FONT_MEDIUM);
    check_inside();
    check_no_overlap();
}

static void test_price_baseline(void)
{
    /* The large, mid and medium price sit on the same baseline */
    static const char *const a[] = { "x", "24,90", "" };
    static const char *const b[] = { "x", "2 for 30", "" };
    int base_a, base_b;

    build(ESL_LAYOUT_PRICE, 0, 3, a);
    base_a = sc.run[1].y + font_get(sc.run[1].font)->baseline;
    build(ESL_LAYOUT_PRICE, 0, 3, b);
    base_b = sc.run[1].y + font_get(sc.run[1].font)->baseline;
    CHECK(sc.run[1].font != FONT_LARGE);
    CHECK_EQ(base_a, base_b);
}

static void test_norwegian(void)
{
    /* æøå in all fields; the AP sends them as Latin-1 (T3) */
    static const char *const f[] = { "Rømme lett 18 % Tine", "19,90", "Ærlig økologisk – kr 66,33/l" };
    int i, j, found = 0;
    build(ESL_LAYOUT_PRICE, 0, 3, f);
    for (i = 0; i < sc.nruns; i++)
        for (j = 0; j < sc.run[i].len; j++) {
            CHECK(font_map(font_get(sc.run[i].font), sc.run[i].text[j]) == sc.run[i].text[j]);
            if (sc.run[i].text[j] == 0xF8 || sc.run[i].text[j] == 0xC6)
                found++;
        }
    CHECK(found >= 3);
    check_inside();
    check_no_overlap();
}

static void test_text_layout(void)
{
    static const char *const f[] = { "Åpningstider", "Man–fre 9–21", "", "Søndag stengt" };
    int i;
    build(ESL_LAYOUT_TEXT, 0, 4, f);
    CHECK_EQ(sc.nruns, 3);
    for (i = 1; i < sc.nruns; i++)
        CHECK(sc.run[i].y > sc.run[i - 1].y);
    for (i = 0; i < sc.nruns; i++)
        CHECK_EQ(sc.run[i].font, FONT_MEDIUM);
    check_inside();
    check_no_overlap();
}

static void test_text_line_shrinks(void)
{
    static const char *const f[] = { "Kort", "Denne linjen er altfor lang for skjermen", "", "" };
    build(ESL_LAYOUT_TEXT, 0, 4, f);
    CHECK_EQ(sc.run[0].font, FONT_MEDIUM);
    CHECK_EQ(sc.run[1].font, FONT_SMALL);
    check_inside();
    check_no_overlap();
}

static void test_frame(void)
{
    static const char *const f[] = { "Tilbud", "", "", "" };
    build(ESL_LAYOUT_TEXT, ESL_CF_FRAME, 4, f);
    CHECK_EQ(sc.nrects, 4);
    fake_render(&sc);
    CHECK_EQ(fake_fb[0][0], 1);
    CHECK_EQ(fake_fb[DRAW_H - 1][DRAW_W - 1], 1);
    CHECK_EQ(fake_fb[LAYOUT_FRAME_W][LAYOUT_FRAME_W], 0);
    CHECK_EQ(fake_fb[DRAW_H / 2][DRAW_W / 2], 0);
    check_inside();
}

static void test_max_length_fields(void)
{
    /* 92 bytes of text (the maximum from the AP) spread over fields, including long words without spaces */
    static char a[40], b[20], c[32];
    const char *f[3] = { a, b, c };
    memset(a, 'W', sizeof(a) - 1);
    memset(b, '8', sizeof(b) - 1);
    memset(c, 'm', sizeof(c) - 1);
    build(ESL_LAYOUT_PRICE, ESL_CF_PRICE_HIGHLIGHT | ESL_CF_FRAME, 3, f);
    CHECK_EQ(sc.nrects, 5);         /* frame + price box: every rectangle is used */
    check_inside();
    check_no_overlap();
    fake_render(&sc);
}

static void test_custom_elements(void)
{
    /* layout_apply with an element list in RAM (as a layout from the AP would be) */
    static const layout_elem_t e[] = {
        { LE_TEXT, 1, { FONT_MEDIUM, LE_NO_FONT, LE_NO_FONT }, LE_CENTER | LE_MIDDLE, 1, LE_INVERT,
          10, 10, 100, 40 },
        { LE_RECT, 0, { LE_NO_FONT, LE_NO_FONT, LE_NO_FONT }, 0, 0, LE_BLACK, 0, 60, 20, 5 },
        { LE_RECT, 0, { LE_NO_FONT, LE_NO_FONT, LE_NO_FONT }, 0, 0, LE_FRAME, 0, 70, 20, 5 },
        { LE_TEXT, 3, { FONT_MEDIUM, LE_NO_FONT, LE_NO_FONT }, 0, 1, LE_BLACK, 10, 80, 100, 30 },
        { LE_RECT, 0, { LE_NO_FONT, LE_NO_FONT, LE_NO_FONT }, 0, 0, LE_RED, 30, 60, 20, 5 },
        { LE_TEXT, 1, { FONT_MEDIUM, LE_NO_FONT, LE_NO_FONT }, 0, 1, LE_RED_INVERT, 100, 10, 90, 40 },
        { LE_END, 0, { 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0 }
    };
    const uint8_t *t[2];
    uint8_t len[2];
    uint16_t w;

    t[0] = L("ignored", &len[0]);
    t[1] = L("Hei", &len[1]);
    layout_apply(&sc, e, t, len, 2, 0);
    CHECK_EQ(sc.nruns, 2);          /* field 3 does not exist */
    CHECK_EQ(sc.nrects, 4);         /* two inverted boxes and two rectangles; the frame needs flag bit1 */
    w = font_text_width(&font_medium, t[1], len[1]);
    /* LE_INVERT: centred in the box made LAYOUT_INVERT_PAD smaller on each side */
    CHECK_EQ(sc.run[0].x, 10 + LAYOUT_INVERT_PAD + (100 - 2 * LAYOUT_INVERT_PAD - w) / 2);
    CHECK_EQ(sc.run[0].y, 10 + LAYOUT_INVERT_PAD + (40 - 2 * LAYOUT_INVERT_PAD - font_medium.height) / 2);
    CHECK_EQ(sc.run[0].color, DRAW_WHITE);
    CHECK_EQ(sc.rect[0].x0, sc.run[0].x - LAYOUT_INVERT_PAD);   /* the box, added with the text */
    CHECK_EQ(sc.rect[0].y0, sc.run[0].y - LAYOUT_INVERT_PAD);
    CHECK_EQ(sc.rect[0].color, DRAW_BLACK);
    CHECK_EQ(sc.rect[1].x1, 19);
    CHECK_EQ(sc.rect[1].y1, 64);
    /* LE_RED: a red rectangle; LE_RED_INVERT: white text in a red box */
    CHECK_EQ(sc.rect[2].color, DRAW_RED);
    CHECK_EQ(sc.rect[2].x0, 30);
    CHECK_EQ(sc.run[1].color, DRAW_WHITE);
    CHECK_EQ(sc.rect[3].color, DRAW_RED);
    CHECK_EQ(sc.rect[3].x0, sc.run[1].x - LAYOUT_INVERT_PAD);
    fake_render(&sc);
    CHECK(fake_ink_count(2) > 20 * 5);

    /* With flag bit1 the frame rectangle is drawn in black */
    layout_apply(&sc, e, t, len, 2, ESL_CF_FRAME);
    CHECK_EQ(sc.nrects, 5);
    CHECK_EQ(sc.rect[2].color, DRAW_BLACK);
    CHECK_EQ(sc.rect[2].y0, 70);
}

/* Two scenes must be the same: every run and rectangle. */
static int same_scene(const draw_scene_t *a, const draw_scene_t *b)
{
    int i;
    if (a->nruns != b->nruns || a->nrects != b->nrects)
        return 0;
    for (i = 0; i < a->nruns; i++) {
        const draw_run_t *x = &a->run[i], *y = &b->run[i];
        if (x->text != y->text || x->len != y->len || x->ellipsis != y->ellipsis || x->font != y->font ||
            x->color != y->color || x->x != y->x || x->y != y->y || x->w != y->w)
            return 0;
    }
    for (i = 0; i < a->nrects; i++)
        if (memcmp(&a->rect[i], &b->rect[i], sizeof(draw_rect_t)) != 0)
            return 0;
    return 1;
}

/* Encodes a built-in layout in the wire format. Returns the number of elements. */
static uint8_t encode_builtin(uint8_t id, uint8_t *out)
{
    const layout_elem_t *e = layout_builtin(id);
    uint8_t n = 0;
    for (; e->kind != LE_END; e++, n++)
        out = put_elem(out, e->kind, e->field, e->font[0], e->font[1], e->font[2], e->opts, e->lines,
                       e->color, e->x, e->y, e->w, e->h);
    return n;
}

static void test_font_heights_match_ap(void)
{
    /* The AP checks layout files with these heights (shared/proto.h) */
    CHECK_EQ(font_get(FONT_SMALL)->height, ESL_FONT_SMALL_H);
    CHECK_EQ(font_get(FONT_MEDIUM)->height, ESL_FONT_MEDIUM_H);
    CHECK_EQ(font_get(FONT_LARGE)->height, ESL_FONT_LARGE_H);
    CHECK_EQ(font_get(FONT_MID)->height, ESL_FONT_MID_H);
}

static void test_wire_matches_builtin(void)
{
    /* The built-in layouts sent as a downloaded file draw exactly the same scene */
    static draw_scene_t sc2;
    static uint8_t wire[16 * ESL_LE_SIZE];
    static const char *const f[] = { "Kjøttdeig av storfe 14 % fett, ekstra stor", "129,90", "kr/kg 108,25", "" };
    const uint8_t *t[4];
    uint8_t len[4], n, id, flags;
    int i;

    for (i = 0; i < 4; i++)
        t[i] = L(f[i], &len[i]);
    for (id = ESL_LAYOUT_PRICE; id <= ESL_LAYOUT_TEXT; id++) {
        n = encode_builtin(id, wire);
        for (flags = 0; flags < 4; flags++) {
            layout_apply(&sc, layout_builtin(id), t, len, 4, flags);
            layout_apply_bin(&sc2, wire, n, t, len, 4, flags);
            CHECK(sc.nruns > 0);
            CHECK(same_scene(&sc, &sc2));
        }
    }
}

/* Builds a file with one layout from the built-in price layout into layout_file. */
static uint16_t price_file(void)
{
    layout_file[0] = ESL_LBIN_FORMAT;
    layout_file[1] = MODEL_CODE;
    layout_file[2] = 1;
    layout_file[3] = 0;
    layout_file[4] = encode_builtin(ESL_LAYOUT_PRICE, layout_file + 5);
    return (uint16_t)(5 + layout_file[4] * ESL_LE_SIZE);
}

static uint32_t crc_of(uint16_t len)
{
    uint32_t c = proto_crc32(0, layout_file, len);
    return c ? c : 1;
}

static void test_file_install(void)
{
    uint16_t len;
    uint8_t n;
    const uint8_t *e;

    len = price_file();
    CHECK(layout_file_install(len, crc_of(len)));
    CHECK(layout_file_ver == crc_of(len));
    e = layout_file_find(0, &n);
    CHECK(e == layout_file + 5);
    CHECK_EQ(n, 7);
    CHECK(layout_file_find(1, &n) == 0);

    /* Content with layout 0x10 is drawn like the built-in price layout */
    {
        static draw_scene_t sc2;
        static const char *const f[] = { "Lettmelk 1 L", "24,90", "kr/l 24,90" };
        build(ESL_LAYOUT_PRICE, ESL_CF_PRICE_HIGHLIGHT, 3, f);
        sc2 = sc;
        build(ESL_LAYOUT_FILE, ESL_CF_PRICE_HIGHLIGHT, 3, f);
        CHECK(same_scene(&sc, &sc2));
    }

    /* Wrong CRC */
    len = price_file();
    CHECK(!layout_file_install(len, crc_of(len) + 1));
    CHECK(layout_file_ver == 0);
    CHECK(layout_file_find(0, &n) == 0);

    /* Each of these makes the file invalid */
#define BAD(stmt) do { len = price_file(); stmt; CHECK(!layout_file_install(len, crc_of(len))); \
                       CHECK(layout_file_ver == 0); } while (0)
    BAD(layout_file[0] = 2);                                   /* format */
    BAD(layout_file[1] = MODEL_CODE == 1 ? 2 : 1);             /* another model */
    BAD(layout_file[2] = 0);                                   /* no layouts */
    BAD(layout_file[2] = 2);                                   /* layout 2 is missing */
    BAD(len--);                                                /* cut */
    BAD(layout_file[len++] = 0);                               /* trailing byte */
    BAD(layout_file[5 + ESL_LE_OFF_KIND] = 4);                 /* element kind */
    BAD(layout_file[5 + ESL_LE_OFF_FIELD] = ESL_MAX_FIELDS);    /* field */
    BAD(layout_file[5 + ESL_LE_OFF_FONT] = ESL_FONT_NONE);     /* text without a font */
    BAD(layout_file[5 + ESL_LE_OFF_FONT + 1] = 4);             /* font */
    BAD(layout_file[5 + ESL_LE_OFF_COLOR] = LE_COLOR_MAX + 1);  /* colour: red only on a model with red */
    BAD(layout_file[5 + ESL_LE_OFF_COLOR] = LE_RED_INVERT + 1);
    BAD(layout_file[5 + ESL_LE_OFF_LINES] = 4);                /* lines */
    BAD(ESL_PUT_U16(layout_file + 5 + ESL_LE_OFF_W, DRAW_W));  /* past the right edge (x = margin) */
    BAD(layout_file[5 + ESL_LE_OFF_H] = DRAW_H);               /* past the bottom */
#undef BAD
    CHECK(!layout_file_install(ESL_LBIN_MAX + 1, 1));          /* too big */
    /* A rectangle may cover the whole screen */
    len = price_file();
    put_elem(layout_file + 5, ESL_LE_RECT, 9, 9, 9, 9, 0, 9, ESL_LE_BLACK, 0, 0, DRAW_W, DRAW_H);
    CHECK(layout_file_install(len, crc_of(len)));
    layout_file_clear();
    CHECK(layout_file_ver == 0);
}

/* A file with one layout: field 0 at the top, fixed text 1 below it, and texts "kr" and "inkl. mva" */
static uint16_t fixed_file(void)
{
    uint8_t *p = layout_file + 5;
    layout_file[0] = ESL_LBIN_FORMAT;
    layout_file[1] = MODEL_CODE;
    layout_file[2] = 1;
    layout_file[ESL_LBIN_OFF_NTEXT] = 2;
    layout_file[4] = 2;
    p = put_elem(p, ESL_LE_TEXT, 0, ESL_FONT_MEDIUM, ESL_FONT_NONE, ESL_FONT_NONE, 0, 1, ESL_LE_BLACK,
                 5, 4, DRAW_W - 10, 30);
    p = put_elem(p, ESL_LE_TEXT, ESL_LE_FIELD_FIXED | 1, ESL_FONT_SMALL, ESL_FONT_NONE, ESL_FONT_NONE,
                 ESL_LE_RIGHT, 1, ESL_LE_BLACK, 5, 40, DRAW_W - 10, 20);
    *p++ = 2;
    *p++ = 'k';
    *p++ = 'r';
    *p++ = 9;
    memcpy(p, "inkl. mva", 9);
    p += 9;
    return (uint16_t)(p - layout_file);
}

static void test_fixed_text(void)
{
    static const char *const f[] = { "Kaffe" };
    uint16_t len;

    len = fixed_file();
    CHECK(layout_file_install(len, crc_of(len)));
    build(ESL_LAYOUT_FILE, 0, 1, f);
    CHECK_EQ(sc.nruns, 2);
    CHECK_EQ(sc.run[0].len, 5);
    CHECK_EQ(sc.run[1].len, 9);
    CHECK(memcmp(sc.run[1].text, "inkl. mva", 9) == 0);
    CHECK_EQ(sc.run[1].x + sc.run[1].w, DRAW_W - 5);      /* right-aligned like any text */
    /* the fixed text is drawn without any content fields too */
    build(ESL_LAYOUT_FILE, 0, 0, f);
    CHECK_EQ(sc.nruns, 1);

    /* after a restart the texts come back with the file */
    len = fixed_file();
    layout_file_clear();
    CHECK(layout_file_restore());
    CHECK_EQ(layout_file_len, len);

    /* Each of these makes the file invalid */
#define BAD(stmt) do { len = fixed_file(); stmt; CHECK(!layout_file_install(len, crc_of(len))); \
                       CHECK(layout_file_ver == 0); } while (0)
    BAD(layout_file[ESL_LBIN_OFF_NTEXT] = 1);                      /* text 1 does not exist, and a trailing byte */
    BAD(layout_file[ESL_LBIN_OFF_NTEXT] = 3);                      /* text 3 is missing */
    BAD(layout_file[ESL_LBIN_OFF_NTEXT] = ESL_LBIN_TEXT_MAX + 1);
    BAD(layout_file[5 + ESL_LE_SIZE + ESL_LE_OFF_FIELD] = ESL_LE_FIELD_FIXED | 2);
    BAD(layout_file[5 + 2 * ESL_LE_SIZE] = 0);                     /* empty text */
    BAD(layout_file[5 + 2 * ESL_LE_SIZE] = 3);                     /* lengths do not add up */
    BAD(len--);                                                    /* cut */
#undef BAD
    layout_file_clear();
}

static void test_status_lines(void)
{
    /* The start screen: 4 lines; the MAC line must fit without "…" */
    static const char *const s[] = { "OpenESL", "MAC 0000000197E5CB3B", "FW 0.1.0", "Waiting for content" };
    const uint8_t *t[4];
    uint8_t len[4];
    int i;
    for (i = 0; i < 4; i++)
        t[i] = L(s[i], &len[i]);
    layout_build_lines(&sc, t, len, 4);
    CHECK_EQ(sc.nruns, 4);
    for (i = 0; i < sc.nruns; i++)
        CHECK_EQ(sc.run[i].ellipsis, 0);
    check_inside();
    check_no_overlap();
}

void test_layout(void)
{
    RUN(test_fit);
    RUN(test_wrap);
    RUN(test_price_layout);
    RUN(test_price_highlight);
    RUN(test_long_name_shrinks);
    RUN(test_too_long_name_truncated);
    RUN(test_price_font_chain);
    RUN(test_price_baseline);
    RUN(test_norwegian);
    RUN(test_text_layout);
    RUN(test_text_line_shrinks);
    RUN(test_frame);
    RUN(test_max_length_fields);
    RUN(test_custom_elements);
    RUN(test_font_heights_match_ap);
    RUN(test_wire_matches_builtin);
    RUN(test_file_install);
    RUN(test_fixed_text);
    RUN(test_status_lines);
}
