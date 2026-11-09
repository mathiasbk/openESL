/* Yellow (black/white/red/yellow Bluetooth tags, docs/ble-tags.md): the AP draws a third
 * plane after black and red. Built with the registry in models_gen.h here, which has a
 * black/white/red/yellow 2.9" (ESL_MODEL_BWRY) and a black/white/yellow one (ESL_MODEL_BWY). */
#include "../tinytest.h"
#include "esl_crc32.h"
#include "esl_image.h"
#include "esl_layout.h"
#include "esl_proto.h"
#include "esl_render.h"
#include <stdlib.h>

void test_yellow(void);

enum { W = 296, H = 128, PLANE = W * 16 };

static bool px(const uint8_t *plane, unsigned x, unsigned y)
{
    return (plane[x * 16 + y / 8] >> (7 - y % 8)) & 1;
}

static const char file_bwry[] =
    "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9-bwry\",\"layouts\":[{\"name\":\"y\","
    "\"fields\":[\"a\"],\"elements\":["
    "{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":296,\"h\":20,\"color\":\"yellow\"},"
    "{\"type\":\"rect\",\"x\":0,\"y\":20,\"w\":296,\"h\":20,\"color\":\"red\"},"
    "{\"type\":\"line\",\"x\":0,\"y\":50,\"w\":296,\"h\":1,\"width\":3,\"color\":\"yellow\"},"
    "{\"type\":\"text\",\"field\":\"a\",\"x\":0,\"y\":80,\"w\":296,\"h\":40,\"fonts\":[\"medium\"]}]}]}";

static void test_draw(void)
{
    static uint8_t img[3 * PLANE];
    static esl_layout_bin_t bin;
    char err[ESL_LAYOUT_ERR_MAX];
    esl_content_t c;

    CHECK(esl_model_has_yellow(ESL_MODEL_BWRY) && esl_model_has_red(ESL_MODEL_BWRY));
    CHECK(esl_model_has_yellow(ESL_MODEL_BWY) && !esl_model_has_red(ESL_MODEL_BWY));
    CHECK(!esl_model_has_yellow(ESL_MODEL_29) && !esl_model_has_yellow(0));
    CHECK_INT(esl_render_size(ESL_MODEL_BWRY), 3 * PLANE);
    CHECK_INT(esl_render_size(ESL_MODEL_BWY), 2 * PLANE);
    CHECK_INT(esl_render_size(ESL_MODEL_29), PLANE);

    CHECK(esl_layout_compile(file_bwry, sizeof(file_bwry) - 1, ESL_MODEL_BWRY, &bin, err, sizeof(err)));
    esl_content_init_named(&c, "y", 1, 0);
    esl_content_add_field(&c, (const uint8_t *)"Tilbud", 6);
    CHECK(esl_render(ESL_MODEL_BWRY, &bin, &c, img, sizeof(img)) == 3 * PLANE);
    const uint8_t *blk = img, *rd = img + PLANE, *yl = img + 2 * PLANE;
    bool two = false, yband = true, rband = true, yline = true, stray = false;
    size_t text = 0;
    for (size_t i = 0; i < PLANE; i++)
        two |= (blk[i] & rd[i]) || (blk[i] & yl[i]) || (rd[i] & yl[i]);
    CHECK(!two);
    for (unsigned x = 0; x < W; x++)
        for (unsigned y = 0; y < H; y++) {
            bool b = px(blk, x, y), r = px(rd, x, y), ye = px(yl, x, y);
            if (y < 20)
                yband &= ye && !r && !b;
            else if (y < 40)
                rband &= r && !ye && !b;
            else if (y == 50)
                yline &= ye && !b && !r;
            else if (y < 80)
                stray |= b || r || (ye && (y < 46 || y > 54)); /* the line is 3 wide */
            else
                text += b, stray |= r || ye;
        }
    CHECK(yband && rband && yline && !stray);
    CHECK(text > 50);

    /* black/white/yellow: the yellow plane comes right after the black one */
    static const char file_bwy[] =
        "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9-bwy\",\"layouts\":[{\"name\":\"y\","
        "\"fields\":[\"a\"],\"elements\":["
        "{\"type\":\"ellipse\",\"x\":0,\"y\":0,\"w\":40,\"h\":40,\"color\":\"yellow\"}]}]}";
    CHECK(esl_layout_compile(file_bwy, sizeof(file_bwy) - 1, ESL_MODEL_BWY, &bin, err, sizeof(err)));
    CHECK(esl_render(ESL_MODEL_BWY, &bin, &c, img, sizeof(img)) == 2 * PLANE);
    CHECK(px(img + PLANE, 20, 20) && !px(img, 20, 20) && !px(img + PLANE, 100, 20));

    /* red is refused without red, yellow without yellow, and yellow_invert does not exist */
    static const char red_on_bwy[] =
        "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9-bwy\",\"layouts\":[{\"name\":\"y\","
        "\"fields\":[\"a\"],\"elements\":["
        "{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":40,\"h\":40,\"color\":\"red\"}]}]}";
    CHECK(!esl_layout_compile(red_on_bwy, sizeof(red_on_bwy) - 1, ESL_MODEL_BWY, &bin, err, sizeof(err)));
    CHECK(strstr(err, "red and red_invert need a model with red") != NULL);
    static const char yellow_on_bw[] =
        "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"y\","
        "\"fields\":[\"a\"],\"elements\":["
        "{\"type\":\"text\",\"field\":\"a\",\"x\":0,\"y\":0,\"w\":296,\"h\":40,\"fonts\":[\"medium\"],"
        "\"color\":\"yellow\"}]}]}";
    CHECK(!esl_layout_compile(yellow_on_bw, sizeof(yellow_on_bw) - 1, ESL_MODEL_29, &bin, err, sizeof(err)));
    CHECK(strstr(err, "yellow needs a model with yellow") != NULL);
    static const char invert[] =
        "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9-bwry\",\"layouts\":[{\"name\":\"y\","
        "\"fields\":[\"a\"],\"elements\":["
        "{\"type\":\"line\",\"x\":0,\"y\":0,\"w\":296,\"h\":1,\"color\":\"invert\"}]}]}";
    CHECK(!esl_layout_compile(invert, sizeof(invert) - 1, ESL_MODEL_BWRY, &bin, err, sizeof(err)));
    CHECK(strstr(err, "a line can be black, white, red or yellow") != NULL);
}

/* ---- pictures ------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t left;
} mem_t;

static bool mem_read(void *ctx, uint8_t *buf, size_t n)
{
    mem_t *m = ctx;
    if (n > m->left)
        return false;
    memcpy(buf, m->p, n);
    m->p += n;
    m->left -= n;
    return true;
}

static uint8_t *put32(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)(v >> 24);
    o[1] = (uint8_t)(v >> 16);
    o[2] = (uint8_t)(v >> 8);
    o[3] = (uint8_t)v;
    return o + 4;
}

static uint8_t *chunk(uint8_t *o, const char *type, const uint8_t *data, uint32_t len)
{
    o = put32(o, len);
    uint8_t *start = o;
    memcpy(o, type, 4);
    if (len)
        memcpy(o + 4, data, len);
    o += 4 + len;
    return put32(o, esl_crc32(start, 4 + len));
}

/* A PNG of the screen (RGB, 8 bits), its rows from colour(x, y), with the image data in
   stored (not compressed) deflate blocks. Returns its length; out is malloc'd. */
static size_t make_png(void (*colour)(unsigned x, unsigned y, uint8_t *rgb), uint8_t **out)
{
    size_t row = 1 + 3 * W, raw = row * H;
    uint8_t *img = malloc(raw);
    for (unsigned y = 0; y < H; y++) {
        img[y * row] = 0; /* filter none */
        for (unsigned x = 0; x < W; x++)
            colour(x, y, img + y * row + 1 + 3 * x);
    }
    size_t blocks = (raw + 65534) / 65535, zlen = 2 + raw + 5 * blocks + 4;
    uint8_t *z = malloc(zlen), *o = z;
    *o++ = 0x78;
    *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i++) {
        a = (a + img[i]) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t at = 0; at < raw; at += 65535) {
        size_t n = raw - at < 65535 ? raw - at : 65535;
        *o++ = at + n == raw;
        *o++ = (uint8_t)n;
        *o++ = (uint8_t)(n >> 8);
        *o++ = (uint8_t)~n;
        *o++ = (uint8_t)(~n >> 8);
        memcpy(o, img + at, n);
        o += n;
    }
    put32(o, (b << 16) | a);
    uint8_t ihdr[13], *p = put32(ihdr, W);
    p = put32(p, H);
    p[0] = 8; /* bit depth */
    p[1] = 2; /* RGB */
    p[2] = p[3] = p[4] = 0;
    uint8_t *png = malloc(8 + 25 + 12 + zlen + 12);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    memcpy(png, sig, 8);
    o = chunk(png + 8, "IHDR", ihdr, 13);
    o = chunk(o, "IDAT", z, (uint32_t)zlen);
    o = chunk(o, "IEND", NULL, 0);
    free(img);
    free(z);
    *out = png;
    return (size_t)(o - png);
}

/* Four bands: yellow, red, black, white */
static void bands(unsigned x, unsigned y, uint8_t *rgb)
{
    (void)x;
    static const uint8_t c[4][3] = {{250, 240, 10}, {230, 20, 10}, {5, 5, 5}, {250, 250, 250}};
    memcpy(rgb, c[y / 32], 3);
}

static void test_picture(void)
{
    static uint8_t img[3 * PLANE];
    uint8_t *png;
    size_t len = make_png(bands, &png);

    /* black/white/red/yellow: each band in its own plane */
    mem_t m = {png, len};
    CHECK_INT(esl_image_picture(ESL_MODEL_BWRY, false, len, mem_read, &m, img), ESL_PIC_OK);
    const uint8_t *blk = img, *rd = img + PLANE, *yl = img + 2 * PLANE;
    bool ok = true;
    for (unsigned x = 0; x < W; x += 7)
        for (unsigned y = 0; y < H; y++) {
            unsigned band = y / 32;
            ok &= px(yl, x, y) == (band == 0) && px(rd, x, y) == (band == 1) && px(blk, x, y) == (band == 2);
        }
    CHECK(ok);

    /* black/white/yellow: no red plane, so the red band is drawn as the nearest of the others */
    m = (mem_t){png, len};
    CHECK_INT(esl_image_picture(ESL_MODEL_BWY, false, len, mem_read, &m, img), ESL_PIC_OK);
    CHECK(px(img + PLANE, 10, 10) && !px(img, 10, 10)); /* yellow band */
    CHECK(px(img, 10, 70) && !px(img + PLANE, 10, 70)); /* black band */

    /* dithered: still only one plane per pixel */
    m = (mem_t){png, len};
    CHECK_INT(esl_image_picture(ESL_MODEL_BWRY, true, len, mem_read, &m, img), ESL_PIC_OK);
    bool two = false;
    for (size_t i = 0; i < PLANE; i++)
        two |= (blk[i] & rd[i]) || (blk[i] & yl[i]) || (rd[i] & yl[i]);
    CHECK(!two);
    free(png);

    /* a raw image: refused with a pixel in two planes, whichever two */
    static uint8_t raw[3 * PLANE];
    for (unsigned k = 0; k < 3; k++) {
        memset(raw, 0, sizeof(raw));
        raw[(k == 2 ? 1 : 0) * PLANE + 5] = 0x10;
        raw[(k == 0 ? 1 : 2) * PLANE + 5] = 0x10;
        m = (mem_t){raw, sizeof(raw)};
        CHECK_INT(esl_image_picture(ESL_MODEL_BWRY, false, sizeof(raw), mem_read, &m, img), ESL_PIC_ERR_BOTH);
    }
    memset(raw, 0, sizeof(raw));
    raw[5] = 0x10;
    raw[PLANE + 6] = 0x10;
    raw[2 * PLANE + 7] = 0x10;
    m = (mem_t){raw, sizeof(raw)};
    CHECK_INT(esl_image_picture(ESL_MODEL_BWRY, false, sizeof(raw), mem_read, &m, img), ESL_PIC_OK);
    m = (mem_t){raw, 2 * PLANE};
    CHECK_INT(esl_image_picture(ESL_MODEL_BWRY, false, 2 * PLANE, mem_read, &m, img), ESL_PIC_ERR_FORMAT);
}

void test_yellow(void)
{
    test_draw();
    test_picture();
}
