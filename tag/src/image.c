#include "image.h"
#include "epd.h"
#include "hal/hal.h"

image_state_t __xdata image;

/* One SPI flash page; kept between wake-ups (XRAM survives deep sleep), so a download
   goes on where it stopped. Bytes dl_got & 0xFF of it are filled. */
static uint8_t __xdata page[IMG_DATA_OFF];
static const uint8_t __xdata zero = 0;

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
    return fmt == ESL_IMG_FMT_RLE && size && size < IMG_RAW_SIZE;
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
    return image.present ? ESL_POLL_F_IMAGE | ESL_POLL_F_IMG_RLE : 0;
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

/* ESL_IMG_FMT_RLE (shared/proto.h): the coded image at IMG_DATA_OFF decoded to
   IMG_RAW_OFF through page[]. False unless it gives exactly IMG_RAW_SIZE bytes. */
static uint8_t __xdata rd_buf[32];
static uint16_t rd_pos, wr_pos;
static uint8_t rd_i, rd_n;

static bool rd(uint8_t *b)
{
    if (rd_i == rd_n) {
        if (rd_pos >= image.dl_size)
            return false;
        rd_n = image.dl_size - rd_pos < sizeof(rd_buf) ? (uint8_t)(image.dl_size - rd_pos) : sizeof(rd_buf);
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
    if (wr_pos != IMG_RAW_SIZE)
        return false;
    n = wr_pos & (IMG_DATA_OFF - 1);
    return !n || flush_at(IMG_SLOT + IMG_RAW_OFF + (wr_pos & ~(IMG_DATA_OFF - 1)), n);
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
    if (r->img_id != image.dl_id || r->lay_size != image.dl_size || r->img_fmt != image.dl_fmt) {
        /* A new image (or it changed on the AP): start from the beginning */
        if (r->fw_model != MODEL_CODE || !size_ok(r->img_fmt, r->lay_size))
            return fail(r->img_id);
        image.dl_id = r->img_id;
        image.dl_size = r->lay_size;
        image.dl_fmt = r->img_fmt;
        image.dl_got = 0;
        image.slot_id = 0;                     /* the download overwrites the slot */
        if (!hal_xflash_erase(IMG_SLOT))       /* the header sector, so no old header survives */
            return fail(r->img_id);
    }
    pos = r->lay_pos;
    n = r->field_len[0];
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
        (image.dl_fmt == ESL_IMG_FMT_RLE && !rle_decode()) || !write_header())
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

void image_forget(void)
{
    if (!image.slot_id)
        return;
    image.slot_id = 0;
    hal_xflash_open();
    hal_xflash_write(IMG_SLOT + IMG_HDR_OFF_MAGIC, &zero, 1);   /* 0xFF -> 0: no erase needed */
    hal_xflash_close();
}

void image_column(uint16_t x, uint8_t __xdata *black, uint8_t __xdata *red)
{
    uint32_t a = IMG_SLOT + (image.slot_fmt == ESL_IMG_FMT_RLE ? IMG_RAW_OFF : IMG_DATA_OFF) +
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
