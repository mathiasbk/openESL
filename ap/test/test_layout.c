#include "tinytest.h"
#include "esl_crc32.h"
#include "esl_layout.h"
#include "esl_render.h"
#include <stdlib.h>

/* Layout files: the compiler (JSON -> the binary the AP draws with, krav A13) and
 * the storage per model. The tests use fixed copies of the example files
 * (ap/test/layouts/, two layouts each) so their sizes do not change when the
 * examples grow; the files in tag/designs/layouts/ are compiled too, so they
 * always stay valid. */

typedef struct {
    int depth, locks, saves, erases;
    uint8_t last_model;
    size_t last_len;
} fake_t;

static void f_lock(void *ctx)
{
    fake_t *f = ctx;
    CHECK_INT(f->depth, 0);
    f->depth++;
    f->locks++;
}

static void f_unlock(void *ctx)
{
    ((fake_t *)ctx)->depth--;
}

static void f_save(void *ctx, uint8_t model, const char *data, size_t len)
{
    fake_t *f = ctx;
    (void)data;
    CHECK_INT(f->depth, 0); /* persistence happens outside the lock */
    f->saves++;
    f->last_model = model;
    f->last_len = len;
}

static void f_erase(void *ctx, uint8_t model)
{
    fake_t *f = ctx;
    CHECK_INT(f->depth, 0);
    f->erases++;
    f->last_model = model;
}

static esl_layout_bin_t bin;
static char err[ESL_LAYOUT_ERR_MAX];

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = malloc(ESL_LAYOUT_MAX_SIZE + 2); /* room for a NUL */
    *len = buf ? fread(buf, 1, ESL_LAYOUT_MAX_SIZE + 1, f) : 0;
    fclose(f);
    return buf;
}

/* Reads ap/test/layouts/<name> (the tests run from ap/test). NULL if missing. */
char *read_example_layout(const char *name, size_t *len)
{
    char path[256];
    snprintf(path, sizeof(path), "layouts/%s", name);
    return read_file(path, len);
}

static bool compile(const char *js, uint8_t model)
{
    err[0] = '\0';
    return esl_layout_compile(js, strlen(js), model, &bin, err, sizeof(err));
}

/* A 2.9" file with one layout "L" (fields a, b) and the given elements. */
static bool compile_el(const char *elements)
{
    static char js[4096];
    snprintf(js, sizeof(js),
             "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"l\","
             "\"fields\":[\"a\",\"b\"],\"elements\":[%s]}]}",
             elements);
    return compile(js, ESL_MODEL_29);
}

/* A text element without h, field and fonts; TEXT_A is a complete one. */
#define TEXT_XYW "{\"type\":\"text\",\"x\":0,\"y\":0,\"w\":100"
#define TEXT_A TEXT_XYW ",\"h\":30,\"field\":\"a\",\"fonts\":[\"medium\"]"

static void test_examples(void)
{
    static const struct {
        const char *file;
        uint8_t model;
    } ex[] = {{"2.9.json", ESL_MODEL_29}, {"1.54.json", ESL_MODEL_154}};

    for (unsigned i = 0; i < 2; i++) {
        size_t len;
        char *js = read_example_layout(ex[i].file, &len);
        CHECK(js != NULL);
        if (!js) {
            printf("  missing ap/test/layouts/%s\n", ex[i].file);
            continue;
        }
        bool ok = esl_layout_compile(js, len, ex[i].model, &bin, err, sizeof(err));
        if (!ok)
            printf("  %s: %s\n", ex[i].file, err);
        CHECK(ok);
        CHECK_INT(bin.count, 2);
        CHECK_STR(bin.names[0], "shelf");
        CHECK_STR(bin.names[1], "sign");
        CHECK_INT(bin.nfields[0], 4);
        CHECK_INT(bin.nfields[1], 2);
        CHECK_INT(bin.size, ESL_LBIN_HDR_LEN + 1 + 5 * ESL_LE_SIZE + 1 + 7 * ESL_LE_SIZE);
        CHECK_INT(bin.bin[0], ESL_LBIN_FORMAT);
        CHECK_INT(bin.bin[1], ex[i].model);
        CHECK_INT(bin.bin[2], 2);
        CHECK_INT(bin.version, esl_crc32(bin.bin, bin.size));
        CHECK_INT(esl_layout_bin_find(&bin, "sign", 4), 1);
        CHECK_INT(esl_layout_bin_find(&bin, "shelf", 5), 0);
        CHECK_INT(esl_layout_bin_find(&bin, "nope", 4), -1);

        /* the other model's screen is checked too: a 1.54" file is not a 2.9" file */
        uint8_t other = ex[i].model == ESL_MODEL_29 ? ESL_MODEL_154 : ESL_MODEL_29;
        CHECK(!esl_layout_compile(js, len, other, &bin, err, sizeof(err)));
        CHECK(HAS(err, "model must be"));
        free(js);

        /* the shipped file: compiles for its model, starts with the same two layouts */
        char path[256];
        snprintf(path, sizeof(path), "../../tag/designs/layouts/%s", ex[i].file);
        js = read_file(path, &len);
        CHECK(js != NULL);
        if (!js) {
            printf("  missing %s\n", path);
            continue;
        }
        ok = esl_layout_compile(js, len, ex[i].model, &bin, err, sizeof(err));
        if (!ok)
            printf("  %s: %s\n", path, err);
        CHECK(ok);
        CHECK(bin.count >= 2);
        CHECK_STR(bin.names[0], "shelf");
        CHECK_STR(bin.names[1], "sign");
        CHECK_INT(bin.bin[2], bin.count);
        CHECK_INT(bin.version, esl_crc32(bin.bin, bin.size));
        CHECK(!esl_layout_compile(js, len, other, &bin, err, sizeof(err)));
        free(js);
    }
}

static void test_binary(void)
{
    /* one text and one rect element, every option set, field by index */
    CHECK(compile_el("{\"type\":\"text\",\"field\":1,\"x\":260,\"y\":5,\"w\":36,\"h\":120,"
                     "\"fonts\":[\"large\",\"mid\",\"small\"],\"align\":\"right\",\"valign\":\"bottom\","
                     "\"lines\":2,\"color\":\"highlight\",\"_note\":\"ignored\"},"
                     "{\"type\":\"rect\",\"x\":0,\"y\":125,\"w\":296,\"h\":3,\"color\":\"frame\"}"));
    static const uint8_t expect[] = {
        ESL_LBIN_FORMAT, ESL_MODEL_29, 1, 0, 0, 2,
        ESL_LE_TEXT, 1, ESL_FONT_LARGE, ESL_FONT_MID, ESL_FONT_SMALL, ESL_LE_RIGHT | ESL_LE_BOTTOM, 2,
        ESL_LE_PRICE_HL, 4, 1, 5, 36, 0, 120,
        ESL_LE_RECT, 0, 0xFF, 0xFF, 0xFF, 0, 0, ESL_LE_FRAME, 0, 0, 125, 0x28, 1, 3};
    CHECK_INT(bin.size, sizeof(expect));
    CHECK_MEM(bin.bin, expect, sizeof(expect));
    CHECK_STR(bin.names[0], "l");
    CHECK_INT(bin.nfields[0], 2);

    /* defaults: left, top, lines 0, black */
    CHECK(compile_el(TEXT_A "}"));
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_OPTS], 0);
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_LINES], 0);
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_COLOR], ESL_LE_BLACK);
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_FONT + 1], ESL_FONT_NONE);

    /* number format in the high bits of opts */
    CHECK(compile_el(TEXT_A ",\"align\":\"right\",\"decimals\":2,\"thousands\":true}"));
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_OPTS], ESL_LE_RIGHT | ESL_LE_NUM_2 | ESL_LE_NUM_THOUSANDS);
    CHECK(compile_el(TEXT_A ",\"decimals\":0}"));
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_OPTS], ESL_LE_NUM_0);
    CHECK(compile_el(TEXT_A ",\"decimals\":\"auto\",\"thousands\":false}"));
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_OPTS], ESL_LE_NUM_AUTO);
    CHECK(compile_el(TEXT_A ",\"decimals\":\"dash\"}"));
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_OPTS], ESL_LE_NUM_DASH);
    CHECK(compile_el(TEXT_A ",\"decimals\":\"0-dash\",\"thousands\":true}"));
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_OPTS], ESL_LE_NUM_0_DASH | ESL_LE_NUM_THOUSANDS);
    CHECK(compile_el(TEXT_A ",\"decimals\":\"keep\"}"));
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_OPTS], ESL_LE_NUM_KEEP);
    CHECK(compile_el(TEXT_A ",\"thousands\":true}"));
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_OPTS], ESL_LE_NUM_KEEP | ESL_LE_NUM_THOUSANDS);

    /* a barcode element: symbology in the first font byte, module width in lines */
    CHECK(compile_el("{\"type\":\"barcode\",\"field\":\"b\",\"symbology\":\"ean13\",\"module\":2,"
                     "\"x\":10,\"y\":70,\"w\":226,\"h\":40,\"align\":\"center\"}"));
    static const uint8_t expect_bc[] = {
        ESL_LBIN_FORMAT, ESL_MODEL_29, 1, 0, 0, 1,
        ESL_LE_BARCODE, 1, ESL_BC_EAN13, 0xFF, 0xFF, ESL_LE_CENTER, 2, ESL_LE_BLACK, 10, 0, 70, 226, 0, 40};
    CHECK_INT(bin.size, sizeof(expect_bc));
    CHECK_MEM(bin.bin, expect_bc, sizeof(expect_bc));
    CHECK_INT(bin.bc_field[0], 1);
    CHECK_INT(bin.bc_sym[0], ESL_BC_EAN13);
    CHECK_INT(bin.bc_max[0], 113);
    CHECK(compile_el("{\"type\":\"barcode\",\"field\":0,\"symbology\":\"code128\",\"x\":0,\"y\":0,\"w\":296,\"h\":10}"));
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_SYM], ESL_BC_CODE128);
    CHECK_INT(bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_MODULE], 0);
    CHECK_INT(bin.bc_max[0], 296);
    CHECK(compile_el(TEXT_A "}"));
    CHECK_INT(bin.bc_field[0], -1);

    /* the 1.54" is 200 x 200 */
    CHECK(compile("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"1.54\",\"layouts\":[{\"name\":\"x\","
                  "\"fields\":[\"a\"],\"elements\":[{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":200,\"h\":200}]}]}",
                  ESL_MODEL_154));
    CHECK_INT(bin.bin[1], ESL_MODEL_154);
}

/* The element list must fail with a message containing want. */
static void bad_el(const char *elements, const char *want)
{
    bool ok = compile_el(elements);
    CHECK(!ok);
    if (!ok && !HAS(err, want))
        printf("  message \"%s\" lacks \"%s\"\n", err, want);
    CHECK(HAS(err, want));
    CHECK_INT(bin.version, 0);
}

static void bad_file(const char *js, uint8_t model, const char *want)
{
    bool ok = compile(js, model);
    CHECK(!ok);
    if (!ok && !HAS(err, want))
        printf("  message \"%s\" lacks \"%s\"\n", err, want);
    CHECK(HAS(err, want));
}

static void test_errors(void)
{
    /* number format */
    bad_el(TEXT_A ",\"decimals\":1}", "decimals must be 0, 2");
    bad_el(TEXT_A ",\"decimals\":\"two\"}", "decimals must be 0, 2");
    bad_el(TEXT_A ",\"thousands\":1}", "thousands must be true or false");
    {
        static char els[2048];
        size_t n = 0;
        for (int i = 0; i <= ESL_DRAW_MAX_NUMS; i++)
            n += (size_t)snprintf(els + n, sizeof(els) - n, "%s" TEXT_A ",\"decimals\":2}", i ? "," : "");
        bad_el(els, "at most 8 numbers");
        els[strrchr(els, '{') - els - 1] = '\0'; /* one fewer */
        CHECK(compile_el(els));
    }
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":10,\"decimals\":2}",
           "decimals is only for text elements");
    bad_el("{\"type\":\"barcode\",\"field\":\"b\",\"symbology\":\"code128\",\"x\":0,\"y\":0,"
           "\"w\":296,\"h\":10,\"thousands\":true}", "thousands is only for text elements");

    /* the whole file */
    bad_file("{\"format\":", ESL_MODEL_29, "invalid JSON");
    bad_file("[]", ESL_MODEL_29, "must be a JSON object");
    bad_file("{}", ESL_MODEL_29, "format must be \"esl-layouts\"");
    bad_file("{\"format\":\"esl-layouts\",\"version\":2}", ESL_MODEL_29, "version must be 1");
    bad_file("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"1.54\"}", ESL_MODEL_29, "model must be \"2.9\"");
    bad_file("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[]}", ESL_MODEL_29,
             "1 to 16 layouts");
    bad_file("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[],\"extra\":1}",
             ESL_MODEL_29, "unknown key \"extra\"");
    bad_file("{}", ESL_MODEL_UNKNOWN, "unknown model");

    /* a layout */
    bad_file("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"price\","
             "\"fields\":[\"a\"],\"elements\":[]}]}", ESL_MODEL_29, "layouts[0]: name must be");
    bad_file("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"Big\","
             "\"fields\":[\"a\"],\"elements\":[]}]}", ESL_MODEL_29, "name must be");
    bad_file("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"a\","
             "\"fields\":[\"a\"],\"elements\":[]}]}", ESL_MODEL_29, "elements must be a list");
    bad_file("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"a\","
             "\"fields\":[],\"elements\":[]}]}", ESL_MODEL_29, "fields must list");
    bad_file("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"a\","
             "\"fields\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\"],\"elements\":[]}]}", ESL_MODEL_29, "fields must list");
    bad_file("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"a\","
             "\"fields\":[\"a\",\"a\"],\"elements\":[]}]}", ESL_MODEL_29, "listed twice");
    bad_file("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"a\","
             "\"fields\":[\"Price\"],\"elements\":[]}]}", ESL_MODEL_29, "field names must be");
    bad_file("{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":["
             "{\"name\":\"a\",\"fields\":[\"a\"],\"elements\":[{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":1,\"h\":1}]},"
             "{\"name\":\"a\",\"fields\":[\"a\"],\"elements\":[{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":1,\"h\":1}]}]}",
             ESL_MODEL_29, "layouts[1]: the name \"a\" is used twice");

    /* elements: the message names the element */
    bad_el("1", "layouts[0].elements[0]: must be an object");
    bad_el("{\"type\":\"circle\"}", "type must be");
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10}", "h must be a whole number");
    bad_el("{\"type\":\"rect\",\"x\":-1,\"y\":0,\"w\":10,\"h\":1}", "x must be a whole number");
    bad_el("{\"type\":\"rect\",\"x\":1.5,\"y\":0,\"w\":10,\"h\":1}", "x must be a whole number");
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":0,\"h\":1}", "at least 1");
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":1},{\"type\":\"rect\",\"x\":200,\"y\":0,"
           "\"w\":100,\"h\":1}",
           "layouts[0].elements[1]: x + w = 300 is past the right edge of the screen (296)");
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":1,\"x\":5}", "key \"x\" appears twice");
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":100,\"w\":10,\"h\":29}", "y + h = 129 is past the bottom");
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":1,\"colour\":\"black\"}", "unknown key \"colour\"");
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":1,\"color\":\"pink\"}", "color must be");
    /* the 2.9" has no red: the AP refuses red before it looks at anything else */
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":1,\"color\":\"red\"}", "black/white model");
    bad_el(TEXT_XYW ",\"h\":40,\"field\":\"a\",\"fonts\":[\"medium\"],\"color\":\"red_invert\"}", "black/white model");
    CHECK(!esl_model_has_red(ESL_MODEL_29) && !esl_model_has_red(ESL_MODEL_154) && !esl_model_has_red(0));
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":1,\"color\":\"invert\"}", "a rect can be");
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":1,\"field\":\"a\"}", "field is only for text");
    bad_el(TEXT_XYW ",\"h\":30,\"fonts\":[\"medium\"],\"field\":\"c\"}", "field \"c\" is not in the layout's fields");
    bad_el(TEXT_XYW ",\"h\":30,\"fonts\":[\"medium\"],\"field\":2}", "field must be one of");
    bad_el(TEXT_XYW ",\"h\":30,\"field\":\"a\",\"fonts\":[]}", "fonts must be a list of 1 to 3");
    bad_el(TEXT_XYW ",\"h\":30,\"field\":\"a\",\"fonts\":[\"small\",\"small\",\"small\",\"small\"]}",
           "fonts must be a list of 1 to 3");
    bad_el(TEXT_XYW ",\"h\":30,\"field\":\"a\",\"fonts\":[\"huge\"]}", "unknown font");
    bad_el(TEXT_A ",\"align\":\"justify\"}", "align must be");
    bad_el(TEXT_A ",\"valign\":\"center\"}", "valign must be");
    bad_el(TEXT_A ",\"lines\":4}", "lines must be");
    bad_el(TEXT_A ",\"rotate\":45}", "rotate must be 0, 90, 180 or 270");
    bad_el(TEXT_A ",\"rotate\":360}", "rotate must be 0, 90, 180 or 270");
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":1,\"rotate\":90}", "rotate is only for text");
    /* turned, the lines are stacked across w: 60 px high fits the font, 10 px wide does not */
    CHECK(compile_el("{\"type\":\"text\",\"x\":0,\"y\":0,\"w\":10,\"h\":60,\"field\":\"a\",\"fonts\":[\"medium\"]}"));
    bad_el("{\"type\":\"text\",\"x\":0,\"y\":0,\"w\":10,\"h\":60,\"field\":\"a\",\"fonts\":[\"medium\"],\"rotate\":90}",
           "the box (10x60, turned) is too small for its smallest font (24 px");
    bad_el(TEXT_XYW ",\"h\":20,\"field\":\"a\",\"fonts\":[\"medium\"]}", "too small for its smallest font (24 px");
    bad_el(TEXT_A ",\"color\":\"invert\"}", "border of an inverted field");
    /* the AP's scene: 32 text lines and 20 rectangles per screen (esl_render.c) */
#define TALL TEXT_XYW ",\"h\":128,\"field\":\"a\",\"fonts\":[\"small\"]"
#define T5 TALL "}," TALL "}," TALL "}," TALL "}," TALL "}"
#define R1 "{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":1,\"h\":1}"
#define R5 R1 "," R1 "," R1 "," R1 "," R1
#define R10 R5 "," R5
#define HIGHLIGHT TEXT_XYW ",\"h\":40,\"field\":\"a\",\"fonts\":[\"medium\"],\"color\":\"highlight\"}"
    bad_el(T5 "," T5 "," TALL ",\"lines\":3}",
           "layouts[0]: needs up to 33 lines of text, the AP draws at most 32");
    bad_el(R10 "," R10 "," HIGHLIGHT, "needs 21 rectangles");
    bad_el(R10 "," R10 "," R1, "needs 21 rectangles");
    /* 19 rects + one inverted field, and 10 * 3 + 2 lines, are exactly the limits */
    CHECK(compile_el(R10 "," R5 "," R1 "," R1 "," R1 "," R1 "," HIGHLIGHT));
    CHECK(compile_el(T5 "," T5 "," TALL ",\"lines\":2}"));
#undef HIGHLIGHT
#undef R10
#undef R5
#undef R1
#undef T5
#undef TALL

    /* the two extra sizes: text20 needs 20 px, price44 44 px */
    CHECK(compile_el(TEXT_XYW ",\"h\":20,\"field\":\"a\",\"fonts\":[\"text20\",\"medium\"]}"));
    CHECK(compile_el(TEXT_XYW ",\"h\":44,\"field\":\"a\",\"fonts\":[\"price44\",\"mid\"]}"));
    bad_el(TEXT_XYW ",\"h\":30,\"field\":\"a\",\"fonts\":[\"price44\"]}", "too small for its smallest font (44 px");
    bad_el(TEXT_XYW ",\"h\":16,\"field\":\"a\",\"fonts\":[\"text20\"]}", "too small for its smallest font (20 px");

    /* fixed texts: stored once at the end of the file, the element points at them */
    CHECK(compile_el(TEXT_XYW ",\"h\":30,\"text\":\"inkl. mva\",\"fonts\":[\"small\"]},"
                     TEXT_XYW ",\"h\":30,\"text\":\"kr\",\"fonts\":[\"small\"]},"
                     TEXT_XYW ",\"h\":30,\"text\":\"inkl. mva\",\"fonts\":[\"small\"]},"
                     TEXT_A "}"));
    {
        const uint8_t *b = bin.bin;
        size_t els = ESL_LBIN_HDR_LEN + 1 + 4 * ESL_LE_SIZE;
        CHECK_INT(b[ESL_LBIN_OFF_NTEXT], 2);
        CHECK_INT(b[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_FIELD], ESL_LE_FIELD_FIXED | 0);
        CHECK_INT(b[ESL_LBIN_HDR_LEN + 1 + ESL_LE_SIZE + ESL_LE_OFF_FIELD], ESL_LE_FIELD_FIXED | 1);
        CHECK_INT(b[ESL_LBIN_HDR_LEN + 1 + 2 * ESL_LE_SIZE + ESL_LE_OFF_FIELD], ESL_LE_FIELD_FIXED | 0);
        CHECK_INT(b[ESL_LBIN_HDR_LEN + 1 + 3 * ESL_LE_SIZE + ESL_LE_OFF_FIELD], 0);
        CHECK_INT(bin.size, els + 1 + 9 + 1 + 2);
        CHECK_INT(b[els], 9);
        CHECK_MEM(&b[els + 1], "inkl. mva", 9);
        CHECK_INT(b[els + 10], 2);
        CHECK_MEM(&b[els + 11], "kr", 2);
    }
    /* UTF-8 becomes Latin-1, as for content */
    CHECK(compile_el(TEXT_XYW ",\"h\":30,\"text\":\"Pris \u00e6\u00f8\u00e5 \u2013 kr\",\"fonts\":[\"small\"]}"));
    CHECK_MEM(&bin.bin[ESL_LBIN_HDR_LEN + 1 + ESL_LE_SIZE + 1], "Pris \xe6\xf8\xe5 - kr", 12);
    bad_el(TEXT_XYW ",\"h\":30,\"text\":\"\",\"fonts\":[\"small\"]}", "text must not be empty");
    bad_el(TEXT_XYW ",\"h\":30,\"text\":1,\"fonts\":[\"small\"]}", "text must be a string");
    bad_el(TEXT_A ",\"text\":\"x\"}", "either field or text");
    bad_el(TEXT_XYW ",\"h\":30,\"fonts\":[\"small\"],\"text\":"
           "\"12345678901234567890123456789012345678901234567890123456789012345\"}", "longer than 64 bytes");
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":1,\"h\":1,\"text\":\"x\"}", "text is only for text");
    bad_el("{\"type\":\"barcode\",\"x\":0,\"y\":0,\"w\":296,\"h\":40,\"symbology\":\"code128\",\"text\":\"x\"}",
           "a barcode takes a field");
    bad_el("{\"type\":\"text\",\"x\":0,\"y\":0,\"w\":100,\"h\":30,\"fonts\":[\"small\"]}",
           "needs a field or a fixed text");

    /* barcodes */
#define BC "{\"type\":\"barcode\",\"x\":0,\"y\":0,\"w\":296,\"h\":40,\"field\":\"a\""
    bad_el(BC "}", "symbology must be code128 or ean13");
    bad_el(BC ",\"symbology\":\"qr\"}", "symbology must be");
    bad_el(BC ",\"symbology\":\"code128\",\"module\":5}", "module must be 0");
    bad_el(BC ",\"symbology\":\"code128\",\"fonts\":[\"small\"]}", "fonts is only for text");
    bad_el(BC ",\"symbology\":\"code128\",\"color\":\"white\"}", "a barcode is black");
    bad_el("{\"type\":\"barcode\",\"x\":0,\"y\":0,\"w\":296,\"h\":40,\"symbology\":\"code128\"}",
           "a barcode element needs a field");
    bad_el("{\"type\":\"barcode\",\"x\":0,\"y\":0,\"w\":296,\"h\":9,\"field\":\"a\",\"symbology\":\"code128\"}",
           "at least 10 px high");
    bad_el(BC ",\"symbology\":\"ean13\",\"module\":3}", "w must be at least 339");
    bad_el(BC ",\"symbology\":\"code128\"}," BC ",\"symbology\":\"code128\"}", "at most 1 barcode");
    bad_el(TEXT_A ",\"module\":1}", "module is only for barcode");
#undef BC

    /* too big: two 128x128 icons are 2 x 2051 bytes compiled */
    static char js[ESL_LAYOUT_MAX_SIZE + 1];
    char data[2800];
    size_t k = 0;
    for (; k < 2048 / 3 * 4; k++)
        data[k] = 'A';
    memcpy(&data[k], "AAA=", 5);
    snprintf(js, sizeof(js),
             "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"icons\":{"
             "\"a\":{\"w\":128,\"h\":128,\"data\":\"%s\"},\"b\":{\"w\":128,\"h\":128,\"data\":\"%s\"}},"
             "\"layouts\":[{\"name\":\"l\",\"fields\":[\"a\"],\"elements\":[" TEXT_A "}]}]}",
             data, data);
    CHECK(!compile(js, ESL_MODEL_29));
    CHECK(HAS(err, "the icons are too big: at most 4096 bytes compiled"));
}

/* A 2.9" file with the given icons and one layout "l" (fields a, b) with the given elements. */
static bool compile_icons(const char *icons, const char *elements)
{
    static char js[4096];
    snprintf(js, sizeof(js),
             "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"icons\":{%s},\"layouts\":[{\"name\":\"l\","
             "\"fields\":[\"a\",\"b\"],\"elements\":[%s]}]}",
             icons, elements);
    return compile(js, ESL_MODEL_29);
}

/* Format 2: lines, icons, rects with round corners or only a border */
static void test_shapes(void)
{
    const uint8_t *e = &bin.bin[ESL_LBIN_HDR_LEN + 1];

    /* a line runs corner to corner in its box */
    CHECK(compile_el("{\"type\":\"line\",\"x\":20,\"y\":10,\"w\":31,\"h\":31,\"direction\":\"up\",\"width\":3}"));
    static const uint8_t expect_line[] = {ESL_LE_LINE, 0, 0xFF, 0xFF, 0xFF, ESL_LE_RISING, 3, ESL_LE_BLACK, 20, 0, 10, 31, 0, 31};
    CHECK_MEM(e, expect_line, sizeof(expect_line));
    CHECK(compile_el("{\"type\":\"line\",\"x\":20,\"y\":10,\"w\":31,\"h\":31}"));
    CHECK_INT(e[ESL_LE_OFF_OPTS], 0);
    CHECK_INT(e[ESL_LE_OFF_WIDTH], 1);
    CHECK(compile_el("{\"type\":\"line\",\"x\":0,\"y\":5,\"w\":296,\"h\":1,\"direction\":\"up\",\"color\":\"white\"}"));
    CHECK_INT(e[ESL_LE_OFF_OPTS], 0); /* straight: no direction */
    CHECK_INT(ESL_GET_U16(e + ESL_LE_OFF_W), 296);
    CHECK_INT(e[ESL_LE_OFF_H], 1);

    /* a plain rect compiles as in format 1; radius and border in lines and opts */
    CHECK(compile_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":40,\"h\":20,\"radius\":6,\"border\":2}"));
    CHECK_INT(e[ESL_LE_OFF_RADIUS], 6);
    CHECK_INT(e[ESL_LE_OFF_BORDER], 2);

    /* an icon: stored column by column after the fixed texts, the element points at it */
    CHECK(compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA=\"},\"bar\":{\"w\":9,\"h\":1,\"data\":\"/4A=\"}",
                        "{\"type\":\"icon\",\"icon\":\"bar\",\"x\":3,\"y\":4,\"color\":\"white\"}," TEXT_XYW
                        ",\"h\":30,\"text\":\"kr\",\"fonts\":[\"small\"]}"));
    {
        static const uint8_t expect_icon[] = {ESL_LE_ICON, 1, 0xFF, 0xFF, 0xFF, 0, 0, ESL_LE_WHITE, 3, 0, 4, 9, 0, 1};
        const uint8_t *b = bin.bin;
        size_t icons = ESL_LBIN_HDR_LEN + 1 + 2 * ESL_LE_SIZE + 1 + 2;
        CHECK_MEM(e, expect_icon, sizeof(expect_icon));
        CHECK_INT(b[ESL_LBIN_OFF_NICON], 2);
        static const uint8_t expect_data[] = {2, 0, 2, 0x80, 0x40, 9, 0, 1, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
        CHECK_INT(bin.size, icons + sizeof(expect_data));
        CHECK_MEM(&b[icons], expect_data, sizeof(expect_data));
    }
    /* an icon named by a field: the field with ESL_LE_ICON_FIELD, and the icon names as fixed
       texts after the file's own, the first one's number in ESL_LE_OFF_NAMES */
    CHECK(compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA=\"},\"bar\":{\"w\":9,\"h\":1,\"data\":\"/4A=\"}",
                        TEXT_XYW ",\"h\":30,\"text\":\"kr\",\"fonts\":[\"small\"]},"
                        "{\"type\":\"icon\",\"field\":\"b\",\"x\":3,\"y\":4,\"w\":20,\"h\":10},"
                        "{\"type\":\"icon\",\"field\":0,\"x\":30,\"y\":4,\"w\":20,\"h\":10}"));
    {
        static const uint8_t expect_icon[] = {ESL_LE_ICON, ESL_LE_ICON_FIELD | 1, 1, 0xFF, 0xFF, 0, 0, ESL_LE_BLACK,
                                              3, 0, 4, 20, 0, 10};
        const uint8_t *b = bin.bin;
        CHECK_MEM(e + ESL_LE_SIZE, expect_icon, sizeof(expect_icon));
        CHECK_INT(e[2 * ESL_LE_SIZE + ESL_LE_OFF_FIELD], ESL_LE_ICON_FIELD | 0);
        CHECK_INT(e[2 * ESL_LE_SIZE + ESL_LE_OFF_NAMES], 1); /* the names are added once */
        CHECK_INT(b[ESL_LBIN_OFF_NTEXT], 3);
        static const uint8_t texts[] = {2, 'k', 'r', 3, 'd', 'o', 't', 3, 'b', 'a', 'r'};
        CHECK_MEM(e + 3 * ESL_LE_SIZE, texts, sizeof(texts));
    }
    CHECK(!compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA\"}",
                         "{\"type\":\"icon\",\"field\":\"a\",\"icon\":\"dot\",\"x\":0,\"y\":0,\"w\":2,\"h\":2}"));
    CHECK(HAS(err, "give either icon or field, not both"));
    CHECK(!compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA\"}",
                         "{\"type\":\"icon\",\"field\":\"c\",\"x\":0,\"y\":0,\"w\":2,\"h\":2}"));
    CHECK(HAS(err, "field \"c\" is not in the layout's fields"));
    CHECK(!compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA\"}",
                         "{\"type\":\"icon\",\"field\":\"a\",\"x\":0,\"y\":0}"));
    CHECK(HAS(err, "w must be a whole number of pixels"));
    CHECK(!compile_icons("", "{\"type\":\"icon\",\"field\":\"a\",\"x\":0,\"y\":0,\"w\":2,\"h\":2}"));
    CHECK(HAS(err, "an icon element with a field needs icons in the file"));
    /* w and h of an icon element may be given, and must then be the icon's */
    CHECK(compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA\"}",
                        "{\"type\":\"icon\",\"icon\":\"dot\",\"x\":0,\"y\":0,\"w\":2,\"h\":2}"));

#define LINE "{\"type\":\"line\",\"x\":0,\"y\":0,\"w\":10,\"h\":10"
    bad_el(LINE ",\"width\":0}", "width must be a whole number from 1 to 16");
    bad_el(LINE ",\"width\":17}", "width must be a whole number from 1 to 16");
    bad_el(LINE ",\"color\":\"invert\"}", "a line can be black, white or red");
    bad_el(LINE ",\"color\":\"red\"}", "red and red_invert need a model with red");
    bad_el(LINE ",\"direction\":\"left\"}", "direction must be down");
    bad_el(LINE ",\"radius\":5}", "radius is only for rect elements");
    bad_el("{\"type\":\"line\",\"x\":0,\"y\":0,\"w\":297,\"h\":10}", "past the right edge");
    bad_el(TEXT_A ",\"direction\":\"up\"}", "direction is only for line elements");
    char many[1200] = "";
    for (int i = 0; i < ESL_DRAW_MAX_LINES + 1; i++)
        strcat(many, i ? "," LINE "}" : LINE "}");
    bad_el(many, "at most 16 lines per screen");
    /* the compiler checks the limits in esl_limits (the web renderer sets them to its AP's) */
    {
        esl_limits_t keep = esl_limits;
        char two[200] = LINE "}," LINE "}";
        CHECK(compile_el(two));
        esl_limits.lines = 1;
        bad_el(two, "at most 1 lines per screen");
        esl_limits.lines = keep.lines;
        esl_limits.runs = 1;
        bad_el(TEXT_A "}," TEXT_A "}", "needs up to 2 lines of text, the AP draws at most 1 per screen");
        esl_limits = keep;
    }
#undef LINE
    /* a QR code: level in the first font byte, module size in lines, alignment in opts */
    CHECK(compile_el("{\"type\":\"qr\",\"field\":\"b\",\"x\":10,\"y\":20,\"w\":66,\"h\":66,\"ecc\":\"H\",\"module\":2,"
                     "\"align\":\"right\",\"valign\":\"bottom\"}"));
    {
        static const uint8_t expect_qr[] = {ESL_LE_QR, 1, ESL_QR_LEVEL_H, 0xFF, 0xFF, ESL_LE_RIGHT | ESL_LE_BOTTOM, 2,
                                            ESL_LE_BLACK, 10, 0, 20, 66, 0, 66};
        CHECK_MEM(e, expect_qr, sizeof(expect_qr));
        CHECK_INT(bin.qr_field[0], 1);
        CHECK_INT(bin.qr_ecc[0], ESL_QR_LEVEL_H);
        CHECK_INT(bin.qr_cap[0], 24); /* 33 modules of 2 px: version 3 (29 and the quiet zone), 24 bytes at H */
        CHECK_INT(bin.bc_field[0], -1);
    }
    CHECK(compile_el("{\"type\":\"qr\",\"field\":\"a\",\"x\":0,\"y\":0,\"w\":25,\"h\":25}"));
    CHECK_INT(e[ESL_LE_OFF_ECC], ESL_QR_LEVEL_M);
    CHECK_INT(bin.qr_cap[0], 14);
#define QR "{\"type\":\"qr\",\"field\":\"a\",\"x\":0,\"y\":0,\"w\":60,\"h\":60"
    bad_el(QR ",\"ecc\":\"X\"}", "ecc must be L, M, Q or H");
    bad_el(QR ",\"module\":9}", "module must be a whole number from 0 to 8");
    bad_el(QR ",\"color\":\"white\"}", "a QR code is black");
    bad_el(QR ",\"symbology\":\"ean13\"}", "symbology is only for barcode elements");
    bad_el(QR ",\"fonts\":[\"small\"]}", "fonts is only for text elements");
    bad_el(QR "}," QR "}", "at most 1 QR code per screen");
    bad_el("{\"type\":\"qr\",\"text\":\"x\",\"x\":0,\"y\":0,\"w\":60,\"h\":60}", "a QR code takes a field, not a fixed text");
    bad_el("{\"type\":\"qr\",\"x\":0,\"y\":0,\"w\":60,\"h\":60}", "a QR element needs a field");
    bad_el("{\"type\":\"qr\",\"field\":\"a\",\"x\":0,\"y\":0,\"w\":60,\"h\":20}", "at least 25 px wide and high");
    bad_el("{\"type\":\"qr\",\"field\":\"a\",\"x\":0,\"y\":0,\"w\":60,\"h\":40,\"module\":2}", "at least 50 px");
    bad_el("{\"type\":\"barcode\",\"field\":0,\"symbology\":\"code128\",\"x\":0,\"y\":0,\"w\":200,\"h\":20,\"ecc\":\"L\"}",
           "ecc is only for QR elements");
#undef QR
#define RECT "{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":40,\"h\":20"
    bad_el(RECT ",\"radius\":11}", "radius must be a whole number from 0 to 10");
    bad_el(RECT ",\"border\":11}", "border must be a whole number from 0 to 10");
    bad_el(RECT ",\"width\":2}", "width is only for line elements");
#undef RECT
    CHECK(!compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA=\"}",
                         "{\"type\":\"icon\",\"icon\":\"none\",\"x\":0,\"y\":0}"));
    CHECK(HAS(err, "icon \"none\" is not in the file's icons"));
    CHECK(!compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA=\"}",
                         "{\"type\":\"icon\",\"icon\":\"dot\",\"x\":0,\"y\":0,\"w\":3}"));
    CHECK(HAS(err, "w must be the icon's width (2), or left out"));
    CHECK(!compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA=\"}",
                         "{\"type\":\"icon\",\"icon\":\"dot\",\"x\":295,\"y\":0}"));
    CHECK(HAS(err, "past the right edge"));
    CHECK(!compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA=\"}",
                         "{\"type\":\"icon\",\"icon\":\"dot\",\"x\":0,\"y\":0,\"color\":\"frame\"}"));
    CHECK(HAS(err, "an icon can be black, white or red"));
    CHECK(!compile_el("{\"type\":\"icon\",\"x\":0,\"y\":0}"));
    CHECK(HAS(err, "an icon element needs icon"));
    CHECK(!compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA=gEA=\"}", TEXT_A "}"));
    CHECK(HAS(err, "icons.dot: data must be base64 of 2 bytes (2 rows of 1)"));
    CHECK(!compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"g!A=\"}", TEXT_A "}"));
    CHECK(HAS(err, "icons.dot: data must be base64"));
    /* "\/" for "/" (PHP's json_encode writes that): 3 rows of 8 drawn pixels, 8 columns of 0xE0 */
    CHECK(compile_icons("\"bar\":{\"w\":8,\"h\":3,\"data\":\"\\/\\/\\/\\/\"}",
                        "{\"type\":\"icon\",\"icon\":\"bar\",\"x\":0,\"y\":0}"));
    for (int i = 1; i <= 8; i++)
        CHECK_INT(bin.bin[bin.size - i], 0xE0);
    CHECK(!compile_icons("\"dot\":{\"w\":0,\"h\":2,\"data\":\"\"}", TEXT_A "}"));
    CHECK(HAS(err, "icons.dot: w and h must be 1 to 128 pixels"));
    CHECK(!compile_icons("\"Dot\":{\"w\":2,\"h\":2,\"data\":\"gEA=\"}", TEXT_A "}"));
    CHECK(HAS(err, "icon names must be"));
    CHECK(!compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA=\"},\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA=\"}",
                         TEXT_A "}"));
    CHECK(HAS(err, "the icon \"dot\" appears twice"));
    CHECK(!compile_icons("\"dot\":{\"w\":2,\"h\":2,\"data\":\"gEA=\",\"x\":1}", TEXT_A "}"));
    CHECK(HAS(err, "unknown key \"x\""));
}

static void *no_memory(size_t n)
{
    (void)n;
    return NULL;
}

/* The sizes the store asks for (#187: a file is kept at its compiled size) */
static size_t alloc_sizes[4];
static unsigned alloc_count;
static void *counting_alloc(size_t n)
{
    if (alloc_count < sizeof(alloc_sizes) / sizeof(alloc_sizes[0]))
        alloc_sizes[alloc_count] = n;
    alloc_count++;
    return malloc(n);
}

static void test_storage(void)
{
    static esl_layouts_t ls;
    fake_t f = {0};
    esl_layout_ops_t ops = {&f, f_lock, f_unlock, f_save, f_erase, NULL};
    char buf[512];
    esl_sb_t sb;
    bool changed;
    uint32_t v;
    size_t len, bsize;
    esl_layout_env_t env;
    uint8_t nf;

    esl_layouts_init(&ls, &ops);
    /* a model holds memory only while it has a file */
    CHECK(ls.models[ESL_MODEL_29 - 1] == NULL && ls.models[ESL_MODEL_154 - 1] == NULL);
    CHECK(!esl_layouts_info(&ls, ESL_MODEL_29, &v, &len, &bsize));
    CHECK_INT(v, 0);
    esl_layouts_env(&ls, ESL_MODEL_29, &env);
    CHECK_INT(env.version, 0);
    CHECK_INT(env.count, 0);
    esl_layouts_env(NULL, ESL_MODEL_29, &env);
    CHECK_INT(env.version, 0);

    /* store the example file */
    size_t jl;
    char *js = read_example_layout("2.9.json", &jl);
    CHECK(js != NULL);
    if (!js)
        return;
    CHECK(esl_layout_compile(js, jl, ESL_MODEL_29, &bin, err, sizeof(err)));
    CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_154, js, jl, &bin, &changed), 0); /* compiled for another model */
    CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_UNKNOWN, js, jl, &bin, &changed), 0);
    CHECK_INT(f.saves, 0);
    v = esl_layouts_set(&ls, ESL_MODEL_29, js, jl, &bin, &changed);
    CHECK_INT(v, bin.version);
    CHECK(changed);
    CHECK_INT(f.saves, 1);
    CHECK_INT(f.last_model, ESL_MODEL_29);
    CHECK_INT(f.last_len, jl);
    CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_29, js, jl, &bin, &changed), v);
    CHECK(!changed);
    CHECK_INT(f.saves, 1);
    CHECK(ls.models[ESL_MODEL_29 - 1] != NULL && ls.models[ESL_MODEL_154 - 1] == NULL);
    CHECK_INT(ls.models[ESL_MODEL_29 - 1]->len, jl);

    /* no memory: 0 back (the API answers 500), the old file stays, nothing is persisted */
    {
        char *spaced = malloc(jl + 1);
        memcpy(spaced, js, jl);
        spaced[jl] = ' ';
        esl_layout_bin_t bin2 = bin;
        bin2.version = bin.version + 1;
        ls.ops.alloc = no_memory;
        CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_29, spaced, jl + 1, &bin2, &changed), 0);
        CHECK(!changed);
        CHECK_INT(f.saves, 1);
        CHECK_INT(ls.models[ESL_MODEL_29 - 1]->len, jl);
        CHECK_INT(ls.models[ESL_MODEL_29 - 1]->bin.version, v);
        size_t jl154;
        char *js154 = read_example_layout("1.54.json", &jl154);
        CHECK(js154 != NULL && esl_layout_compile(js154, jl154, ESL_MODEL_154, &bin2, err, sizeof(err)));
        CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_154, js154, jl154, &bin2, &changed), 0);
        CHECK(!changed);
        CHECK(ls.models[ESL_MODEL_154 - 1] == NULL);
        ls.ops.alloc = NULL;
        free(js154);
        free(spaced);
    }

    /* a stored file takes its compiled size, not ESL_LBIN_MAX, and draws as the whole one */
    {
        char *spaced = malloc(jl + 1);
        memcpy(spaced, js, jl);
        spaced[jl] = ' ';
        alloc_count = 0;
        ls.ops.alloc = counting_alloc;
        CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_29, spaced, jl + 1, &bin, &changed), v);
        CHECK(changed);
        ls.ops.alloc = NULL;
        CHECK_INT(alloc_count, 2);
        CHECK_INT(alloc_sizes[0], jl + 1);
        CHECK_INT(alloc_sizes[1], ESL_LAYOUT_BYTES(bin.size));
        CHECK(ESL_LAYOUT_BYTES(bin.size) + ESL_LBIN_MAX / 2 < sizeof(esl_layout_t));
        CHECK_INT(ls.models[ESL_MODEL_29 - 1]->bin.size, bin.size);
        CHECK(memcmp(ls.models[ESL_MODEL_29 - 1]->bin.bin, bin.bin, bin.size) == 0);
        CHECK_STR(ls.models[ESL_MODEL_29 - 1]->bin.names[1], "sign");
        static uint8_t a[ESL_LBIN_MAX * 2], b[ESL_LBIN_MAX * 2];
        esl_content_t c;
        CHECK(esl_content_init_named(&c, "shelf", 5, 0) == ESL_CONTENT_OK);
        CHECK(esl_content_add_field(&c, (const uint8_t *)"Milk", 4) == ESL_CONTENT_OK);
        size_t na = esl_layouts_render(&ls, ESL_MODEL_29, &c, a, sizeof(a));
        size_t nb = esl_render(ESL_MODEL_29, &bin, &c, b, sizeof(b));
        CHECK(na > 0 && na == nb && memcmp(a, b, na) == 0);
        /* back to the file it had, in a new block of its size (the old one is freed) */
        CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_29, js, jl, &bin, &changed), v);
        CHECK(changed);
        free(spaced);
    }

    uint32_t v2;
    CHECK(esl_layouts_info(&ls, ESL_MODEL_29, &v2, &len, &bsize));
    CHECK_INT(v2, v);
    CHECK_INT(len, jl);
    CHECK_INT(bsize, bin.size);
    CHECK(!esl_layouts_info(&ls, ESL_MODEL_154, &v2, NULL, NULL));
    esl_sb_init(&sb, buf, sizeof(buf));
    esl_layouts_write_names(&ls, ESL_MODEL_29, &sb);
    CHECK_STR(buf, "[\"shelf\",\"sign\"]");
    esl_sb_init(&sb, buf, sizeof(buf));
    esl_layouts_write_names(&ls, ESL_MODEL_154, &sb);
    CHECK_STR(buf, "[]");
    CHECK_INT(esl_layouts_find(&ls, ESL_MODEL_29, "sign", 4, &nf), 1);
    CHECK_INT(nf, 2);
    CHECK_INT(esl_layouts_find(&ls, ESL_MODEL_29, "sig", 3, &nf), -1);
    CHECK_INT(esl_layouts_find(&ls, ESL_MODEL_154, "sign", 4, &nf), -1);
    esl_layouts_env(&ls, ESL_MODEL_29, &env);
    CHECK_INT(env.version, v);
    CHECK_INT(env.count, 2);
    CHECK_STR(env.names[1], "sign");
    CHECK_INT(env.nfields[0], 4);

    esl_sb_init(&sb, buf, sizeof(buf));
    CHECK(!esl_layouts_write(&ls, ESL_MODEL_154, &sb));
    CHECK(!esl_layouts_write(&ls, 9, &sb));

    /* delete erases once */
    CHECK(esl_layouts_delete(&ls, ESL_MODEL_29));
    CHECK_INT(f.erases, 1);
    CHECK(!esl_layouts_delete(&ls, ESL_MODEL_29));
    CHECK(!esl_layouts_delete(&ls, 0));
    CHECK_INT(f.erases, 1);
    CHECK_INT(esl_layouts_find(&ls, ESL_MODEL_29, "sign", 4, &nf), -1);
    CHECK(ls.models[ESL_MODEL_29 - 1] == NULL);

    /* restore at boot compiles the file again and does not write back */
    int saves = f.saves;
    CHECK(esl_layouts_restore(&ls, ESL_MODEL_29, js, jl));
    CHECK(!esl_layouts_restore(&ls, ESL_MODEL_154, js, jl)); /* does not compile for that model */
    CHECK(!esl_layouts_restore(&ls, 0, js, jl));
    CHECK(!esl_layouts_restore(&ls, ESL_MODEL_29, "{}", 2));
    CHECK_INT(f.saves, saves);
    CHECK(esl_layouts_info(&ls, ESL_MODEL_29, &v2, NULL, NULL));
    CHECK_INT(v2, v); /* the same binary, the same version */
    CHECK(ls.ops.persist_save == f_save); /* hooks are back */
    esl_sb_init(&sb, buf, sizeof(buf));
    CHECK(esl_layouts_write(&ls, ESL_MODEL_29, &sb));
    CHECK(sb.overflow); /* 2 KB file, 512-byte buffer */
    CHECK_INT(f.depth, 0);

    /* whitespace does not change what the tags get: same version, but the file is stored */
    char *spaced = malloc(jl + 2);
    memcpy(spaced, js, jl);
    spaced[jl] = '\n';
    CHECK(esl_layout_compile(spaced, jl + 1, ESL_MODEL_29, &bin, err, sizeof(err)));
    CHECK_INT(esl_layouts_set(&ls, ESL_MODEL_29, spaced, jl + 1, &bin, &changed), v);
    CHECK(changed);
    free(spaced);
    free(js);
}

void test_layout(void)
{
    test_examples();
    test_binary();
    test_errors();
    test_shapes();
    test_storage();
}
