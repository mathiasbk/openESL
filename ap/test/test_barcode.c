/* test_barcode.c - Code 128 and EAN-13 encoding (tag/src/barcode.c), and the barcode layout
 * element as esl_render draws it. The screens are also written as PBM images to
 * $ESL_TEST_OUT (build/barcodes by default), where test_barcode_decode.py reads them back
 * with a barcode decoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "barcode.h"
#include "esl_render.h"
#include "tinytest.h"

#define W 296
#define H 128

static uint8_t bits[64];
static uint8_t img[H][W];
static FILE *list;

static uint16_t enc(uint8_t sym, const char *t)
{
    return barcode_encode(sym, (const uint8_t *)t, (uint8_t)strlen(t), bits, W);
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

    CHECK_INT(n, 95);
    for (i = 0; i < 95; i++)
        if (bit(i) != want[i] - '0')
            same = 0;
    CHECK(same);
    CHECK_INT(enc(ESL_BC_EAN13, "5901234123457"), 95);   /* with the right check digit */
    CHECK_INT(enc(ESL_BC_EAN13, "5901234123458"), 0);    /* wrong check digit */
    CHECK_INT(enc(ESL_BC_EAN13, "59012341234"), 0);      /* 11 digits */
    CHECK_INT(enc(ESL_BC_EAN13, "59012341234A"), 0);
    CHECK_INT(barcode_quiet_left(ESL_BC_EAN13), 11);
    CHECK_INT(barcode_quiet_right(ESL_BC_EAN13), 7);
}

static void test_code128(void)
{
    uint8_t hi[2] = { 'A', 0xE6 };

    /* digits: code set C, two per symbol; start + 4 + check = 6 symbols, stop 13 */
    CHECK_INT(enc(ESL_BC_CODE128, "12345678"), 11 * 6 + 13);
    /* odd number of digits: 3 pairs, CODE B, the last digit */
    CHECK_INT(enc(ESL_BC_CODE128, "1234567"), 11 * 7 + 13);
    CHECK_INT(enc(ESL_BC_CODE128, "Hello-123"), 11 * 11 + 13);
    CHECK_INT(enc(ESL_BC_CODE128, "7"), 11 * 3 + 13);
    /* start B (11010010000) first, stop (1100011101011) last */
    enc(ESL_BC_CODE128, "A");
    CHECK(bit(0) && bit(1) && !bit(2) && bit(3) && !bit(4) && !bit(5) && bit(6));
    CHECK(bit(33) && bit(34) && !bit(35) && bit(45));
    CHECK_INT(enc(ESL_BC_CODE128, ""), 0);
    CHECK_INT(barcode_encode(ESL_BC_CODE128, hi, 2, bits, W), 0);   /* æ is not ASCII */
    CHECK_INT(barcode_encode(ESL_BC_CODE128, (const uint8_t *)"ABCDEFGHIJ", 10, bits, 100), 0); /* too wide */
    CHECK_INT(barcode_encode(9, (const uint8_t *)"1", 1, bits, W), 0);  /* unknown symbology */
}

/* ---- the layout element ---- */

static esl_layout_bin_t bin;

/* A file with one layout for the 2.9": a barcode element in the box x..x+w, rows 20..79 */
static void barcode_layout(const char *sym, int module, const char *align, int x, int w)
{
    char json[512], err[ESL_LAYOUT_ERR_MAX];
    snprintf(json, sizeof(json),
             "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"bc\","
             "\"fields\":[\"code\"],\"elements\":[{\"type\":\"barcode\",\"field\":\"code\",\"symbology\":\"%s\","
             "\"module\":%d,\"align\":\"%s\",\"x\":%d,\"y\":20,\"w\":%d,\"h\":60}]}]}",
             sym, module, align, x, w);
    if (!esl_layout_compile(json, strlen(json), ESL_MODEL_29, &bin, err, sizeof(err))) {
        printf("  %s\n", err);
        CHECK(0);
    }
}

/* Draws text with the layout; returns how many pixels are black */
static int draw(const char *text)
{
    static uint8_t out[W * ((H + 7) / 8)];
    esl_content_t c;
    int x, y, ink = 0;

    esl_content_init(&c, ESL_LAYOUT_FILE, 0);
    esl_content_add_field(&c, (const uint8_t *)text, strlen(text));
    CHECK_INT(esl_render(ESL_MODEL_29, &bin, &c, out, sizeof(out)), sizeof(out));
    for (x = 0; x < W; x++)
        for (y = 0; y < H; y++) {
            img[y][x] = (out[x * ((H + 7) / 8) + (y >> 3)] >> (7 - (y & 7))) & 1;
            ink += img[y][x];
        }
    return ink;
}

static void save(const char *name, const char *sym, const char *text)
{
    const char *dir = getenv("ESL_TEST_OUT");
    char path[512];
    FILE *fp;
    int x, y;

    if (!dir)
        dir = "build/barcodes";
    snprintf(path, sizeof(path), "%s/2.9-%s.pbm", dir, name);
    fp = fopen(path, "wb");
    if (!fp)
        return;
    fprintf(fp, "P1\n%d %d\n", W, H);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++)
            fputc(img[y][x] ? '1' : '0', fp);
        fputc('\n', fp);
    }
    fclose(fp);
    if (list)
        fprintf(list, "2.9-%s.pbm\t%s\t%s\n", name, sym, text);
}

/* First and last black column in row y, and the narrowest bar */
static void span(int y, int *first, int *last, int *narrow)
{
    int x, run = 0;
    *first = -1;
    *last = -1;
    *narrow = 1000;
    for (x = 0; x <= W; x++) {
        if (x < W && img[y][x]) {
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
    int first, last, narrow, mw = W / 113;
    const char *dir = getenv("ESL_TEST_OUT");
    char path[512];

    snprintf(path, sizeof(path), "%s/2.9-list.txt", dir ? dir : "build/barcodes");
    list = fopen(path, "w");

    /* EAN-13, widest module that fits, centred: bars only inside the box rows */
    barcode_layout("ean13", 0, "center", 0, W);
    draw("590123412345");
    span(50, &first, &last, &narrow);
    CHECK_INT(narrow, mw);
    CHECK_INT(last - first + 1, 95 * mw);
    CHECK_INT(first, (W - 113 * mw) / 2 + 11 * mw);
    span(19, &first, &last, &narrow);
    CHECK_INT(first, -1);
    span(80, &first, &last, &narrow);
    CHECK_INT(first, -1);
    span(79, &first, &last, &narrow);
    CHECK(first >= 0);
    save("ean13", "ean13", "5901234123457");

    CHECK(draw("8713331002910") > 0);
    save("ean13-b", "ean13", "8713331002910");

    /* invalid content: nothing drawn */
    CHECK_INT(draw("8713331002911"), 0);

    /* Code 128, module 1, left */
    barcode_layout("code128", 1, "left", 0, W);
    draw("Hello-123");
    span(50, &first, &last, &narrow);
    CHECK_INT(first, 10);
    CHECK_INT(last - first + 1, 11 * 11 + 13);       /* the code starts and ends with a bar */
    save("c128-text", "code128", "Hello-123");

    barcode_layout("code128", 0, "right", 0, W);
    draw("1234567");
    span(50, &first, &last, &narrow);
    CHECK_INT(last, W - 1 - 10 * narrow);
    save("c128-digits", "code128", "1234567");

    /* too wide for the box with 1-pixel modules: nothing */
    barcode_layout("code128", 0, "left", 0, 60);
    CHECK_INT(draw("ABCDEFGH"), 0);

    if (list)
        fclose(list);
    list = NULL;
}

void test_barcode(void)
{
    test_ean13();
    test_code128();
    test_element();
}
