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
    "\"x\":0,\"y\":0,\"w\":250,\"h\":120,\"fonts\":[\"large\",\"mid\"]}]}]}";

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

static void content(esl_content_t *c, uint8_t layout, const char *f0)
{
    esl_content_init(c, layout, 0);
    esl_content_add_field(c, (const uint8_t *)f0, strlen(f0));
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
    named(&c, "fmt", "12:30");
    n = esl_render(ESL_MODEL_29, &bin, &c, img, sizeof(img));
    named(&c, "raw", "12:30");
    CHECK(n && esl_render(ESL_MODEL_29, &bin, &c, img2, sizeof(img2)) == n && memcmp(img, img2, n) == 0);
}
