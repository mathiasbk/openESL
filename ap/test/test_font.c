/* Font files (esl_font.c, #177): the file format, the table, layouts that draw with a font
   file, and /api/fonts */
#include "tinytest.h"
#include "font.h" /* tag/src: the built-in fonts */
#include "esl_api.h"
#include "esl_font.h"
#include "esl_render.h"
#include <stdio.h>
#include <stdlib.h>

#define AUTH "Bearer s3cret"

/* "A" is a block w columns wide and 10 rows high (2 bytes a column); "B" is not in the font */
static void font_json(char *buf, size_t cap, const char *name, unsigned w, unsigned height, const char *bitmap)
{
    snprintf(buf, cap,
             "{\"format\":\"esl-font\",\"version\":1,%s%s%s\"height\":%u,\"baseline\":8,\"first\":65,\"last\":66,"
             "\"widths\":[%u,0],\"bitmap\":\"%s\",\"source\":\"test\",\"_note\":1}",
             name ? "\"name\":\"" : "", name ? name : "", name ? "\"," : "", height, w, bitmap);
}

#define A4  "/8D/wP/A/8A="          /* 4 columns of FF C0: rows 0-9 */
#define A6  "/8D/wP/A/8D/wP/A"      /* 6 of them */
#define A4H "///w///w///w///w"      /* 4 columns of FF FF F0, for a font 20 rows high */

static void test_file(void)
{
    char js[512], err[ESL_FONT_ERR_MAX];
    esl_ufont_t *f;

    font_json(js, sizeof(js), "blk", 4, 10, A4);
    f = esl_font_from_json(js, strlen(js), NULL, err, sizeof(err));
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK_STR(esl_font_name(f), "blk");
    size_t bl;
    const uint8_t *b = esl_font_blob(f, &bl);
    CHECK_INT(bl, 6 + 3 + 1 + 4 + 2 + 8); /* header, name, source, widths, bitmap */

    /* written back as JSON, it reads as the same font */
    static char out_buf[1024];
    esl_sb_t sb;
    esl_sb_init(&sb, out_buf, sizeof(out_buf));
    esl_font_write_json(f, &sb);
    out_buf[sb.len] = '\0';
    CHECK(HAS(out_buf, "\"bitmap\":\"" A4 "\""));
    CHECK(HAS(out_buf, "\"widths\":[4,0]"));
    esl_ufont_t *g = esl_font_from_json(out_buf, sb.len, "blk", err, sizeof(err));
    CHECK(g != NULL);
    if (g) {
        size_t gl;
        const uint8_t *gb = esl_font_blob(g, &gl);
        CHECK_INT(gl, bl);
        CHECK_MEM(gb, b, bl);
    }
    esl_font_free(g);
    /* and from its blob, as at boot */
    g = esl_font_from_blob(b, bl);
    CHECK(g != NULL);
    esl_font_free(g);
    CHECK(esl_font_from_blob(b, bl - 1) == NULL);
    esl_font_free(f);

    /* the name comes from the URL when the file has none, and must match one it has */
    font_json(js, sizeof(js), NULL, 4, 10, A4);
    CHECK(esl_font_from_json(js, strlen(js), NULL, err, sizeof(err)) == NULL);
    CHECK(HAS(err, "name is missing"));
    f = esl_font_from_json(js, strlen(js), "x1", err, sizeof(err));
    CHECK(f != NULL && strcmp(esl_font_name(f), "x1") == 0);
    esl_font_free(f);
    font_json(js, sizeof(js), "blk", 4, 10, A4);
    CHECK(esl_font_from_json(js, strlen(js), "other", err, sizeof(err)) == NULL);
    CHECK(HAS(err, "not \"other\""));

    /* bad files */
    static const struct {
        const char *name;
        unsigned w, h;
        const char *bitmap;
        const char *msg;
    } bad[] = {
        {"medium", 4, 10, A4, "built-in"},   /* a built-in font's name */
        {"Big", 4, 10, A4, "name must be"},  /* capitals */
        {"blk", 4, 5, A4, "height must be"},
        {"blk", 4, 65, A4, "height must be"},
        {"blk", 6, 10, A4, "base64 of 12 bytes"}, /* the bitmap is too short for the widths */
        {"blk", 4, 10, A6, "base64 of 8 bytes"},  /* too long */
        {"blk", 4, 10, "/8D/wP/A/8A*", "base64"},
        {"blk", 0, 10, "", "no characters"},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        font_json(js, sizeof(js), bad[i].name, bad[i].w, bad[i].h, bad[i].bitmap);
        f = esl_font_from_json(js, strlen(js), NULL, err, sizeof(err));
        CHECK(f == NULL);
        if (f || !HAS(err, bad[i].msg))
            printf("  bad[%u]: %s\n", i, err);
        esl_font_free(f);
    }
    static const char *const worse[] = {
        "[]",
        "{\"format\":\"esl-layouts\",\"version\":1}",
        "{\"format\":\"esl-font\",\"version\":2}",
        "{\"format\":\"esl-font\",\"version\":1,\"name\":\"a\",\"height\":10,\"baseline\":8,\"first\":65,\"last\":66,"
        "\"widths\":[4],\"bitmap\":\"" A4 "\"}",                                       /* one width for two characters */
        "{\"format\":\"esl-font\",\"version\":1,\"name\":\"a\",\"height\":10,\"baseline\":11,\"first\":65,\"last\":65,"
        "\"widths\":[4],\"bitmap\":\"" A4 "\"}",                                       /* baseline below the font */
        "{\"format\":\"esl-font\",\"version\":1,\"name\":\"a\",\"height\":10,\"baseline\":8,\"first\":10,\"last\":10,"
        "\"widths\":[4],\"bitmap\":\"" A4 "\"}",                                       /* a control character */
        "{\"format\":\"esl-font\",\"version\":1,\"name\":\"a\",\"height\":10,\"baseline\":8,\"first\":65,\"last\":65,"
        "\"widths\":[4],\"bitmap\":\"" A4 "\",\"colour\":1}",                          /* an unknown key */
    };
    for (unsigned i = 0; i < sizeof(worse) / sizeof(worse[0]); i++) {
        f = esl_font_from_json(worse[i], strlen(worse[i]), NULL, err, sizeof(err));
        CHECK(f == NULL);
        esl_font_free(f);
    }
    CHECK(esl_font_name_ok("text_28-b", 9));
    CHECK(!esl_font_name_ok("small", 5) && !esl_font_name_ok("", 0) && !esl_font_name_ok("a/b", 3));
    CHECK(!esl_font_name_ok("abcdefghijklmnop", 16));
}

static esl_store_t st;
static esl_layouts_t ls;
static esl_api_t api = {.store = &st, .token = "s3cret", .fw_version = "0.1.0", .layouts = &ls};
static char resp[ESL_API_RESP_SIZE(4)];
static esl_sb_t out;

static int req(const char *method, const char *uri, const char *body)
{
    int s = esl_api_handle(&api, method, uri, AUTH, body, body ? strlen(body) : 0, 0, &out);
    resp[out.len < sizeof(resp) ? out.len : sizeof(resp) - 1] = '\0';
    return s;
}

/* What was saved (font_save) per slot */
static unsigned saves, erases;
static size_t saved_len[ESL_FONT_USER_MAX];
static bool refuse_save;
static bool font_save(void *ctx, unsigned slot, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (refuse_save && data)
        return false;
    if (data)
        saves++;
    else
        erases++;
    saved_len[slot] = data ? len : 0;
    return true;
}

static uint32_t layout_version(uint8_t model)
{
    uint32_t v = 0;
    esl_layouts_info(&ls, model, &v, NULL, NULL);
    return v;
}

static const char layout29[] =
    "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"f\",\"fields\":[\"a\"],"
    "\"elements\":[{\"type\":\"text\",\"field\":\"a\",\"x\":10,\"y\":10,\"w\":50,\"h\":12,\"fonts\":[\"blk\",\"small\"]}]}]}";

/* Pixel (x, y) of the black plane of a 2.9" image (column by column, 16 bytes each) */
static bool px(const uint8_t *img, unsigned x, unsigned y)
{
    return (img[x * 16 + y / 8] & (0x80 >> (y % 8))) != 0;
}

static unsigned ink(const uint8_t *img, size_t n)
{
    unsigned k = 0;
    for (size_t i = 0; i < n; i++)
        for (uint8_t m = 0x80; m; m >>= 1)
            k += (img[i] & m) != 0;
    return k;
}

static void test_api_and_layouts(void)
{
    static const esl_layout_ops_t ops = {.font_save = font_save};
    char js[512];
    static uint8_t img[296 * 16 * 2];
    esl_content_t c;

    esl_font_clear();
    esl_store_free(&st);
    esl_store_init(&st, 4, NULL, 0);
    esl_layouts_init(&ls, &ops);
    esl_sb_init(&out, resp, sizeof(resp));

    CHECK_INT(req("GET", "/api/fonts", NULL), 200);
    CHECK(HAS(resp, "\"max_fonts\":8") && HAS(resp, "\"bytes\":0") && HAS(resp, "\"fonts\":[]"));
    CHECK(HAS(resp, "{\"name\":\"small\",\"height\":17,\"baseline\":13,\"first\":32,\"last\":255}"));
    CHECK(HAS(resp, "{\"name\":\"price44\",\"height\":44"));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/fonts", NULL, NULL, 0, 0, &out), 401);
    CHECK_INT(req("GET", "/api/fonts/blk", NULL), 404);
    CHECK_INT(req("GET", "/api/fonts/small", NULL), 404);
    CHECK_INT(req("PUT", "/api/fonts/small", "{}"), 403);
    CHECK_INT(req("PUT", "/api/fonts/Bad", "{}"), 404);
    CHECK_INT(req("PUT", "/api/fonts", "{}"), 405);
    CHECK_INT(req("PUT", "/api/fonts/blk", NULL), 400);
    CHECK_INT(req("PUT", "/api/fonts/blk", "{\"format\":\"esl-font\"}"), 422);
    CHECK(HAS(resp, "version must be 1"));

    /* a layout cannot name a font the AP does not have */
    CHECK_INT(req("PUT", "/api/layouts/2.9", layout29), 422);
    CHECK(HAS(resp, "unknown font \\\"blk\\\""));

    font_json(js, sizeof(js), NULL, 4, 10, A4);
    CHECK_INT(req("PUT", "/api/fonts/blk", js), 200);
    CHECK(HAS(resp, "\"font\":{\"name\":\"blk\",\"height\":10,\"baseline\":8,\"first\":65,\"last\":66,\"characters\":1"));
    CHECK(HAS(resp, "\"replaced\":false,\"redrawn\":[]"));
    CHECK_INT(saves, 1);
    CHECK_INT(saved_len[0], 6 + 3 + 1 + 4 + 2 + 8);
    CHECK_INT(req("GET", "/api/fonts/blk", NULL), 200);
    CHECK(HAS(resp, "\"format\":\"esl-font\"") && HAS(resp, "\"bitmap\":\"" A4 "\""));
    CHECK_INT(req("GET", "/api/fonts/blk/", NULL), 200);
    CHECK_INT(req("GET", "/api/fonts", NULL), 200);
    CHECK(HAS(resp, "\"bytes\":24") && HAS(resp, "\"slot\":0,\"used_by\":[]"));

    /* a layout draws with it like with a built-in font */
    CHECK_INT(req("PUT", "/api/layouts/2.9", layout29), 200);
    uint32_t v1 = layout_version(ESL_MODEL_29);
    CHECK(v1 != 0);
    CHECK_INT(req("GET", "/api/fonts", NULL), 200);
    CHECK(HAS(resp, "\"used_by\":[\"2.9\"]"));
    esl_content_init_named(&c, "f", 1, 0);
    esl_content_add_field(&c, (const uint8_t *)"A", 1);
    size_t n = esl_layouts_render(&ls, ESL_MODEL_29, &c, img, sizeof(img));
    CHECK(n > 0);
    CHECK_INT(ink(img, 296 * 16), 40); /* the 4 x 10 block, nothing else */
    CHECK(px(img, 10, 10) && px(img, 13, 19) && !px(img, 14, 10) && !px(img, 10, 20) && !px(img, 9, 10));

    /* "B" is not in it: the next font (small) draws it */
    esl_content_init_named(&c, "f", 1, 0);
    esl_content_add_field(&c, (const uint8_t *)"B", 1);
    esl_layouts_render(&ls, ESL_MODEL_29, &c, img, sizeof(img));
    CHECK(ink(img, 296 * 16) != 40 && ink(img, 296 * 16) > 0);

    /* a new version of the font: the file is compiled again with a new version, so the tags
       are drawn again with it */
    font_json(js, sizeof(js), "blk", 6, 10, A6);
    CHECK_INT(req("PUT", "/api/fonts/blk", js), 200);
    CHECK(HAS(resp, "\"replaced\":true,\"redrawn\":[\"2.9\"],\"content_errors\":[]"));
    uint32_t v2 = layout_version(ESL_MODEL_29);
    CHECK(v2 != 0 && v2 != v1);
    esl_content_init_named(&c, "f", 1, 0);
    esl_content_add_field(&c, (const uint8_t *)"A", 1);
    esl_layouts_render(&ls, ESL_MODEL_29, &c, img, sizeof(img));
    CHECK_INT(ink(img, 296 * 16), 60);
    CHECK(px(img, 15, 10) && !px(img, 16, 10));
    /* the same font again changes nothing */
    CHECK_INT(req("PUT", "/api/fonts/blk", js), 200);
    CHECK_INT(layout_version(ESL_MODEL_29), v2);

    /* one that no longer fits the box (20 px in a 12 px box) is turned down, and nothing changes */
    saves = erases = 0;
    font_json(js, sizeof(js), "blk", 4, 20, A4H);
    CHECK_INT(req("PUT", "/api/fonts/blk", js), 422);
    CHECK(HAS(resp, "the layout file for 2.9 does not work with this font") && HAS(resp, "too small"));
    CHECK_INT(layout_version(ESL_MODEL_29), v2);
    CHECK_INT(saves, 2); /* saved, then the old one saved back */
    CHECK_INT(saved_len[0], 6 + 3 + 1 + 4 + 2 + 12);
    esl_layouts_render(&ls, ESL_MODEL_29, &c, img, sizeof(img));
    CHECK_INT(ink(img, 296 * 16), 60);

    /* no room to save it: nothing changes */
    refuse_save = true;
    font_json(js, sizeof(js), "blk", 4, 10, A4);
    CHECK_INT(req("PUT", "/api/fonts/blk", js), 507);
    refuse_save = false;
    CHECK_INT(layout_version(ESL_MODEL_29), v2);

    /* not deleted while a layout draws with it */
    CHECK_INT(req("DELETE", "/api/fonts/blk", NULL), 409);
    CHECK(HAS(resp, "the layout file for 2.9 draws with this font"));
    CHECK_INT(req("DELETE", "/api/fonts/nope", NULL), 404);

    /* at most ESL_FONT_USER_MAX of them */
    char name[16];
    for (unsigned i = 1; i < ESL_FONT_USER_MAX; i++) {
        snprintf(name, sizeof(name), "/api/fonts/f%u", i);
        font_json(js, sizeof(js), NULL, 4, 10, A4);
        CHECK_INT(req("PUT", name, js), 200);
    }
    CHECK_INT(req("PUT", "/api/fonts/one-too-many", js), 507);
    CHECK(HAS(resp, "at most 8 font files"));
    for (unsigned i = 1; i < ESL_FONT_USER_MAX; i++) {
        snprintf(name, sizeof(name), "/api/fonts/f%u", i);
        CHECK_INT(req("DELETE", name, NULL), 200);
    }

    /* restored at boot from what was saved, before the layouts: the same versions */
    size_t bl;
    int s = esl_font_slot_of("blk", 3);
    CHECK_INT(s, 0);
    const uint8_t *b = esl_font_blob(esl_font_slot(0), &bl);
    uint8_t *copy = malloc(bl);
    memcpy(copy, b, bl);
    size_t jl;
    esl_sb_init(&out, resp, sizeof(resp));
    esl_layouts_write(&ls, ESL_MODEL_29, &out); /* the JSON, for the restore below */
    jl = out.len;
    char *lj = malloc(jl);
    memcpy(lj, resp, jl);
    esl_font_clear();
    esl_layouts_init(&ls, NULL);
    CHECK(!esl_layouts_restore(&ls, ESL_MODEL_29, lj, jl)); /* no font yet */
    CHECK(esl_layouts_font_restore(&ls, 0, copy, bl));
    CHECK(!esl_layouts_font_restore(&ls, 0, copy, bl)); /* slot in use */
    CHECK(!esl_layouts_font_restore(&ls, 1, copy, bl)); /* the same name twice */
    CHECK(!esl_layouts_font_restore(&ls, ESL_FONT_USER_MAX, copy, bl));
    CHECK(esl_layouts_restore(&ls, ESL_MODEL_29, lj, jl));
    CHECK_INT(layout_version(ESL_MODEL_29), v2);
    free(copy);
    free(lj);

    /* deleted once no layout uses it */
    esl_layouts_init(&ls, &ops);
    erases = 0;
    CHECK_INT(req("DELETE", "/api/fonts/blk", NULL), 200);
    CHECK(HAS(resp, "\"deleted\":true"));
    CHECK_INT(erases, 1);
    CHECK_INT(req("GET", "/api/fonts/blk", NULL), 404);
    CHECK(esl_font_bytes() == 0);

    esl_layouts_free(&ls);
    esl_store_free(&st);
    esl_font_clear();
}

/* All font files together take at most ESL_FONT_MAX_BYTES */
static void test_bytes(void)
{
    static esl_layouts_t l2;
    esl_layouts_init(&l2, NULL);
    esl_font_clear();
    /* 224 characters of 20 columns at 8 bytes a column: 35 KB, more than all may take */
    size_t cap = 64 * 1024;
    char *js = malloc(cap), err[ESL_FONT_ERR_MAX];
    size_t bits = 224 * 20 * 8, pos;
    pos = (size_t)snprintf(js, cap, "{\"format\":\"esl-font\",\"version\":1,\"name\":\"big\",\"height\":64,"
                                    "\"baseline\":50,\"first\":32,\"last\":255,\"widths\":[");
    for (unsigned i = 0; i < 224; i++)
        pos += (size_t)snprintf(js + pos, cap - pos, i ? ",20" : "20");
    pos += (size_t)snprintf(js + pos, cap - pos, "],\"bitmap\":\"");
    for (size_t i = 0; i < (bits * 4 + 2) / 3; i++) /* base64 of bits zero bytes */
        js[pos++] = 'A';
    while (pos % 4 != (size_t)(strstr(js, "\"bitmap\":\"") + 10 - js) % 4)
        js[pos++] = '=';
    js[pos] = '\0';
    strcat(js, "\"}");
    esl_ufont_t *f = esl_font_from_json(js, strlen(js), NULL, err, sizeof(err));
    CHECK(f != NULL);
    if (!f)
        printf("  %s\n", err);
    if (f) {
        unsigned models;
        bool replaced;
        CHECK_INT(esl_layouts_font_put(&l2, f, &models, &replaced, err, sizeof(err)), 507);
        CHECK(HAS(err, "may take 32768 bytes together"));
    }
    CHECK(esl_font_bytes() == 0);
    free(js);
    esl_layouts_free(&l2);
}

/* A font file made from a built-in font draws exactly as the built-in font */
static void test_same_as_builtin(void)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static char js[16384], lay[1024], err[ESL_LAYOUT_ERR_MAX];
    static uint8_t a[296 * 16 * 2], b[296 * 16 * 2];
    static esl_layout_bin_t bin;
    const font_t *m = &font_mid;
    size_t n = (size_t)(m->last - m->first) + 1, bits = 0, pos;
    esl_font_clear();
    pos = (size_t)snprintf(js, sizeof(js), "{\"format\":\"esl-font\",\"version\":1,\"name\":\"mid2\",\"height\":%u,"
                           "\"baseline\":%u,\"first\":%u,\"last\":%u,\"widths\":[", m->height, m->baseline, m->first, m->last);
    for (size_t i = 0; i < n; i++) {
        pos += (size_t)snprintf(js + pos, sizeof(js) - pos, i ? ",%u" : "%u", m->widths[i]);
        bits += (size_t)m->widths[i] * m->bytes_per_col;
    }
    pos += (size_t)snprintf(js + pos, sizeof(js) - pos, "],\"bitmap\":\"");
    for (size_t i = 0; i < bits; i += 3) {
        uint32_t v = (uint32_t)m->bitmap[i] << 16 | (i + 1 < bits ? (uint32_t)m->bitmap[i + 1] << 8 : 0) |
                     (i + 2 < bits ? m->bitmap[i + 2] : 0);
        js[pos++] = abc[v >> 18];
        js[pos++] = abc[(v >> 12) & 63];
        js[pos++] = i + 1 < bits ? abc[(v >> 6) & 63] : '=';
        js[pos++] = i + 2 < bits ? abc[v & 63] : '=';
    }
    snprintf(js + pos, sizeof(js) - pos, "\"}");
    esl_ufont_t *f = esl_font_from_json(js, strlen(js), NULL, err, sizeof(err));
    CHECK(f != NULL);
    if (!f)
        return;
    esl_font_swap(0, f);
    static const char *const fonts[] = {"mid", "mid2"};
    uint8_t *img[] = {a, b};
    /* turned text too (#295): a font file is drawn by its own tables in every direction */
    for (unsigned rot = 0; rot < 4; rot++) {
        for (unsigned k = 0; k < 2; k++) {
            unsigned w = rot & 1 ? 40 : 280, h = rot & 1 ? 120 : 40;
            snprintf(lay, sizeof(lay),
                     "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"p\",\"fields\":[\"price\"],"
                     "\"elements\":[{\"type\":\"text\",\"field\":\"price\",\"x\":4,\"y\":4,\"w\":%u,\"h\":%u,\"fonts\":[\"%s\"],"
                     "\"align\":\"right\",\"decimals\":\"dash\",\"thousands\":true,\"rotate\":%u}]}]}",
                     w, h, fonts[k], rot * 90);
            CHECK(esl_layout_compile(lay, strlen(lay), ESL_MODEL_29, &bin, err, sizeof(err)));
            esl_content_t c;
            esl_content_init_named(&c, "p", 1, 0);
            esl_content_add_field(&c, (const uint8_t *)(rot & 1 ? "99.00" : "12499.00"), rot & 1 ? 5 : 8);
            memset(img[k], 0, sizeof(a));
            CHECK(esl_render(ESL_MODEL_29, &bin, &c, img[k], sizeof(a)) > 0);
        }
        CHECK(ink(a, 296 * 16) > 100);
        CHECK_MEM(a, b, sizeof(a));
    }
    esl_font_clear();
}

void test_font(void)
{
    test_same_as_builtin();
    test_file();
    test_api_and_layouts();
    test_bytes();
}
