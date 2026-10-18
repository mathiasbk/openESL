#include "tinytest.h"
#include "esl_crc32.h"
#include "esl_layout.h"
#include <stdlib.h>

/* Layout files: the compiler (JSON -> what the tags download, krav A13) and
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
        ESL_LBIN_FORMAT, ESL_MODEL_29, 1, 0, 2,
        ESL_LE_TEXT, 1, ESL_FONT_LARGE, ESL_FONT_MID, ESL_FONT_SMALL, ESL_LE_RIGHT | ESL_LE_BOTTOM, 2,
        ESL_LE_PRICE_HL, 4, 1, 5, 36, 0, 120,
        ESL_LE_RECT, 0, 0xFF, 0xFF, 0xFF, 0, 0, ESL_LE_FRAME, 0, 0, 125, 0x28, 1, 3};
    CHECK_INT(bin.size, sizeof(expect));
    CHECK_MEM(bin.bin, expect, sizeof(expect));
    CHECK_STR(bin.names[0], "l");
    CHECK_INT(bin.nfields[0], 2);

    /* defaults: left, top, lines 0, black */
    CHECK(compile_el(TEXT_A "}"));
    CHECK_INT(bin.bin[5 + ESL_LE_OFF_OPTS], 0);
    CHECK_INT(bin.bin[5 + ESL_LE_OFF_LINES], 0);
    CHECK_INT(bin.bin[5 + ESL_LE_OFF_COLOR], ESL_LE_BLACK);
    CHECK_INT(bin.bin[5 + ESL_LE_OFF_FONT + 1], ESL_FONT_NONE);

    /* a barcode element: symbology in the first font byte, module width in lines */
    CHECK(compile_el("{\"type\":\"barcode\",\"field\":\"b\",\"symbology\":\"ean13\",\"module\":2,"
                     "\"x\":10,\"y\":70,\"w\":226,\"h\":40,\"align\":\"center\"}"));
    static const uint8_t expect_bc[] = {
        ESL_LBIN_FORMAT, ESL_MODEL_29, 1, 0, 1,
        ESL_LE_BARCODE, 1, ESL_BC_EAN13, 0xFF, 0xFF, ESL_LE_CENTER, 2, ESL_LE_BLACK, 10, 0, 70, 226, 0, 40};
    CHECK_INT(bin.size, sizeof(expect_bc));
    CHECK_MEM(bin.bin, expect_bc, sizeof(expect_bc));
    CHECK_INT(bin.bc_field[0], 1);
    CHECK_INT(bin.bc_sym[0], ESL_BC_EAN13);
    CHECK_INT(bin.bc_max[0], 113);
    CHECK(compile_el("{\"type\":\"barcode\",\"field\":0,\"symbology\":\"code128\",\"x\":0,\"y\":0,\"w\":296,\"h\":10}"));
    CHECK_INT(bin.bin[5 + ESL_LE_OFF_SYM], ESL_BC_CODE128);
    CHECK_INT(bin.bin[5 + ESL_LE_OFF_MODULE], 0);
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
    bad_el("{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":10,\"h\":1,\"color\":\"red\"}", "color must be");
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
    bad_el(TEXT_XYW ",\"h\":20,\"field\":\"a\",\"fonts\":[\"medium\"]}", "too small for its smallest font (24 px");
    bad_el(TEXT_A ",\"color\":\"invert\"}", "black border of an inverted field");
    /* the tag's scene: 16 text lines and 10 rectangles per screen */
#define TALL TEXT_XYW ",\"h\":128,\"field\":\"a\",\"fonts\":[\"small\"]"
#define R1 "{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":1,\"h\":1}"
#define R5 R1 "," R1 "," R1 "," R1 "," R1
    bad_el(TALL "}," TALL "}," TALL "}," TALL "}," TALL "}," TALL ",\"lines\":2}",
           "layouts[0]: needs up to 17 lines of text, the tag draws at most 16");
    bad_el(R5 "," R5 "," TEXT_XYW ",\"h\":40,\"field\":\"a\",\"fonts\":[\"medium\"],\"color\":\"highlight\"}",
           "needs 11 rectangles");
    bad_el(R5 "," R5 "," R1, "needs 11 rectangles");
    /* 9 rects + one inverted field, and 5 * 3 + 1 lines, are exactly the limits */
    CHECK(compile_el(R5 "," R1 "," R1 "," R1 "," R1 ","
                     TEXT_XYW ",\"h\":40,\"field\":\"a\",\"fonts\":[\"medium\"],\"color\":\"highlight\"}"));
    CHECK(compile_el(TALL "}," TALL "}," TALL "}," TALL "}," TALL "}," TALL ",\"lines\":1}"));
#undef R5
#undef R1
#undef TALL

    /* fixed texts: stored once at the end of the file, the element points at them */
    CHECK(compile_el(TEXT_XYW ",\"h\":30,\"text\":\"inkl. mva\",\"fonts\":[\"small\"]},"
                     TEXT_XYW ",\"h\":30,\"text\":\"kr\",\"fonts\":[\"small\"]},"
                     TEXT_XYW ",\"h\":30,\"text\":\"inkl. mva\",\"fonts\":[\"small\"]},"
                     TEXT_A "}"));
    {
        const uint8_t *b = bin.bin;
        size_t els = ESL_LBIN_HDR_LEN + 1 + 4 * ESL_LE_SIZE;
        CHECK_INT(b[ESL_LBIN_OFF_NTEXT], 2);
        CHECK_INT(b[5 + ESL_LE_OFF_FIELD], ESL_LE_FIELD_FIXED | 0);
        CHECK_INT(b[5 + ESL_LE_SIZE + ESL_LE_OFF_FIELD], ESL_LE_FIELD_FIXED | 1);
        CHECK_INT(b[5 + 2 * ESL_LE_SIZE + ESL_LE_OFF_FIELD], ESL_LE_FIELD_FIXED | 0);
        CHECK_INT(b[5 + 3 * ESL_LE_SIZE + ESL_LE_OFF_FIELD], 0);
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

    /* too big for the tags: 16 layouts of 4 rects = 4 + 16 * 57 bytes */
    static char js[8192];
    esl_sb_t sb;
    esl_sb_init(&sb, js, sizeof(js));
    esl_sb_puts(&sb, "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[");
    for (int i = 0; i < 14; i++) {
        esl_sb_printf(&sb, "%s{\"name\":\"l%d\",\"fields\":[\"a\"],\"elements\":[", i ? "," : "", i);
        for (int k = 0; k < 4; k++)
            esl_sb_printf(&sb, "%s{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":1,\"h\":1}", k ? "," : "");
        esl_sb_puts(&sb, "]}");
    }
    esl_sb_puts(&sb, "]}");
    CHECK(!sb.overflow);
    CHECK(!compile(js, ESL_MODEL_29));
    CHECK(HAS(err, "layouts[13]: the file is too big for the tags: at most 768 bytes"));
}

static void test_storage(void)
{
    static esl_layouts_t ls;
    fake_t f = {0};
    esl_layout_ops_t ops = {&f, f_lock, f_unlock, f_save, f_erase};
    char buf[512];
    esl_sb_t sb;
    bool changed;
    uint32_t v;
    size_t len, bsize;
    esl_layout_env_t env;
    uint8_t msg[ESL_MAX_PAYLOAD];
    esl_reply_t r;
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

    /* no file: a LAYOUT with version 0 tells the tag to use the built-in layouts */
    size_t n = esl_layouts_block(&ls, ESL_MODEL_29, 0, 0, 94, 60, msg);
    CHECK(esl_reply_parse(msg, n, &r));
    CHECK_INT(r.type, ESL_MSG_LAYOUT);
    CHECK_INT(r.next_poll_s, 60);
    CHECK_INT(r.layout_ver, 0);
    CHECK_INT(r.layout_size, 0);
    CHECK_INT(r.layout_len, 0);
    n = esl_layouts_block(NULL, ESL_MODEL_29, 0, 0, 94, 0, msg);
    CHECK(esl_reply_parse(msg, n, &r));
    CHECK_INT(r.layout_ver, 0);

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

    /* blocks: from the start, by position, at most max and ESL_LAYOUT_CHUNK_MAX bytes */
    n = esl_layouts_block(&ls, ESL_MODEL_29, 0, 0, 200, 0, msg);
    CHECK(esl_reply_parse(msg, n, &r));
    CHECK_INT(r.layout_ver, v);
    CHECK_INT(r.layout_size, bin.size);
    CHECK_INT(r.layout_pos, 0);
    CHECK_INT(r.layout_len, ESL_LAYOUT_CHUNK_MAX);
    CHECK_INT(n, ESL_MAX_PAYLOAD - ESL_MIC_RESERVE);
    CHECK_MEM(r.layout_data, bin.bin, ESL_LAYOUT_CHUNK_MAX);
    n = esl_layouts_block(&ls, ESL_MODEL_29, v, 94, 50, 0, msg);
    CHECK(esl_reply_parse(msg, n, &r));
    CHECK_INT(r.layout_pos, 94);
    CHECK_INT(r.layout_len, 50);
    CHECK_MEM(r.layout_data, &bin.bin[94], 50);
    n = esl_layouts_block(&ls, ESL_MODEL_29, v, 150, 94, 0, msg); /* the last, short block */
    CHECK(esl_reply_parse(msg, n, &r));
    CHECK_INT(r.layout_len, bin.size - 150);
    /* the file changed under the tag (or it asks for an old version): block 0 of the current one */
    n = esl_layouts_block(&ls, ESL_MODEL_29, v ^ 1, 94, 94, 0, msg);
    CHECK(esl_reply_parse(msg, n, &r));
    CHECK_INT(r.layout_ver, v);
    CHECK_INT(r.layout_pos, 0);
    n = esl_layouts_block(&ls, ESL_MODEL_29, v, 4000, 94, 0, msg); /* past the end */
    CHECK(esl_reply_parse(msg, n, &r));
    CHECK_INT(r.layout_pos, 0);

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
    test_storage();
}
