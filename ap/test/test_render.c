/* test_render.c - esl_render: the AP draws a tag's screen. The drawing itself is checked
 * against the tag's own code in sim/ (every layout of the example files, both models);
 * here only the entry points. */
#include <string.h>
#include "esl_render.h"
#include "esl_text.h"
#include "tinytest.h"

static const char FILE_29[] =
    "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"sign\",\"fields\":[\"title\"],"
    "\"elements\":[{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":296,\"h\":128},"
    "{\"type\":\"text\",\"field\":\"title\",\"x\":8,\"y\":40,\"w\":280,\"h\":48,\"fonts\":[\"medium\"],\"color\":\"white\"}]}]}";

static size_t ink(const uint8_t *img, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        for (uint8_t m = 0x80; m; m >>= 1)
            if (img[i] & m)
                k++;
    return k;
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

    /* straight lines: 1 px from end to end, nothing past the ends; 3 px is rows 29-31 */
    for (x = 10; x <= 50; x++)
        CHECK(px(img, x, 20) && !px(img, x, 19) && !px(img, x, 21));
    CHECK(!px(img, 9, 20) && !px(img, 51, 20));
    CHECK(px(img, 30, 29) && px(img, 30, 31) && !px(img, 30, 28) && !px(img, 30, 32));
    CHECK(px(img, 9, 30) && !px(img, 8, 30)); /* a round end, 1.5 px past the end point */
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
    /* the icon: its two pixels where they are in the picture, nothing else */
    CHECK(px(img, 260, 50) && px(img, 267, 51) && !px(img, 261, 50) && !px(img, 260, 51) && !px(img, 267, 50));
}

void test_render(void)
{
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

    test_shapes();
}
