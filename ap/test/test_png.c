#include "esl_crc32.h"
#include "esl_png.h"
#include "png_fixtures.h"
#include "tinytest.h"
#include <string.h>

/* The file is handed out in pieces of `piece` bytes at most, as a body arrives */
typedef struct {
    const uint8_t *data;
    size_t len, pos;
    uint32_t w, h, rows, crc;
    uint32_t want_w;   /* header refuses another width; 0 takes any */
    uint32_t stop_at;  /* row stops the decoding there; 0 never */
} src_t;

static bool src_read(void *ctx, uint8_t *buf, size_t n)
{
    src_t *s = ctx;
    if (n > s->len - s->pos)
        return false;
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return true;
}

static bool src_header(void *ctx, uint32_t w, uint32_t h)
{
    src_t *s = ctx;
    s->w = w;
    s->h = h;
    return !s->want_w || w == s->want_w;
}

static bool src_row(void *ctx, uint32_t y, const uint8_t *rgb)
{
    src_t *s = ctx;
    if (y != s->rows || (s->stop_at && y == s->stop_at))
        return false;
    s->rows++;
    s->crc = esl_crc32_update(s->crc, rgb, 3 * (size_t)s->w);
    return true;
}

static esl_png_err_t decode(src_t *s)
{
    esl_png_io_t io = {s, src_read, src_header, src_row};
    return esl_png_decode(&io);
}

void test_png(void)
{
    for (size_t i = 0; i < sizeof(png_good) / sizeof(png_good[0]); i++) {
        src_t s = {png_good[i].data, png_good[i].len, 0, 0, 0, 0, 0, 0, 0};
        esl_png_err_t err = decode(&s);
        if (err != ESL_PNG_OK)
            printf("  %s: %s\n", png_good[i].name, esl_png_err_text(err));
        CHECK_INT(err, ESL_PNG_OK);
        CHECK_INT(s.w, png_good[i].w);
        CHECK_INT(s.h, png_good[i].h);
        CHECK_INT(s.rows, png_good[i].h);
        if (s.crc != png_good[i].crc)
            printf("  %s: rows differ\n", png_good[i].name);
        CHECK_INT(s.crc, png_good[i].crc);
    }
    for (size_t i = 0; i < sizeof(png_bad) / sizeof(png_bad[0]); i++) {
        src_t s = {png_bad[i].data, png_bad[i].len, 0, 0, 0, 0, 0, 0, 0};
        esl_png_err_t err = decode(&s);
        if (err != png_bad[i].err)
            printf("  %s: %s\n", png_bad[i].name, esl_png_err_text(err));
        CHECK_INT(err, png_bad[i].err);
    }

    /* the header callback refuses the size, the row callback stops it */
    src_t s = {png_good[0].data, png_good[0].len, 0, 0, 0, 0, 0, 5, 0};
    CHECK_INT(decode(&s), ESL_PNG_ERR_SIZE);
    CHECK_INT(s.rows, 0);
    src_t s2 = {png_good[0].data, png_good[0].len, 0, 0, 0, 0, 0, 0, 4};
    CHECK_INT(decode(&s2), ESL_PNG_ERR_STOPPED);
    CHECK_INT(s2.rows, 4);

    /* any cut of a file is an error, never a crash */
    for (size_t n = 0; n < png_good[1].len; n += 3) {
        src_t c = {png_good[1].data, n, 0, 0, 0, 0, 0, 0, 0};
        CHECK(decode(&c) != ESL_PNG_OK);
    }
    /* and so is a flipped bit anywhere (the CRCs and Adler-32 catch it) */
    static uint8_t buf[2048];
    CHECK(png_good[5].len <= sizeof(buf));
    unsigned flipped_ok = 0;
    for (size_t i = 8; i < png_good[5].len && png_good[5].len <= sizeof(buf); i++) {
        memcpy(buf, png_good[5].data, png_good[5].len);
        buf[i] ^= (uint8_t)(1u << (i % 8));
        src_t c = {buf, png_good[5].len, 0, 0, 0, 0, 0, 0, 0};
        if (decode(&c) == ESL_PNG_OK)
            flipped_ok++;
    }
    CHECK_INT(flipped_ok, 0);
}
