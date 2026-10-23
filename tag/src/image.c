#include "image.h"
#include "epd.h"
#include "hal/hal.h"

image_state_t __xdata image;

/* One SPI flash page; kept between wake-ups (XRAM survives deep sleep), so a download
   goes on where it stopped. Bytes dl_got & 0xFF of it are filled. */
static uint8_t __xdata page[IMG_DATA_OFF];

/* CRC-32 of len bytes of the SPI flash from addr, read through page[], as an image id:
   a CRC of 0 counts as 1 (the AP sends it so, since id 0 means "no image"). */
static uint32_t xflash_crc(uint32_t addr, uint16_t len)
{
    uint32_t crc = 0;
    uint16_t n;

    while (len) {
        n = len < sizeof(page) ? len : sizeof(page);
        hal_xflash_read(addr, page, n);
        crc = proto_crc32(crc, page, n);
        addr += n;
        len -= n;
    }
    return crc ? crc : 1;
}

/* A size that fits the format: exactly the screen raw, shorter coded. */
static bool size_ok(uint8_t fmt, uint16_t size)
{
    if (fmt == ESL_IMG_FMT_RAW)
        return size == IMG_RAW_SIZE;
    return (fmt == ESL_IMG_FMT_RLE || fmt == ESL_IMG_FMT_2D) && size && size < IMG_RAW_SIZE;
}

/* The id of the image the slot holds whole, or 0. The flash must be open. */
static uint32_t slot_check(void)
{
    uint16_t size;
    uint32_t id;

    hal_xflash_read(IMG_SLOT, page, IMG_HDR_LEN);
    size = ESL_GET_U16(page + IMG_HDR_OFF_SIZE);
    id = ESL_GET_U32(page + IMG_HDR_OFF_ID);
    image.slot_fmt = page[IMG_HDR_OFF_FMT];
    if (ESL_GET_U32(page + IMG_HDR_OFF_MAGIC) != IMG_HDR_MAGIC || page[IMG_HDR_OFF_FORMAT] != IMG_HDR_FORMAT ||
        page[IMG_HDR_OFF_MODEL] != MODEL_CODE || !size_ok(image.slot_fmt, size) || id == 0)
        return 0;
    return xflash_crc(IMG_SLOT + IMG_DATA_OFF, size) == id ? id : 0;
}

void image_init(uint8_t present)
{
    uint8_t i;

    for (i = 0; i < sizeof(image); i++)
        ((uint8_t __xdata *)&image)[i] = 0;
    image.present = present;
    if (!present || !hal_xflash_open())
        return;
    image.slot_id = slot_check();
    hal_xflash_close();
}

uint8_t image_poll_flags(void)
{
    return image.present ? ESL_POLL_F_IMAGE | ESL_POLL_F_IMG_RLE | ESL_POLL_F_IMG_2D : 0;
}

void image_open(void)
{
    hal_xflash_open();
}

void image_close(void)
{
    hal_xflash_close();
}

static bool write_header(void)
{
    uint8_t i;

    for (i = 0; i < IMG_HDR_LEN; i++)
        page[i] = 0;
    ESL_PUT_U32(page + IMG_HDR_OFF_MAGIC, IMG_HDR_MAGIC);
    page[IMG_HDR_OFF_FORMAT] = IMG_HDR_FORMAT;
    page[IMG_HDR_OFF_MODEL] = MODEL_CODE;
    page[IMG_HDR_OFF_FMT] = image.dl_fmt;
    ESL_PUT_U16(page + IMG_HDR_OFF_SIZE, image.dl_size);
    ESL_PUT_U32(page + IMG_HDR_OFF_ID, image.dl_id);
    return hal_xflash_write(IMG_SLOT, page, IMG_HDR_LEN);
}

/* Programs page[] (filled up to n bytes) at a, the start of a flash page. */
static bool flush_at(uint32_t a, uint16_t n)
{
    if ((a & (IMG_DATA_OFF * 16 - 1)) == 0 && !hal_xflash_erase(a))   /* a new 4 KB sector */
        return false;
    return hal_xflash_write(a, page, n);
}

/* ... as the downloaded image page that starts at pos. */
static bool flush(uint16_t pos, uint16_t n)
{
    return flush_at(IMG_SLOT + IMG_DATA_OFF + pos, n);
}

/* A coded image (shorter than the screen) ends before IMG_RAW_OFF, and the decoded one
   fits in the 64 KB slot */
#if IMG_DATA_OFF + DRAW_W * DRAW_COL_BYTES * (1 + MODEL_HAS_RED) - 1 > IMG_RAW_OFF || \
    IMG_RAW_OFF + DRAW_W * DRAW_COL_BYTES * (1 + MODEL_HAS_RED) > 0x10000
#error "IMG_RAW_OFF does not fit this model's image"
#endif

/* A coded image (shared/proto.h) at IMG_DATA_OFF is decoded to IMG_RAW_OFF through
   page[]: rd() reads the code, wr() writes the image. False unless it gives exactly
   IMG_RAW_SIZE bytes. */
static uint8_t __xdata rd_buf[32];
static uint16_t rd_pos, wr_pos;
static uint8_t rd_i, rd_n;

static bool rd(uint8_t *b)
{
    if (rd_i == rd_n) {
        if (rd_pos >= image.dl_size)
            return false;
        rd_n = image.dl_size - rd_pos < (uint16_t)sizeof(rd_buf) ? (uint8_t)(image.dl_size - rd_pos) : sizeof(rd_buf);
        hal_xflash_read(IMG_SLOT + IMG_DATA_OFF + rd_pos, rd_buf, rd_n);
        rd_pos += rd_n;
        rd_i = 0;
    }
    *b = rd_buf[rd_i++];
    return true;
}

static bool wr(uint8_t b)
{
    if (wr_pos >= IMG_RAW_SIZE)
        return false;
    page[wr_pos & (IMG_DATA_OFF - 1)] = b;
    wr_pos++;
    return (wr_pos & (IMG_DATA_OFF - 1)) || flush_at(IMG_SLOT + IMG_RAW_OFF + wr_pos - IMG_DATA_OFF, IMG_DATA_OFF);
}

/* The last, partial page of the image; false unless it is exactly IMG_RAW_SIZE bytes */
static bool wr_end(void)
{
    uint8_t n;

    if (wr_pos != IMG_RAW_SIZE)
        return false;
    n = wr_pos & (IMG_DATA_OFF - 1);
    return !n || flush_at(IMG_SLOT + IMG_RAW_OFF + (wr_pos & ~(IMG_DATA_OFF - 1)), n);
}

static bool rle_decode(void)
{
    uint8_t c, b, n;

    rd_pos = wr_pos = 0;
    rd_i = rd_n = 0;
    while (rd(&c)) {
        if (c < 0x80) {
            for (n = c + 1; n; n--)
                if (!rd(&b) || !wr(b))
                    return false;
        } else {
            if (!rd(&b))
                return false;
            for (n = c - 0x80 + 2; n; n--)
                if (!wr(b))
                    return false;
        }
    }
    return wr_end();
}

/* ESL_IMG_FMT_2D: column by column, each against the one before. A column is kept as
   the rows where its colour changes, in order (ink from the first to the second, from
   the third to the fourth, ...), ended by DRAW_H three times: the column before is the
   reference, the one being decoded becomes the next. */
static uint8_t __xdata chg[2][DRAW_H + 3];
static uint8_t __xdata cur[DRAW_COL_BYTES];
static __code const uint8_t ink_from[8] = {0xFF, 0x7F, 0x3F, 0x1F, 0x0F, 0x07, 0x03, 0x01};

/* The code is read a bit at a time from the top of bit_buf, bit_n bits left in it; bit_ok
   is cleared past the end (the bits read are then 0). Inline, since it runs for every
   bit: a call per bit costs the 8051 more than the rest of the decoder. In internal RAM
   (__data), like the state below, which the 8051 reaches several times faster than
   external RAM. */
static uint8_t __data bit_buf, bit_n, bit_v, bit_ok;
#define BIT() (bit_n ? (bit_n--, bit_v = bit_buf >> 7, bit_buf <<= 1, bit_v) : bit_load())

static uint8_t bit_load(void)
{
    if (!rd(&bit_buf)) {
        bit_ok = 0;
        return 0;
    }
    bit_n = 7;
    bit_v = bit_buf >> 7;
    bit_buf <<= 1;
    return bit_v;
}

/* A run length (Exp-Golomb, order 2), 0xFFFF if the code is broken */
static uint16_t run(void)
{
    uint8_t z = 0;
    uint16_t v = 1;

    while (!BIT())
        if (++z > 8 || !bit_ok)
            return 0xFFFF;
    for (z += 2; z; z--)
        v = (v << 1) | BIT();
    return bit_ok ? v - 4 : 0xFFFF;
}

/* Ink in cur from row y0 up to y1, a byte at a time where it can */
static void ink(uint8_t y0, uint8_t y1)
{
    uint8_t i = y0 >> 3, e = y1 >> 3;

    if (y0 >= y1)
        return;
    if (i == e) {
        cur[i] |= ink_from[y0 & 7] & ~ink_from[y1 & 7];
        return;
    }
    cur[i++] |= ink_from[y0 & 7];
    while (i < e)
        cur[i++] = 0xFF;
    if (y1 & 7)
        cur[e] |= ~ink_from[y1 & 7];
}

/* The decoder's state, in internal RAM as the bits above */
static uint8_t __xdata *__data rl, *__data cl;
static uint8_t __data lo, k, n, col, b1, same;

static bool decode_2d(void)
{
    uint16_t x, r1, r2;
    uint8_t __xdata *t;
    /* a0 is lo - 1: lo = 0 is a0 = -1 above the top, and the column is done at DRAW_H + 1 */
    uint8_t s, i, d, a1;

    rd_pos = wr_pos = 0;
    rd_i = rd_n = 0;
    bit_n = 0;
    bit_ok = 1;
    rl = chg[0];
    cl = chg[1];
    rl[0] = rl[1] = rl[2] = DRAW_H;               /* before the first column: no ink */
    for (i = 0; i < DRAW_COL_BYTES; i++)
        cur[i] = 0;
    for (x = 0; x < IMG_RAW_SIZE / DRAW_COL_BYTES; x++) {
        lo = 0;
        k = 0;
        n = 0;
        col = 0;
        same = 0;
        while (lo <= DRAW_H) {
            /* b1: the first change below a0 in the reference to the other colour; after
               change k the colour is ink when k is even. After vertical 0 it is the next
               change in the reference (same), so a column like the one before goes fast. */
            if (!same) {
                while (k && rl[k - 1] >= lo)
                    k--;
                while (rl[k] < lo)
                    k++;
                if ((k & 1) != col)
                    k++;
                b1 = rl[k];
            }
            same = 0;
            /* the mode: the number of zeros before the first 1 */
            for (d = 0; !BIT(); d++)
                if (d == 5 || !bit_ok)
                    return false;
            if (d == 0) {                                   /* 1: vertical 0, the common case */
                if (b1 < DRAW_H) {
                    cl[n++] = b1;
                    col ^= 1;
                    lo = b1 + 1;
                    b1 = rl[++k];
                    same = 1;
                } else
                    lo = DRAW_H + 1;
                continue;
            } else if (d == 2) {                            /* 001 horizontal */
                s = lo ? lo - 1 : 0;
                r1 = run();
                r2 = run();
                if (r1 > DRAW_H || r2 > DRAW_H || s + r1 + r2 > DRAW_H || s + r1 < lo || s + r1 + r2 < lo)
                    return false;
                a1 = s + (uint8_t)r1;
                if (a1 < DRAW_H) {
                    cl[n++] = a1;
                    if (a1 + r2 < DRAW_H) {
                        if (!r2)
                            return false;
                        cl[n++] = a1 + (uint8_t)r2;
                    }
                }
                lo = a1 + (uint8_t)r2 + 1;
                continue;
            } else if (d == 3) {                            /* 0001 pass: a0 = b2 */
                if (rl[k + 1] >= DRAW_H)
                    return false;
                lo = rl[k + 1] + 1;
                continue;
            } else {
                /* vertical: 01x, 00001x, 000001x; x = 1 below b1, 0 above */
                if (d != 1 && d < 4)
                    return false;
                d = d == 1 ? 1 : d - 2;
                if (BIT()) {
                    if (b1 + d > DRAW_H)
                        return false;
                    a1 = b1 + d;
                } else {
                    if (b1 < d)
                        return false;
                    a1 = b1 - d;
                }
            }
            if (a1 < lo || !bit_ok)
                return false;
            if (a1 < DRAW_H)
                cl[n++] = a1;
            col ^= 1;
            lo = a1 + 1;
        }
        cl[n] = cl[n + 1] = cl[n + 2] = DRAW_H;
        for (i = 0; i < n; i += 2)
            ink(cl[i], cl[i + 1]);
        for (i = 0; i < DRAW_COL_BYTES; i++) {
            if (!wr(cur[i]))
                return false;
            cur[i] = 0;                             /* for the next column */
        }
        t = rl;
        rl = cl;
        cl = t;
    }
    /* nothing after the code but the zeros that end its last byte */
    if (!bit_ok || rd_pos != image.dl_size || rd_i != rd_n || bit_buf)
        return false;
    return wr_end();
}

static uint8_t fail(uint32_t id)
{
    image.rejected = id;    /* reported with ESL_POLL_F_DRAWFAIL, so the AP stops offering it */
    image.dl_id = 0;
    return IMAGE_STOP;
}

uint8_t image_block(const proto_reply_t *r, const uint8_t *d)
{
    uint8_t i, n;
    uint16_t pos;

    if (!image.present || r->img_id == 0 || r->img_id == image.rejected)
        return IMAGE_STOP;
    if (r->img_id == image.slot_id)
        return IMAGE_DONE;                     /* already here: show it again */
    if (r->img_id != image.dl_id || r->size != image.dl_size || r->img_fmt != image.dl_fmt) {
        /* A new image (or it changed on the AP): start from the beginning */
        if (r->fw_model != MODEL_CODE || !size_ok(r->img_fmt, r->size))
            return fail(r->img_id);
        image.dl_id = r->img_id;
        image.dl_size = r->size;
        image.dl_fmt = r->img_fmt;
        image.dl_got = 0;
        image.slot_id = 0;                     /* the download overwrites the slot */
        if (!hal_xflash_erase(IMG_SLOT))       /* the header sector, so no old header survives */
            return fail(r->img_id);
    }
    pos = r->pos;
    n = r->data_len;
    if (pos > image.dl_got || pos + n <= image.dl_got)
        return IMAGE_MORE;                     /* not the block we asked for */
    for (i = (uint8_t)(image.dl_got - pos); i < n; i++) {
        page[image.dl_got & (IMG_DATA_OFF - 1)] = d[i];
        image.dl_got++;
        if ((image.dl_got & (IMG_DATA_OFF - 1)) == 0 && !flush(image.dl_got - IMG_DATA_OFF, IMG_DATA_OFF))
            return fail(image.dl_id);
    }
    if (image.dl_got < image.dl_size)
        return IMAGE_MORE;
    /* Complete: the last, partial page, then check what the flash holds and write the
       header, which marks the slot as whole. */
    if (((image.dl_got & (IMG_DATA_OFF - 1)) != 0 &&
         !flush(image.dl_got & ~(IMG_DATA_OFF - 1), image.dl_got & (IMG_DATA_OFF - 1))) ||
        xflash_crc(IMG_SLOT + IMG_DATA_OFF, image.dl_size) != image.dl_id ||
        (image.dl_fmt == ESL_IMG_FMT_RLE && !rle_decode()) || (image.dl_fmt == ESL_IMG_FMT_2D && !decode_2d()) ||
        !write_header())
        return fail(image.dl_id);
    image.slot_id = image.dl_id;
    image.slot_fmt = image.dl_fmt;
    image.dl_id = 0;
    return IMAGE_DONE;
}

bool image_show(void)
{
    bool ok;

    if (!image.slot_id || !hal_xflash_open())
        return false;
    ok = epd_draw_image();
    hal_xflash_close();
    return ok;
}

void image_column(uint16_t x, uint8_t __xdata *black, uint8_t __xdata *red)
{
    uint32_t a = IMG_SLOT + (image.slot_fmt != ESL_IMG_FMT_RAW ? IMG_RAW_OFF : IMG_DATA_OFF) +
                 (uint32_t)x * DRAW_COL_BYTES;
    uint8_t i;

    hal_xflash_read(a, black, DRAW_COL_BYTES);
#if MODEL_HAS_RED
    hal_xflash_read(a + (uint32_t)DRAW_W * DRAW_COL_BYTES, red, DRAW_COL_BYTES);
#else
    for (i = 0; i < DRAW_COL_BYTES; i++)
        red[i] = 0;
#endif
}
