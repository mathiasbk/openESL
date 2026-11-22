#include "esl_gicisky.h"
#include <string.h>

/* The framebuffer is 128 columns x 296 rows. Logical (x, y) is framebuffer row 295 - x,
   column y: the logical image rotated 90 degrees counter-clockwise. */
static void put_pixel(uint8_t *fb, unsigned x, unsigned y, uint8_t v)
{
    unsigned idx = (ESL_GICISKY_WIDTH - 1u - x) * ESL_GICISKY_HEIGHT + y;
    fb[idx >> 2] |= (uint8_t)(v << (6u - 2u * (idx & 3u)));
}

bool esl_gicisky_pack_bw_red(const uint8_t *planes, size_t len, uint8_t *fb)
{
    if (!planes || !fb || len != ESL_GICISKY_PLANES_SIZE)
        return false;
    const unsigned plane = ESL_GICISKY_WIDTH * ESL_GICISKY_COL_BYTES;
    memset(fb, 0, ESL_GICISKY_FB_SIZE);
    for (unsigned x = 0; x < ESL_GICISKY_WIDTH; x++) {
        for (unsigned y = 0; y < ESL_GICISKY_HEIGHT; y++) {
            unsigned bit = 7u - (y & 7u);
            unsigned off = x * ESL_GICISKY_COL_BYTES + (y >> 3);
            bool black = (planes[off] & (1u << bit)) != 0;
            bool red = (planes[plane + off] & (1u << bit)) != 0;
            put_pixel(fb, x, y, red ? ESL_GICISKY_RED : black ? ESL_GICISKY_BLACK : ESL_GICISKY_WHITE);
        }
    }
    return true;
}

bool esl_gicisky_pack_logical_2bpp(const uint8_t *logical, size_t len, uint8_t *fb)
{
    if (!logical || !fb || len != ESL_GICISKY_FB_SIZE)
        return false;
    memset(fb, 0, ESL_GICISKY_FB_SIZE);
    for (unsigned y = 0; y < ESL_GICISKY_HEIGHT; y++) {
        for (unsigned x = 0; x < ESL_GICISKY_WIDTH; x++) {
            unsigned idx = y * ESL_GICISKY_WIDTH + x;
            uint8_t v = (uint8_t)((logical[idx >> 2] >> (6u - 2u * (idx & 3u))) & 3u);
            put_pixel(fb, x, y, v);
        }
    }
    return true;
}

static bool cmd(const esl_gicisky_io_t *io, const uint8_t *data, size_t len)
{
    return io->write(io->ctx, ESL_GICISKY_CMD, data, len);
}

static bool ack(const esl_gicisky_io_t *io, const uint8_t *prefix, size_t len)
{
    return io->wait(io->ctx, prefix, len);
}

esl_gicisky_res_t esl_gicisky_send(const esl_gicisky_io_t *io, const uint8_t *fb, size_t len)
{
    static const uint8_t start[] = {0x01};
    static const uint8_t start_ack[] = {0x01, 0xf4, 0x00};
    static const uint8_t info[] = {0x02, 0x00, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t info_ack[] = {0x02, 0x00, 0x25};
    static const uint8_t begin[] = {0x03};
    static const uint8_t begin_ack[] = {0x05, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t done_ack[] = {0x05, 0x08, 0x00, 0x00, 0x00, 0x00};

    if (!io || !io->write || !io->wait || !fb || len != ESL_GICISKY_FB_SIZE)
        return ESL_GICISKY_AWAY;

    if (!cmd(io, start, sizeof start) || !ack(io, start_ack, sizeof start_ack))
        return ESL_GICISKY_AWAY;
    if (!cmd(io, info, sizeof info) || !ack(io, info_ack, sizeof info_ack))
        return ESL_GICISKY_AWAY;
    if (!cmd(io, begin, sizeof begin) || !ack(io, begin_ack, sizeof begin_ack))
        return ESL_GICISKY_AWAY;

    for (uint32_t i = 0; i < ESL_GICISKY_CHUNKS; i++) {
        uint8_t chunk[4 + ESL_GICISKY_CHUNK];
        size_t n = i + 1 == ESL_GICISKY_CHUNKS ? ESL_GICISKY_LAST_CHUNK : ESL_GICISKY_CHUNK;
        chunk[0] = (uint8_t)(i & 0xffu);
        chunk[1] = (uint8_t)((i >> 8) & 0xffu);
        chunk[2] = (uint8_t)((i >> 16) & 0xffu);
        chunk[3] = (uint8_t)((i >> 24) & 0xffu);
        memcpy(chunk + 4, fb + (size_t)i * ESL_GICISKY_CHUNK, n);
        if (!io->write(io->ctx, ESL_GICISKY_DATA, chunk, 4 + n))
            return ESL_GICISKY_AWAY;
        if (i + 1 == ESL_GICISKY_CHUNKS) {
            if (!ack(io, done_ack, sizeof done_ack))
                return ESL_GICISKY_AWAY;
        } else {
            uint8_t next[6] = {0x05, 0x00, (uint8_t)(i + 1), 0x00, 0x00, 0x00};
            if (!ack(io, next, sizeof next))
                return ESL_GICISKY_AWAY;
        }
    }
    return ESL_GICISKY_SENT;
}
