/* test_render.c - esl_render: the AP draws a tag's screen. The drawing itself is checked
 * against the tag's own code in sim/ (every layout of the example files, both models);
 * here only the entry points. */
#include <stdio.h>
#include <string.h>
#include "esl_render.h"
#include "esl_text.h"
#include "tinytest.h"

static const char FILE_29[] =
    "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"sign\",\"fields\":[\"title\"],"
    "\"elements\":[{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":296,\"h\":128},"
    "{\"type\":\"text\",\"field\":\"title\",\"x\":8,\"y\":40,\"w\":280,\"h\":48,\"fonts\":[\"medium\"],\"color\":\"white\"}]}]}";

static const char FILE_PRICE[] =
    "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":["
    "{\"name\":\"fmt\",\"fields\":[\"price\"],\"elements\":[{\"type\":\"text\",\"field\":\"price\","
    "\"x\":0,\"y\":0,\"w\":296,\"h\":60,\"fonts\":[\"large\"],\"decimals\":2,\"thousands\":true}]},"
    "{\"name\":\"raw\",\"fields\":[\"price\"],\"elements\":[{\"type\":\"text\",\"field\":\"price\","
    "\"x\":0,\"y\":0,\"w\":296,\"h\":60,\"fonts\":[\"large\"]}]},"
    /* tall enough for two lines of large: a formatted number still keeps to one, in mid */
    "{\"name\":\"tall\",\"fields\":[\"price\"],\"elements\":[{\"type\":\"text\",\"field\":\"price\","
    "\"x\":0,\"y\":0,\"w\":250,\"h\":120,\"fonts\":[\"large\",\"mid\"],\"decimals\":2,\"thousands\":true}]},"
    "{\"name\":\"tall1\",\"fields\":[\"price\"],\"elements\":[{\"type\":\"text\",\"field\":\"price\","
    "\"x\":0,\"y\":0,\"w\":250,\"h\":120,\"fonts\":[\"large\",\"mid\"],\"lines\":1}]},"
    "{\"name\":\"tall0\",\"fields\":[\"price\"],\"elements\":[{\"type\":\"text\",\"field\":\"price\","
    "\"x\":0,\"y\":0,\"w\":250,\"h\":120,\"fonts\":[\"large\",\"mid\"]}]},"
    /* room for "9,90" but not "1 299,90" in either font */
    "{\"name\":\"narrow\",\"fields\":[\"price\"],\"elements\":[{\"type\":\"text\",\"field\":\"price\","
    "\"x\":0,\"y\":0,\"w\":110,\"h\":60,\"fonts\":[\"large\",\"mid\"],\"decimals\":2,\"thousands\":true}]}]}";

static size_t ink(const uint8_t *img, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        for (uint8_t m = 0x80; m; m >>= 1)
            if (img[i] & m)
                k++;
    return k;
}

static void named(esl_content_t *c, const char *layout, const char *f0)
{
    esl_content_init_named(c, layout, strlen(layout), 0);
    esl_content_add_field(c, (const uint8_t *)f0, strlen(f0));
}

/* Pixel (x, y) of the black plane of a 2.9" image (column by column, 16 bytes each) */
static bool px(const uint8_t *img, unsigned x, unsigned y)
{
    return (img[x * 16 + y / 8] & (0x80 >> (y % 8))) != 0;
}

static void content(esl_content_t *c, uint8_t layout, const char *f0)
{
    esl_content_init(c, layout, 0);
    esl_content_add_field(c, (const uint8_t *)f0, strlen(f0));
}

/* Lines, rounded and open rects and icons (layout format 2), pixel by pixel */
static void test_shapes(void)
{
    static const char shapes[] =
        "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\","
        "\"icons\":{\"corner\":{\"w\":8,\"h\":2,\"data\":\"gAE=\"}},"
        "\"layouts\":[{\"name\":\"s\",\"fields\":[\"a\"],\"elements\":["
        "{\"type\":\"line\",\"x\":10,\"y\":20,\"w\":41,\"h\":1},"
        "{\"type\":\"line\",\"x\":10,\"y\":30,\"w\":41,\"h\":1,\"width\":3},"
        "{\"type\":\"line\",\"x\":10,\"y\":40,\"w\":41,\"h\":1,\"width\":2},"
        "{\"type\":\"line\",\"x\":10,\"y\":50,\"w\":41,\"h\":1,\"width\":4},"
        "{\"type\":\"line\",\"x\":180,\"y\":10,\"w\":1,\"h\":30,\"width\":2},"
        "{\"type\":\"line\",\"x\":60,\"y\":10,\"w\":41,\"h\":41},"
        "{\"type\":\"line\",\"x\":110,\"y\":10,\"w\":41,\"h\":41,\"direction\":\"up\"},"
        "{\"type\":\"line\",\"x\":160,\"y\":10,\"w\":11,\"h\":91},"
        "{\"type\":\"rect\",\"x\":200,\"y\":10,\"w\":20,\"h\":20,\"radius\":5},"
        "{\"type\":\"rect\",\"x\":230,\"y\":10,\"w\":20,\"h\":20,\"radius\":5,\"border\":2},"
        "{\"type\":\"rect\",\"x\":200,\"y\":60,\"w\":30,\"h\":30,\"border\":1},"
        "{\"type\":\"icon\",\"icon\":\"corner\",\"x\":260,\"y\":50}]}]}";
    static uint8_t img[296 * 16];
    static esl_layout_bin_t bin;
    char err[ESL_LAYOUT_ERR_MAX];
    esl_content_t c;
    unsigned x, y, n;

    CHECK(esl_layout_compile(shapes, sizeof(shapes) - 1, ESL_MODEL_29, &bin, err, sizeof(err)));
    esl_content_init_named(&c, "s", 1, 0);
    esl_content_add_field(&c, (const uint8_t *)"x", 1);
    CHECK(esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img)) == sizeof(img));

    /* straight lines: exactly their box along them and width px across it, (width - 1) / 2
     * before it: 1 px is row 20, 3 px rows 29-31, 2 px rows 40-41, 4 px rows 49-52 */
    for (x = 10; x <= 50; x++)
        CHECK(px(img, x, 20) && !px(img, x, 19) && !px(img, x, 21));
    CHECK(!px(img, 9, 20) && !px(img, 51, 20));
    CHECK(px(img, 30, 29) && px(img, 30, 31) && !px(img, 30, 28) && !px(img, 30, 32));
    CHECK(!px(img, 9, 30) && !px(img, 51, 30) && px(img, 10, 31) && px(img, 50, 29)); /* square ends */
    for (x = 10; x <= 50; x++) {
        CHECK(!px(img, x, 39) && px(img, x, 40) && px(img, x, 41) && !px(img, x, 42));
        CHECK(!px(img, x, 48) && px(img, x, 49) && px(img, x, 52) && !px(img, x, 53));
    }
    CHECK(!px(img, 9, 40) && !px(img, 51, 40) && !px(img, 9, 50) && !px(img, 51, 50));
    for (y = 10; y <= 39; y++)
        CHECK(!px(img, 179, y) && px(img, 180, y) && px(img, 181, y) && !px(img, 182, y));
    CHECK(!px(img, 180, 9) && !px(img, 180, 40));
    /* 45 degrees: one pixel per column, down from (60, 10), and up from (110, 50) */
    for (x = 60; x <= 100; x++) {
        n = 0;
        for (y = 0; y < 128; y++)
            n += px(img, x, y);
        CHECK(px(img, x, 10 + x - 60) && n == 1);
        CHECK(px(img, 110 + x - 60, 50 - (x - 60)));
    }
    /* steep: no gap between rows */
    for (y = 10; y <= 100; y++) {
        n = 0;
        for (x = 158; x <= 172; x++)
            n += px(img, x, y);
        CHECK(n >= 1 && n <= 2);
    }

    /* a rounded rect: the corner pixel is left out, the middle of each side is in */
    CHECK(!px(img, 200, 10) && !px(img, 219, 29) && px(img, 205, 10) && px(img, 200, 20) && px(img, 210, 20));
    CHECK(px(img, 200, 13) && !px(img, 200, 12) && px(img, 201, 11) && !px(img, 201, 10));
    /* with a 2 px border: the inside is empty, the sides 2 px wide */
    CHECK(!px(img, 230, 10) && px(img, 240, 10) && px(img, 240, 11) && !px(img, 240, 12) && !px(img, 240, 20));
    CHECK(px(img, 230, 20) && px(img, 231, 20) && !px(img, 232, 20) && px(img, 249, 20) && !px(img, 247, 20));
    /* a square border, 1 px */
    CHECK(px(img, 200, 60) && px(img, 229, 89) && px(img, 215, 60) && !px(img, 215, 61) && !px(img, 215, 75));
    /* a QR code on a black box: its quiet zone (2 modules of 2 px) is white, the finder black */
    {
        static const char qr[] =
            "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"q\","
            "\"fields\":[\"a\"],\"elements\":[{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":100,\"h\":100},"
            "{\"type\":\"qr\",\"field\":\"a\",\"x\":0,\"y\":0,\"w\":50,\"h\":50}]}]}";
        CHECK(esl_layout_compile(qr, sizeof(qr) - 1, ESL_MODEL_29, &bin, err, sizeof(err)));
        esl_content_init_named(&c, "q", 1, 0);
        esl_content_add_field(&c, (const uint8_t *)"x", 1);
        CHECK(esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img)) == sizeof(img));
        CHECK(!px(img, 0, 0) && !px(img, 3, 3) && !px(img, 49, 0) && !px(img, 0, 49) && !px(img, 49, 49));
        CHECK(px(img, 4, 4) && px(img, 17, 4) && px(img, 50, 0) && px(img, 0, 50));
        CHECK(esl_layout_compile(shapes, sizeof(shapes) - 1, ESL_MODEL_29, &bin, err, sizeof(err)));
        esl_content_init_named(&c, "s", 1, 0);
        esl_content_add_field(&c, (const uint8_t *)"x", 1);
        CHECK(esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img)) == sizeof(img));
    }
    /* the icon: its two pixels where they are in the picture, nothing else */
    CHECK(px(img, 260, 50) && px(img, 267, 51) && !px(img, 261, 50) && !px(img, 260, 51) && !px(img, 267, 50));
}

/* An icon named by a field: the field's text picks the icon, centred in the box; an unknown
 * name, or an icon bigger than the box, draws nothing */
static void test_icon_field(void)
{
    static const char file[] =
        "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\","
        "\"icons\":{\"dot\":{\"w\":2,\"h\":2,\"data\":\"wMA=\"},\"big\":{\"w\":8,\"h\":8,\"data\":\"//////////8=\"}},"
        "\"layouts\":[{\"name\":\"w\",\"fields\":[\"t\",\"sym\"],\"elements\":["
        "{\"type\":\"icon\",\"field\":\"sym\",\"x\":100,\"y\":20,\"w\":6,\"h\":4},"
        "{\"type\":\"text\",\"text\":\"dot\",\"x\":0,\"y\":100,\"w\":50,\"h\":20,\"fonts\":[\"small\"]}]}]}";
    static uint8_t img[296 * 16];
    static esl_layout_bin_t bin;
    char err[ESL_LAYOUT_ERR_MAX];
    esl_content_t c;

    CHECK(esl_layout_compile(file, sizeof(file) - 1, ESL_MODEL_29, &bin, err, sizeof(err)));
    esl_content_init_named(&c, "w", 1, 0);
    esl_content_add_field(&c, (const uint8_t *)"x", 1);
    esl_content_add_field(&c, (const uint8_t *)"dot", 3);
    CHECK(esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img)) == sizeof(img));
    /* 2x2 in a 6x4 box at (100, 20): at (102, 21) */
    CHECK(px(img, 102, 21) && px(img, 103, 22) && !px(img, 101, 21) && !px(img, 104, 21) && !px(img, 102, 20));
    /* too big for the box, unknown or empty: nothing in the box */
    static const char *const none[] = {"big", "do", "dots", ""};
    for (int k = 0; k < 4; k++) {
        esl_content_init_named(&c, "w", 1, 0);
        esl_content_add_field(&c, (const uint8_t *)"x", 1);
        esl_content_add_field(&c, (const uint8_t *)none[k], strlen(none[k]));
        CHECK(esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img)) == sizeof(img));
        for (unsigned x = 100; x < 106; x++)
            for (unsigned y = 20; y < 24; y++)
                CHECK(!px(img, x, y));
    }
}

/* Turned text (ESL_LE_ROT): in a square box each turn draws exactly the plain text turned
 * round the box, inverted fields with their box too */
static void test_rotate(void)
{
    static const char *const colors[] = {"black", "invert"};
    static uint8_t plain[296 * 16], turned[296 * 16];
    static esl_layout_bin_t bin;
    static char file[600];
    char err[ESL_LAYOUT_ERR_MAX];
    esl_content_t c;
    const unsigned X = 30, Y = 10, S = 100;

    for (int k = 0; k < 2; k++)
        for (unsigned rot = 0; rot < 4; rot++) {
            snprintf(file, sizeof(file),
                     "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"r\","
                     "\"fields\":[\"t\"],\"elements\":[{\"type\":\"text\",\"field\":\"t\",\"x\":%u,\"y\":%u,"
                     "\"w\":%u,\"h\":%u,\"fonts\":[\"medium\"],\"align\":\"center\",\"color\":\"%s\",\"rotate\":%u}]}]}",
                     X, Y, S, S, colors[k], rot * 90);
            CHECK(esl_layout_compile(file, strlen(file), ESL_MODEL_29, &bin, err, sizeof(err)));
            CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_LINES], rot << ESL_LE_ROT_SHIFT);
            named(&c, "r", "Turned text, two lines");
            CHECK(esl_render(ESL_MODEL_29, &bin, &c, rot ? turned : plain, sizeof(plain)) == sizeof(plain));
            if (!rot) {
                CHECK(ink(plain, sizeof(plain)) > 100);
                continue;
            }
            unsigned bad = 0;
            for (unsigned u = 0; u < S; u++)
                for (unsigned v = 0; v < S; v++) {
                    unsigned tx = rot == 1 ? X + S - 1 - v : rot == 2 ? X + S - 1 - u : X + v;
                    unsigned ty = rot == 1 ? Y + u : rot == 2 ? Y + S - 1 - v : Y + S - 1 - u;
                    bad += px(plain, X + u, Y + v) != px(turned, tx, ty);
                }
            CHECK_INT(bad, 0);
            CHECK_INT(ink(turned, sizeof(turned)), ink(plain, sizeof(plain)));
        }

    /* a tall narrow box along the left edge of the 2.9": the text runs bottom to top */
    static const char side[] =
        "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"s\","
        "\"fields\":[\"t\"],\"elements\":[{\"type\":\"text\",\"field\":\"t\",\"x\":0,\"y\":0,"
        "\"w\":24,\"h\":128,\"fonts\":[\"medium\"],\"rotate\":270}]}]}";
    CHECK(esl_layout_compile(side, sizeof(side) - 1, ESL_MODEL_29, &bin, err, sizeof(err)));
    named(&c, "s", "SALE");
    CHECK(esl_render(ESL_MODEL_29, &bin, &c, turned, sizeof(turned)) == sizeof(turned));
    size_t in = 0;
    for (unsigned x = 0; x < 24; x++)
        for (unsigned y = 0; y < 128; y++)
            in += px(turned, x, y);
    CHECK(in > 50 && in == ink(turned, sizeof(turned)));
    /* left-aligned along the turned box: the text starts at the bottom */
    bool low = false, high = false;
    for (unsigned x = 0; x < 24; x++) {
        low |= px(turned, x, 120);
        high |= px(turned, x, 10);
    }
    CHECK(low && !high);
}

/* The 2.9" with red (#255): two planes, black then red, and no pixel in both */
static void test_red(void)
{
    static const char file[] =
        "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9-bwr\",\"layouts\":[{\"name\":\"r\","
        "\"fields\":[\"a\",\"b\"],\"elements\":["
        "{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":296,\"h\":20,\"color\":\"red\"},"
        "{\"type\":\"text\",\"field\":\"a\",\"x\":0,\"y\":30,\"w\":296,\"h\":40,\"fonts\":[\"medium\"],\"color\":\"red_invert\"},"
        "{\"type\":\"text\",\"field\":\"b\",\"x\":0,\"y\":80,\"w\":296,\"h\":40,\"fonts\":[\"medium\"]}]}]}";
    enum { PLANE = 296 * 16 };
    static uint8_t img[2 * PLANE];
    static esl_layout_bin_t bin;
    char err[ESL_LAYOUT_ERR_MAX];
    esl_content_t c;
    uint16_t w, h;
    bool red;

    CHECK(esl_render_model(ESL_MODEL_293, &w, &h, &red) && w == 296 && h == 128 && red);
    CHECK(esl_render_size(ESL_MODEL_293) == 2 * PLANE);
    CHECK(esl_layout_compile(file, sizeof(file) - 1, ESL_MODEL_293, &bin, err, sizeof(err)));
    /* the black/white 2.9" has the same screen but refuses the red */
    static esl_layout_bin_t bin29;
    CHECK(!esl_layout_compile(file, sizeof(file) - 1, ESL_MODEL_29, &bin29, err, sizeof(err)));

    esl_content_init_named(&c, "r", 1, 0);
    esl_content_add_field(&c, (const uint8_t *)"Tilbud", 6);
    esl_content_add_field(&c, (const uint8_t *)"49,90", 5);
    CHECK(esl_render(ESL_MODEL_293, &bin, &c, img, sizeof(img)) == 2 * PLANE);
    CHECK(esl_render(ESL_MODEL_293, &bin, &c, img, 2 * PLANE - 1) == 0);
    const uint8_t *blk = img, *rd = img + PLANE;
    bool both = false, band = true, black_top = false, red_bottom = false;
    for (size_t i = 0; i < PLANE; i++)
        both |= (blk[i] & rd[i]) != 0;
    CHECK(!both);
    for (unsigned x = 0; x < 296; x++)
        for (unsigned y = 0; y < 128; y++) {
            bool b = px(blk, x, y), r = px(rd, x, y);
            if (y < 20 && (!r || b))
                band = false;                /* the red band */
            if (y < 72 && b)
                black_top = true;            /* nothing black above the black text */
            if (y >= 72 && r)
                red_bottom = true;           /* nothing red below the red_invert box */
        }
    CHECK(band && !black_top && !red_bottom);
    /* the red_invert text: a red box with white text in it, so mostly red but not all */
    size_t box = 0;
    for (unsigned x = 0; x < 296; x++)
        for (unsigned y = 30; y < 70; y++)
            box += px(rd, x, y);
    CHECK(box > 0 && box < 296u * 40u);
    /* the black text below */
    CHECK(ink(blk, PLANE) > 50);
}

void test_render(void)
{
    test_icon_field();
    test_rotate();
    static uint8_t img[8192], img2[8192];
    static esl_layout_bin_t bin;
    char err[ESL_LAYOUT_ERR_MAX];
    esl_content_t c;
    uint16_t w, h;
    bool red;
    size_t n;

    CHECK(esl_render_model(ESL_MODEL_29, &w, &h, &red) && w == 296 && h == 128 && !red);
    CHECK(esl_render_model(ESL_MODEL_154, &w, &h, &red) && w == 200 && h == 200);
    CHECK(!esl_render_model(0, &w, &h, &red) && !esl_render_model(ESL_MODEL_COUNT, &w, &h, &red));
    CHECK(esl_render_size(ESL_MODEL_29) == 296 * 16 && esl_render_size(ESL_MODEL_154) == 200 * 25);
    CHECK(esl_render_size(0) == 0);

    /* built-in text, no file: ink where the text is, nothing in an image too small */
    content(&c, ESL_LAYOUT_TEXT, "Hei");
    n = esl_render(ESL_MODEL_29, NULL, &c, img, sizeof(img));
    CHECK(n == 296 * 16);
    CHECK(ink(img, n) > 20 && ink(img, n) < 400);
    CHECK(esl_render(ESL_MODEL_29, NULL, &c, img, n - 1) == 0);
    CHECK(esl_render(0, NULL, &c, img, sizeof(img)) == 0);

    /* the same content twice is the same image; other content is not */
    CHECK(esl_render(ESL_MODEL_29, NULL, &c, img2, sizeof(img2)) == n && memcmp(img, img2, n) == 0);
    content(&c, ESL_LAYOUT_TEXT, "Hallo");
    CHECK(esl_render(ESL_MODEL_29, NULL, &c, img2, sizeof(img2)) == n && memcmp(img, img2, n) != 0);

    /* a named layout from the file: white text on a black screen, so mostly ink */
    CHECK(esl_layout_compile(FILE_29, sizeof(FILE_29) - 1, ESL_MODEL_29, &bin, err, sizeof(err)));
    esl_content_init_named(&c, "sign", 4, 0);
    esl_content_add_field(&c, (const uint8_t *)"Hei", 3);
    n = esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img));
    CHECK(n == 296 * 16 && ink(img, n) > 296 * 128 * 3 / 4);
    /* layout 0x10 is the same layout by index */
    content(&c, ESL_LAYOUT_FILE, "Hei");
    CHECK(esl_render(ESL_MODEL_29, &bin, &c, img2, sizeof(img2)) == n && memcmp(img, img2, n) == 0);
    /* a name the file does not have, or no file at all: not drawn (the AP holds such content back) */
    esl_content_init_named(&c, "nope", 4, 0);
    esl_content_add_field(&c, (const uint8_t *)"Hei", 3);
    CHECK(esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img)) == 0);
    esl_content_init_named(&c, "sign", 4, 0);
    esl_content_add_field(&c, (const uint8_t *)"Hei", 3);
    CHECK(esl_render(ESL_MODEL_29, NULL, &c, img, sizeof(img)) == 0);
    /* a layout number with no file: the built-in text layout, as on the tag */
    content(&c, ESL_LAYOUT_TEXT, "Hei");
    n = esl_render(ESL_MODEL_29, NULL, &c, img2, sizeof(img2));
    CHECK(n == 296 * 16);
    content(&c, ESL_LAYOUT_FILE + 3, "Hei");
    CHECK(esl_render(ESL_MODEL_29, NULL, &c, img, sizeof(img)) == n && memcmp(img, img2, n) == 0);

    /* a number format: "1299.9" is drawn as "1 299,90"; text that is not a number as sent */
    CHECK(esl_layout_compile(FILE_PRICE, sizeof(FILE_PRICE) - 1, ESL_MODEL_29, &bin, err, sizeof(err)));
    named(&c, "fmt", "1299.9");
    n = esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img));
    named(&c, "raw", "1 299,90");
    CHECK(n == 296 * 16 && esl_render(ESL_MODEL_29, &bin, &c, img2, sizeof(img2)) == n && memcmp(img, img2, n) == 0);
    named(&c, "raw", "1299.9");
    CHECK(esl_render(ESL_MODEL_29, &bin, &c, img2, sizeof(img2)) == n && memcmp(img, img2, n) != 0);
    named(&c, "tall", "12345.5");
    n = esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img));
    named(&c, "tall1", "12 345,50");
    CHECK(n && esl_render(ESL_MODEL_29, &bin, &c, img2, sizeof(img2)) == n && memcmp(img, img2, n) == 0);
    named(&c, "tall0", "12 345,50"); /* as text it wraps after "12" */
    CHECK(esl_render(ESL_MODEL_29, &bin, &c, img2, sizeof(img2)) == n && memcmp(img, img2, n) != 0);
    /* a number that fits in none of its fonts is not cut short (a wrong price): it is left
       out, and esl_render_number_cut says so for the API */
    named(&c, "narrow", "9.9");
    n = esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img));
    CHECK(n && ink(img, n) > 0 && !esl_render_number_cut(ESL_MODEL_29, &bin, &c));
    named(&c, "narrow", "1299.9");
    n = esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img));
    CHECK(n && ink(img, n) == 0 && esl_render_number_cut(ESL_MODEL_29, &bin, &c));
    named(&c, "fmt", "1299.9");
    CHECK(!esl_render_number_cut(ESL_MODEL_29, &bin, &c));
    content(&c, ESL_LAYOUT_TEXT, "a long text that does not fit on the screen at all, cut with an ellipsis");
    CHECK(!esl_render_number_cut(ESL_MODEL_29, NULL, &c));
    named(&c, "fmt", "12:30");
    n = esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img));
    named(&c, "raw", "12:30");
    CHECK(n && esl_render(ESL_MODEL_29, &bin, &c, img2, sizeof(img2)) == n && memcmp(img, img2, n) == 0);

    test_shapes();
    test_red();
}
