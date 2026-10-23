/* test_barcode.c - Code 128 and EAN-13 encoding (tag/src/barcode.c), and the barcode layout
 * element as esl_render draws it. The screens are also written as PBM images to
 * $ESL_TEST_OUT (build/barcodes by default), where test_barcode_decode.py reads them back
 * with a barcode decoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "barcode.h"
#include "esl_qr.h"
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

/* The same with a QR element in the box x, y, w x h */
static void qr_layout(const char *ecc, int module, const char *align, int x, int y, int w, int h)
{
    char json[512], err[ESL_LAYOUT_ERR_MAX];
    snprintf(json, sizeof(json),
             "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"qr\","
             "\"fields\":[\"url\"],\"elements\":[{\"type\":\"qr\",\"field\":\"url\",\"ecc\":\"%s\","
             "\"module\":%d,\"align\":\"%s\",\"valign\":\"middle\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}]}]}",
             ecc, module, align, x, y, w, h);
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

    /* QR codes: every level, versions 1 to 10, module sizes; read back by the decoder */
    qr_layout("M", 0, "center", 0, 0, W, H);
    draw("https://openesl.example/p/123");
    save("qr-url", "qr", "https://openesl.example/p/123");
    span(64, &first, &last, &narrow);
    CHECK(first > 0 && last < W - 1);
    CHECK_INT((last + first + 1) / 2, W / 2);   /* centred */
    CHECK_INT(bin.qr_field[0], 0);
    CHECK_INT(bin.qr_ecc[0], ESL_QR_LEVEL_M);
    CHECK_INT(bin.qr_cap[0], (int)esl_qr_capacity(10, ESL_QR_ECC_M)); /* 128 px holds version 10 (57) */
    qr_layout("L", 1, "left", 4, 4, 120, 120);
    draw("Ost 1 kg 129,90");
    save("qr-l1", "qr", "Ost 1 kg 129,90");
    span(4 + (120 - 21) / 2, &first, &last, &narrow);   /* in the middle of the box */
    CHECK_INT(first, 4);
    CHECK_INT(last - first + 1, 21);            /* version 1, 1 px, the top row of two finders */
    qr_layout("H", 2, "right", 0, 0, W, H);
    draw("HELLO WORLD 0123456789 abcdefghijklmnopqrstuvwxyz");
    save("qr-h2", "qr", "HELLO WORLD 0123456789 abcdefghijklmnopqrstuvwxyz");
    {
        /* 90 bytes at Q is version 8, with the version information */
        char longer[91];
        memset(longer, 'a', 90);
        memcpy(longer, "https://x.example/", 18);
        longer[90] = 0;
        qr_layout("Q", 0, "center", 0, 0, W, H);
        CHECK(draw(longer) > 0);
        save("qr-v8", "qr", longer);
    }
    /* longer than the box can hold: nothing */
    qr_layout("Q", 0, "left", 0, 0, 40, 40);
    CHECK_INT(bin.qr_cap[0], (int)esl_qr_capacity(5, ESL_QR_ECC_Q));   /* 37 modules fit 40 px */
    CHECK_INT(draw("https://openesl.example/a/rather/long/address/for/a/small/box/x/y"), 0);

    if (list)
        fclose(list);
    list = NULL;
}

/* Capacities in bytes from the standard's tables (ISO/IEC 18004, byte mode) */
static void test_qr_capacity(void)
{
    static const uint16_t cap[4][10] = {
        {17, 32, 53, 78, 106, 134, 154, 192, 230, 271},
        {14, 26, 42, 62, 84, 106, 122, 152, 180, 213},
        {11, 20, 32, 46, 60, 74, 86, 108, 130, 151},
        {7, 14, 24, 34, 44, 58, 64, 84, 98, 119},
    };
    static uint8_t bits[ESL_QR_BYTES];
    int same = 1;
    for (uint8_t l = 0; l < 4; l++)
        for (uint8_t v = 1; v <= 10; v++)
            if (esl_qr_capacity(v, l) != cap[l][v - 1])
                same = 0;
    CHECK(same);
    CHECK_INT(esl_qr_version(17, ESL_QR_ECC_L, 10), 1);
    CHECK_INT(esl_qr_version(18, ESL_QR_ECC_L, 10), 2);
    CHECK_INT(esl_qr_version(272, ESL_QR_ECC_L, 10), 0);
    CHECK_INT(esl_qr_version(100, ESL_QR_ECC_L, 4), 0);
    CHECK_INT(esl_qr_encode((const uint8_t *)"x", 1, ESL_QR_ECC_H, 10, bits), 21);
    CHECK_INT(esl_qr_encode((const uint8_t *)"x", 1, 4, 10, bits), 0);
    /* the finder in the top-left corner: a dark ring, light, a dark 3 x 3 centre */
    CHECK(esl_qr_module(bits, 21, 0, 0) && esl_qr_module(bits, 21, 6, 6) && !esl_qr_module(bits, 21, 1, 1) &&
          esl_qr_module(bits, 21, 3, 3) && !esl_qr_module(bits, 21, 7, 7) && esl_qr_module(bits, 21, 8, 13));
}

void test_barcode(void)
{
    test_ean13();
    test_code128();
    test_qr_capacity();
    test_element();
}
