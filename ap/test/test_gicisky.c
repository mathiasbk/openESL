#include "tinytest.h"
#include "esl_gicisky.h"

/* The Gicisky 2.9" BWRY tag: the two packers and the command/chunk sequence, with a
   fake transport that records the writes and answers the notifications. */

static uint8_t planes[ESL_GICISKY_PLANES_SIZE];
static uint8_t logical[ESL_GICISKY_FB_SIZE];
static uint8_t fb[ESL_GICISKY_FB_SIZE];

static uint8_t pixel(const uint8_t *buf, unsigned x, unsigned y)
{
    unsigned idx = (ESL_GICISKY_WIDTH - 1u - x) * ESL_GICISKY_HEIGHT + y;
    return (uint8_t)((buf[idx >> 2] >> (6u - 2u * (idx & 3u))) & 3u);
}

static void set_logical(uint8_t *buf, unsigned x, unsigned y, uint8_t v)
{
    unsigned idx = y * ESL_GICISKY_WIDTH + x;
    buf[idx >> 2] |= (uint8_t)(v << (6u - 2u * (idx & 3u)));
}

static void test_gicisky_pack(void)
{
    /* No ink: white everywhere (value 1 -> 0x55). */
    memset(planes, 0, sizeof planes);
    CHECK(esl_gicisky_pack_bw_red(planes, sizeof planes, fb));
    for (size_t i = 0; i < sizeof fb; i++)
        CHECK_INT(fb[i], 0x55);
    CHECK_INT(pixel(fb, 0, 0), ESL_GICISKY_WHITE);
    CHECK_INT(pixel(fb, 295, 127), ESL_GICISKY_WHITE);

    /* The black plane: black everywhere (value 0). */
    memset(planes, 0, sizeof planes);
    memset(planes, 0xff, ESL_GICISKY_WIDTH * ESL_GICISKY_COL_BYTES);
    CHECK(esl_gicisky_pack_bw_red(planes, sizeof planes, fb));
    for (size_t i = 0; i < sizeof fb; i++)
        CHECK_INT(fb[i], 0x00);
    CHECK_INT(pixel(fb, 0, 0), ESL_GICISKY_BLACK);

    /* The red plane: red everywhere (value 3 -> 0xff). */
    memset(planes, 0, sizeof planes);
    memset(planes + ESL_GICISKY_WIDTH * ESL_GICISKY_COL_BYTES, 0xff,
           ESL_GICISKY_WIDTH * ESL_GICISKY_COL_BYTES);
    CHECK(esl_gicisky_pack_bw_red(planes, sizeof planes, fb));
    for (size_t i = 0; i < sizeof fb; i++)
        CHECK_INT(fb[i], 0xff);
    CHECK_INT(pixel(fb, 0, 0), ESL_GICISKY_RED);

    /* A 60 x 60 black square at the logical top-left. It lands at framebuffer rows
       236-295, columns 0-59: row 236 (index 236 * 128 = 30208, byte 7552) starts black
       and turns white at column 60 (byte 7552 + 15). */
    memset(planes, 0, sizeof planes);
    for (unsigned x = 0; x < 60; x++)
        for (unsigned y = 0; y < 60; y++)
            planes[x * ESL_GICISKY_COL_BYTES + (y >> 3)] |= (uint8_t)(1u << (7u - (y & 7u)));
    CHECK(esl_gicisky_pack_bw_red(planes, sizeof planes, fb));
    CHECK_INT(pixel(fb, 0, 0), ESL_GICISKY_BLACK);
    CHECK_INT(pixel(fb, 59, 59), ESL_GICISKY_BLACK);
    CHECK_INT(pixel(fb, 60, 0), ESL_GICISKY_WHITE);
    CHECK_INT(pixel(fb, 0, 60), ESL_GICISKY_WHITE);
    CHECK_INT(fb[7552], 0x00);
    CHECK_INT(fb[7552 + 15], 0x55);
    CHECK_INT(fb[7520], 0x55); /* the row above the square is white */
    CHECK_INT(fb[9440], 0x00); /* the last row, columns 0-3 */
    CHECK_INT(fb[9455], 0x55); /* the last row, columns 60-63 */

    CHECK(!esl_gicisky_pack_bw_red(NULL, sizeof planes, fb));
    CHECK(!esl_gicisky_pack_bw_red(planes, sizeof planes - 1, fb));
    CHECK(!esl_gicisky_pack_bw_red(planes, sizeof planes, NULL));
}

static void test_gicisky_logical(void)
{
    /* The four values in the four logical quadrants. */
    memset(logical, 0, sizeof logical);
    for (unsigned y = 0; y < ESL_GICISKY_HEIGHT; y++)
        for (unsigned x = 0; x < ESL_GICISKY_WIDTH; x++)
            set_logical(logical, x, y, x < ESL_GICISKY_WIDTH / 2
                                          ? (y < ESL_GICISKY_HEIGHT / 2 ? ESL_GICISKY_BLACK : ESL_GICISKY_YELLOW)
                                          : (y < ESL_GICISKY_HEIGHT / 2 ? ESL_GICISKY_WHITE : ESL_GICISKY_RED));
    CHECK(esl_gicisky_pack_logical_2bpp(logical, sizeof logical, fb));
    CHECK_INT(pixel(fb, 0, 0), ESL_GICISKY_BLACK);
    CHECK_INT(pixel(fb, 295, 0), ESL_GICISKY_WHITE);
    CHECK_INT(pixel(fb, 0, 127), ESL_GICISKY_YELLOW);
    CHECK_INT(pixel(fb, 295, 127), ESL_GICISKY_RED);

    CHECK(!esl_gicisky_pack_logical_2bpp(NULL, sizeof logical, fb));
    CHECK(!esl_gicisky_pack_logical_2bpp(logical, sizeof logical - 1, fb));
    CHECK(!esl_gicisky_pack_logical_2bpp(logical, sizeof logical, NULL));
}

static struct {
    unsigned writes, waits;
    unsigned write_fail_at, wait_fail_at; /* 1-based; 0 = never */
    unsigned cmds, chunks;
    uint8_t cmd[3][8];
    size_t cmd_len[3];
    uint8_t first[4 + ESL_GICISKY_CHUNK];
    size_t first_len;
    uint8_t last[4 + ESL_GICISKY_CHUNK];
    size_t last_len;
} fake;

static void reset_fake(void)
{
    memset(&fake, 0, sizeof fake);
}

static bool fake_write(void *ctx, esl_gicisky_chan_t chan, const uint8_t *data, size_t len)
{
    (void)ctx;
    fake.writes++;
    if (fake.write_fail_at && fake.writes >= fake.write_fail_at)
        return false;
    if (chan == ESL_GICISKY_CMD) {
        if (fake.cmds < 3) {
            size_t n = len < sizeof fake.cmd[0] ? len : sizeof fake.cmd[0];
            memcpy(fake.cmd[fake.cmds], data, n);
            fake.cmd_len[fake.cmds] = len;
        }
        fake.cmds++;
    } else {
        if (fake.chunks == 0) {
            size_t n = len < sizeof fake.first ? len : sizeof fake.first;
            memcpy(fake.first, data, n);
            fake.first_len = len;
        }
        if (fake.chunks == ESL_GICISKY_CHUNKS - 1) {
            size_t n = len < sizeof fake.last ? len : sizeof fake.last;
            memcpy(fake.last, data, n);
            fake.last_len = len;
        }
        fake.chunks++;
    }
    return true;
}

static bool fake_wait(void *ctx, const uint8_t *prefix, size_t len)
{
    (void)ctx;
    unsigned n = fake.waits++;
    if (fake.wait_fail_at && n + 1 >= fake.wait_fail_at)
        return false;
    uint8_t exp[6];
    size_t elen;
    if (n == 0) {
        memcpy(exp, "\x01\xf4\x00", 3);
        elen = 3;
    } else if (n == 1) {
        memcpy(exp, "\x02\x00\x25", 3);
        elen = 3;
    } else if (n == 2) {
        memcpy(exp, "\x05\x00\x00\x00\x00\x00", 6);
        elen = 6;
    } else if (n < 2 + ESL_GICISKY_CHUNKS) {
        exp[0] = 0x05;
        exp[1] = 0x00;
        exp[2] = (uint8_t)(n - 2);
        exp[3] = exp[4] = exp[5] = 0x00;
        elen = 6;
    } else {
        memcpy(exp, "\x05\x08\x00\x00\x00\x00", 6);
        elen = 6;
    }
    CHECK_INT(len, elen);
    CHECK_MEM(prefix, exp, elen < len ? elen : len);
    return len == elen && memcmp(prefix, exp, elen) == 0;
}

static void test_gicisky_send(void)
{
    static const esl_gicisky_io_t io = {NULL, fake_write, fake_wait};
    for (size_t i = 0; i < sizeof fb; i++)
        fb[i] = (uint8_t)(i * 7u);

    reset_fake();
    CHECK_INT(esl_gicisky_send(&io, fb, sizeof fb), ESL_GICISKY_SENT);
    CHECK_INT(fake.writes, 3 + ESL_GICISKY_CHUNKS);
    CHECK_INT(fake.waits, 3 + ESL_GICISKY_CHUNKS);
    CHECK_INT(fake.cmds, 3);
    CHECK_INT(fake.cmd_len[0], 1);
    CHECK_INT(fake.cmd[0][0], 0x01);
    CHECK_INT(fake.cmd_len[1], 8);
    CHECK_MEM(fake.cmd[1], "\x02\x00\x25\x00\x00\x00\x00\x00", 8);
    CHECK_INT(fake.cmd_len[2], 1);
    CHECK_INT(fake.cmd[2][0], 0x03);
    CHECK_INT(fake.chunks, ESL_GICISKY_CHUNKS);
    CHECK_INT(fake.first_len, 4 + ESL_GICISKY_CHUNK);
    CHECK_INT(fake.first[0], 0);
    CHECK_INT(fake.first[1], 0);
    CHECK_INT(fake.first[2], 0);
    CHECK_INT(fake.first[3], 0);
    CHECK_MEM(fake.first + 4, fb, ESL_GICISKY_CHUNK);
    CHECK_INT(fake.last_len, 4 + ESL_GICISKY_LAST_CHUNK);
    CHECK_INT(fake.last[0], ESL_GICISKY_CHUNKS - 1);
    CHECK_INT(fake.last[1], 0);
    CHECK_INT(fake.last[2], 0);
    CHECK_INT(fake.last[3], 0);
    CHECK_MEM(fake.last + 4, fb + (ESL_GICISKY_CHUNKS - 1) * ESL_GICISKY_CHUNK, ESL_GICISKY_LAST_CHUNK);

    reset_fake();
    fake.write_fail_at = 3;
    CHECK_INT(esl_gicisky_send(&io, fb, sizeof fb), ESL_GICISKY_AWAY);
    reset_fake();
    fake.wait_fail_at = 2;
    CHECK_INT(esl_gicisky_send(&io, fb, sizeof fb), ESL_GICISKY_AWAY);
    CHECK_INT(esl_gicisky_send(NULL, fb, sizeof fb), ESL_GICISKY_AWAY);
    CHECK_INT(esl_gicisky_send(&io, fb, sizeof fb - 1), ESL_GICISKY_AWAY);
}

void test_gicisky(void)
{
    test_gicisky_pack();
    test_gicisky_logical();
    test_gicisky_send();
}
