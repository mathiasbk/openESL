/* test_barcode.c - Code 128 and EAN-13 encoding, and the barcode layout element.
   The scenes are also written as PBM images to $ESL_TEST_OUT (set by tools/build.py), where
   test_label_barcode.py reads them back with a barcode decoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "barcode.h"
#include "layout.h"
#include "helpers.h"

static uint8_t bits[DRAW_BC_BYTES];

static uint16_t enc(uint8_t sym, const char *t)
{
    return barcode_encode(sym, (const uint8_t *)t, (uint8_t)strlen(t), bits, DRAW_W);
}

static int bit(uint16_t i)
{
    return (bits[i >> 3] >> (7 - (i & 7))) & 1;
}

static void test_ean13(void)
{
    /* 590123412345 + check digit 7, modules from python-barcode 0.16 */
    static const char want[] = "10100010110100111011001100100110111101001110101010110011011011001000010101110010011101000100101";
    uint16_t i, n = enc(ESL_BC_EAN13, "590123412345");
    int same = 1;

    CHECK_EQ(n, 95);
    for (i = 0; i < 95; i++)
        if (bit(i) != want[i] - '0')
            same = 0;
    CHECK(same);
    CHECK_EQ(enc(ESL_BC_EAN13, "5901234123457"), 95);   /* with the right check digit */
    CHECK_EQ(enc(ESL_BC_EAN13, "5901234123458"), 0);    /* wrong check digit */
    CHECK_EQ(enc(ESL_BC_EAN13, "59012341234"), 0);      /* 11 digits */
    CHECK_EQ(enc(ESL_BC_EAN13, "59012341234A"), 0);
    CHECK_EQ(barcode_quiet_left(ESL_BC_EAN13), 11);
    CHECK_EQ(barcode_quiet_right(ESL_BC_EAN13), 7);
}

static void test_code128(void)
{
    uint8_t hi[2] = { 'A', 0xE6 };

    /* digits: code set C, two per symbol; start + 4 + check = 6 symbols, stop 13 */
    CHECK_EQ(enc(ESL_BC_CODE128, "12345678"), 11 * 6 + 13);
    /* odd number of digits: 3 pairs, CODE B, the last digit */
    CHECK_EQ(enc(ESL_BC_CODE128, "1234567"), 11 * 7 + 13);
    CHECK_EQ(enc(ESL_BC_CODE128, "Hello-123"), 11 * 11 + 13);
    CHECK_EQ(enc(ESL_BC_CODE128, "7"), 11 * 3 + 13);
    /* start B (11010010000) first, stop (1100011101011) last */
    enc(ESL_BC_CODE128, "A");
    CHECK(bit(0) && bit(1) && !bit(2) && bit(3) && !bit(4) && !bit(5) && bit(6));
    CHECK(bit(33) && bit(34) && !bit(35) && bit(45));
    CHECK_EQ(enc(ESL_BC_CODE128, ""), 0);
    CHECK_EQ(barcode_encode(ESL_BC_CODE128, hi, 2, bits, DRAW_W), 0);   /* æ is not ASCII */
    CHECK_EQ(barcode_encode(ESL_BC_CODE128, (const uint8_t *)"ABCDEFGHIJ", 10, bits, 100), 0); /* too wide */
    CHECK_EQ(barcode_encode(9, (const uint8_t *)"1", 1, bits, DRAW_W), 0);  /* unknown symbology */
}

/* ---- the layout element ---- */

static uint8_t file[64];
static draw_scene_t sc;
static uint8_t img[DRAW_H][DRAW_W];
static FILE *list;

/* A file with one layout: a barcode element over the whole width */
static void barcode_layout(uint8_t sym, uint8_t module, uint8_t align, uint16_t x, uint16_t w)
{
    uint16_t len;
    uint32_t c;
    layout_file[0] = ESL_LBIN_FORMAT;
    layout_file[1] = MODEL_CODE;
    layout_file[2] = 1;
    layout_file[3] = 0;
    layout_file[4] = 1;
    put_elem(layout_file + 5, ESL_LE_BARCODE, 0, sym, ESL_FONT_NONE, ESL_FONT_NONE, align, module,
             ESL_LE_BLACK, x, 20, w, 60);
    len = 5 + ESL_LE_SIZE;
    c = proto_crc32(0, layout_file, len);
    CHECK(layout_file_install(len, c ? c : 1));
}

/* Installs the one-layout file again after a change, with the right CRC */
static bool install_again(void)
{
    uint32_t c = proto_crc32(0, layout_file, 5 + ESL_LE_SIZE);
    return layout_file_install(5 + ESL_LE_SIZE, c ? c : 1);
}

static void draw(const char *text)
{
    static const char *f[1];
    static proto_reply_t r;
    uint8_t black[DRAW_COL_BYTES], red[DRAW_COL_BYTES];
    uint16_t x;
    uint8_t y, n;

    f[0] = text;
    n = make_content(file, 0, 1, ESL_LAYOUT_FILE, 0, 1, f);
    CHECK_EQ(proto_parse_reply(file, n, &r), PROTO_OK);
    layout_build(&sc, &r, file);
    draw_begin(&sc);
    for (x = 0; x < DRAW_W; x++) {
        draw_column(&sc, x, black, red);
        for (y = 0; y < DRAW_H; y++)
            img[y][x] = (black[y >> 3] >> (7 - (y & 7))) & 1;
    }
}

static void save(const char *name, const char *sym, const char *text)
{
    const char *dir = getenv("ESL_TEST_OUT");
    char path[512];
    FILE *fp;
    int x, y;

    if (!dir)
        return;
    snprintf(path, sizeof(path), "%s/%s-%s.pbm", dir, MODEL_NAME, name);
    fp = fopen(path, "wb");
    if (!fp)
        return;
    fprintf(fp, "P1\n%d %d\n", DRAW_W, DRAW_H);
    for (y = 0; y < DRAW_H; y++) {
        for (x = 0; x < DRAW_W; x++)
            fputc(img[y][x] ? '1' : '0', fp);
        fputc('\n', fp);
    }
    fclose(fp);
    if (list)
        fprintf(list, "%s-%s.pbm\t%s\t%s\n", MODEL_NAME, name, sym, text);
}

/* First and last black column in row y, and the narrowest bar */
static void span(uint8_t y, int *first, int *last, int *narrow)
{
    int x, run = 0;
    *first = -1;
    *last = -1;
    *narrow = 1000;
    for (x = 0; x <= DRAW_W; x++) {
        if (x < DRAW_W && img[y][x]) {
            if (*first < 0)
                *first = x;
            *last = x;
            run++;
        } else if (run) {
            if (run < *narrow)
                *narrow = run;
            run = 0;
        }
    }
}

static void test_element(void)
{
    int first, last, narrow;
    const char *dir = getenv("ESL_TEST_OUT");
    char path[512];

    list = 0;
    if (dir) {
        snprintf(path, sizeof(path), "%s/%s-list.txt", dir, MODEL_NAME);
        list = fopen(path, "w");
    }

    /* EAN-13, widest module that fits, centred: bars only inside the box rows */
    barcode_layout(ESL_BC_EAN13, 0, ESL_LE_CENTER, 0, DRAW_W);
    draw("590123412345");
    CHECK_EQ(sc.nbars, 1);
    CHECK_EQ(sc.bars.mw, DRAW_W / 113);
    span(50, &first, &last, &narrow);
    CHECK_EQ(narrow, sc.bars.mw);
    CHECK_EQ(last - first + 1, 95 * sc.bars.mw);
    CHECK_EQ(first, (DRAW_W - 113 * sc.bars.mw) / 2 + 11 * sc.bars.mw);
    span(19, &first, &last, &narrow);
    CHECK_EQ(first, -1);
    span(80, &first, &last, &narrow);
    CHECK_EQ(first, -1);
    span(79, &first, &last, &narrow);
    CHECK(first >= 0);
    save("ean13", "ean13", "5901234123457");

    draw("8713331002910");
    CHECK_EQ(sc.nbars, 1);
    save("ean13-b", "ean13", "8713331002910");

    /* invalid content: nothing drawn */
    draw("8713331002911");
    CHECK_EQ(sc.nbars, 0);

    /* Code 128, module 1, left */
    barcode_layout(ESL_BC_CODE128, 1, ESL_LE_LEFT, 0, DRAW_W);
    draw("Hello-123");
    CHECK_EQ(sc.nbars, 1);
    span(50, &first, &last, &narrow);
    CHECK_EQ(first, 10);
    CHECK_EQ(last - first + 1, 11 * 11 + 13);       /* the code starts and ends with a bar */
    save("c128-text", "code128", "Hello-123");

    barcode_layout(ESL_BC_CODE128, 0, ESL_LE_RIGHT, 0, DRAW_W);
    draw("1234567");
    CHECK_EQ(sc.nbars, 1);
    span(50, &first, &last, &narrow);
    CHECK_EQ(last, DRAW_W - 1 - 10 * sc.bars.mw);
    save("c128-digits", "code128", "1234567");

    /* too wide for the box with 1-pixel modules: nothing; a fixed module that does not fit: nothing */
    barcode_layout(ESL_BC_CODE128, 0, ESL_LE_LEFT, 0, 60);
    draw("ABCDEFGH");
    CHECK_EQ(sc.nbars, 0);
    barcode_layout(ESL_BC_EAN13, 4, ESL_LE_LEFT, 0, DRAW_W);
    draw("590123412345");
    CHECK_EQ(sc.nbars, 0);

    /* the file check rejects a bad symbology, module width or colour */
    barcode_layout(ESL_BC_EAN13, 0, ESL_LE_LEFT, 0, DRAW_W);
    layout_file[5 + ESL_LE_OFF_SYM] = ESL_BC_COUNT;
    CHECK(!install_again());
    barcode_layout(ESL_BC_EAN13, 0, ESL_LE_LEFT, 0, DRAW_W);
    layout_file[5 + ESL_LE_OFF_MODULE] = ESL_BC_MODULE_MAX + 1;
    CHECK(!install_again());
    barcode_layout(ESL_BC_EAN13, 0, ESL_LE_LEFT, 0, DRAW_W);
    layout_file[5 + ESL_LE_OFF_COLOR] = ESL_LE_WHITE;
    CHECK(!install_again());
    barcode_layout(ESL_BC_EAN13, 0, ESL_LE_LEFT, 0, DRAW_W);
    layout_file[5 + ESL_LE_OFF_FIELD] = ESL_MAX_FIELDS;
    CHECK(!install_again());
    barcode_layout(ESL_BC_EAN13, 0, ESL_LE_LEFT, 0, DRAW_W);
    CHECK(install_again());
    layout_file_clear();
    if (list)
        fclose(list);
}

void test_barcode(void)
{
    RUN(test_ean13);
    RUN(test_code128);
    RUN(test_element);
}
