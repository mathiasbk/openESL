/* test_image.c - screen images from the AP (docs/protokoll.md part 1, "Image transfer"):
 * the download into the SPI flash slot (image.c), the drawing from the slot, what
 * survives a restart, and the tag's side over the radio (app.c). */
#include <string.h>
#include "test.h"
#include "fake.h"
#include "helpers.h"
#include "app.h"
#include "config.h"
#include "model.h"
#include "ota.h"
#include "image.h"
#include "status.h"
#include "esl_img2d.h"

static uint8_t img[IMG_RAW_SIZE];
static uint32_t img_id;
static uint8_t pl[ESL_MAX_PAYLOAD], fr[128];

/* A picture: column x has the bits of (x * 3 + seed) in its first byte, a frame at the
   top row, and the rest of the bytes count. */
static void make_picture(uint8_t seed)
{
    uint16_t x;
    uint8_t k;

    for (x = 0; x < DRAW_W; x++)
        for (k = 0; k < DRAW_COL_BYTES; k++)
            img[x * DRAW_COL_BYTES + k] = k == 0 ? (uint8_t)(x * 3 + seed) | 0x80 : (uint8_t)(x + k + seed);
#if MODEL_HAS_RED
    memset(img + (uint16_t)DRAW_W * DRAW_COL_BYTES, 0, (uint16_t)DRAW_W * DRAW_COL_BYTES);
#endif
    img_id = proto_crc32(0, img, IMG_RAW_SIZE);
}

/* Sets the last 4 bytes of img so its CRC-32 is 0 (the id is then 1): the reflected
   CRC can be run backwards a byte at a time, since the top byte of each table entry
   is different. */
static uint32_t crc_table[256];

static void make_crc_zero(void)
{
    uint32_t c, s;
    uint16_t i;
    uint8_t idx[4], k, j;

    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
        crc_table[i] = c;
    }
    s = ~proto_crc32(0, img, IMG_RAW_SIZE - 4);   /* register before the last 4 bytes */
    c = 0xFFFFFFFFu;                              /* register that gives CRC 0 */
    for (k = 4; k--;) {
        for (j = 0; (crc_table[j] >> 24) != (c >> 24); j++)
            ;
        idx[k] = j;
        c = (c ^ crc_table[j]) << 8;
    }
    for (k = 0; k < 4; k++) {
        img[IMG_RAW_SIZE - 4 + k] = (uint8_t)((s ^ idx[k]) & 0xFF);
        s = crc_table[idx[k]] ^ (s >> 8);
    }
    img_id = 1;
}

/* fake_fb shows img (black plane) */
static bool screen_is_picture(void)
{
    uint16_t x;
    uint8_t y;

    for (x = 0; x < DRAW_W; x++)
        for (y = 0; y < DRAW_H; y++) {
            uint8_t b = img[x * DRAW_COL_BYTES + (y >> 3)] & (uint8_t)(0x80 >> (y & 7));
            if ((fake_fb[y][x] == 1) != (b != 0))
                return false;
        }
    return true;
}

static void setup(bool present)
{
    fake_xflash_clear(present);
    ota_init();
    image_init(ota.present);
}

/* One IMG block of img at pos, parsed as the tag does */
static uint8_t feed(uint32_t id, uint16_t size, uint16_t pos, uint8_t model, uint8_t fmt)
{
    proto_reply_t r;
    uint16_t left = size - pos;
    uint8_t n = (uint8_t)(left > ESL_IMG_CHUNK_MAX ? ESL_IMG_CHUNK_MAX : left);
    uint8_t len = make_img_block(pl, 0, id, size, pos, model, fmt, img + pos, n);

    memset(&r, 0, sizeof(r));
    CHECK_EQ(proto_parse_reply(pl, len, &r), PROTO_OK);
    return image_block(&r, pl + r.data_off);
}

/* The whole image, block by block as the tag asks for it */
static uint8_t download(void)
{
    uint8_t res;
    image_open();
    do {
        res = feed(img_id, IMG_RAW_SIZE, image.dl_got, MODEL_CODE, ESL_IMG_FMT_RAW);
    } while (res == IMAGE_MORE);
    image_close();
    return res;
}

static void test_refused(void)
{
    make_picture(1);
    /* No SPI flash: no flag, nothing taken */
    setup(false);
    CHECK_EQ(image_poll_flags(), 0);
    image_open();
    CHECK_EQ(feed(img_id, IMG_RAW_SIZE, 0, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_STOP);
    image_close();
    CHECK_EQ(image.rejected, 0);

    setup(true);
    CHECK_EQ(image_poll_flags(), ESL_POLL_F_IMAGE | ESL_POLL_F_IMG_RLE | ESL_POLL_F_IMG_2D);
    CHECK_EQ(image.slot_id, 0);
    image_open();
    CHECK_EQ(feed(img_id, IMG_RAW_SIZE, 0, MODEL_CODE + 1, ESL_IMG_FMT_RAW), IMAGE_STOP);   /* another model */
    CHECK(image.rejected == img_id);
    CHECK_EQ(feed(img_id, IMG_RAW_SIZE, 0, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_STOP);       /* still refused */
    CHECK_EQ(feed(7, IMG_RAW_SIZE, 0, MODEL_CODE, 1), IMAGE_STOP);                         /* unknown format */
    CHECK(image.rejected == 7);
    CHECK_EQ(feed(8, IMG_RAW_SIZE - 1, 0, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_STOP);       /* wrong size */
    CHECK_EQ(feed(0, IMG_RAW_SIZE, 0, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_STOP);           /* id 0 */
    image_close();
    CHECK_EQ(image.dl_id, 0);
    CHECK_EQ(fake_xflash_misuse, 0);
}

/* An image whose CRC-32 is 0 has id 1, on the AP and on the tag (protokoll.md) */
static void test_crc_zero(void)
{
    make_picture(7);
    make_crc_zero();
    CHECK(proto_crc32(0, img, IMG_RAW_SIZE) == 0);
    setup(true);
    CHECK_EQ(download(), IMAGE_DONE);
    CHECK(image.slot_id == 1);
    CHECK(image.rejected == 0);
    ota_init();
    image_init(ota.present);
    CHECK(image.slot_id == 1);       /* the slot check counts 0 as 1 too */
    CHECK(image_show());
    CHECK(screen_is_picture());
}

static void test_download(void)
{
    uint32_t first;

    make_picture(2);
    setup(true);
    fake_draw_count = 0;
    CHECK_EQ(download(), IMAGE_DONE);
    CHECK(image.slot_id == img_id);
    CHECK_EQ(image.dl_id, 0);
    CHECK(memcmp(fake_xflash + IMG_SLOT + IMG_DATA_OFF, img, IMG_RAW_SIZE) == 0);
    CHECK(ESL_GET_U32(fake_xflash + IMG_SLOT + IMG_HDR_OFF_MAGIC) == IMG_HDR_MAGIC);
    CHECK_EQ(ESL_GET_U16(fake_xflash + IMG_SLOT + IMG_HDR_OFF_SIZE), IMG_RAW_SIZE);
    CHECK(ESL_GET_U32(fake_xflash + IMG_SLOT + IMG_HDR_OFF_ID) == img_id);
    CHECK_EQ(fake_xflash[IMG_SLOT + IMG_HDR_OFF_MODEL], MODEL_CODE);
    /* The firmware slots are untouched */
    CHECK_EQ(fake_xflash[0x80000], 0xFF);
    CHECK_EQ(fake_xflash[0xA0000], 0xFF);

    CHECK(image_show());
    CHECK_EQ(fake_draw_count, 1);
    CHECK(screen_is_picture());
    CHECK(!fake_xflash_is_open);

    /* The same image offered again: no download, just DONE */
    image_open();
    CHECK_EQ(feed(img_id, IMG_RAW_SIZE, 0, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_DONE);
    image_close();

    /* A restart finds it in the slot */
    first = img_id;
    ota_init();
    image_init(ota.present);
    CHECK(image.slot_id == first);
    CHECK(image_show());
    CHECK(screen_is_picture());

    /* A damaged image is not */
    fake_xflash[IMG_SLOT + IMG_DATA_OFF + 100] ^= 0x40;
    image_init(1);
    CHECK_EQ(image.slot_id, 0);
    CHECK(!image_show());
    fake_xflash[IMG_SLOT + IMG_DATA_OFF + 100] ^= 0x40;
    image_init(1);
    CHECK(image.slot_id == first);

    /* A new picture replaces it; the old id is no longer in the slot */
    make_picture(3);
    CHECK(img_id != first);
    CHECK_EQ(download(), IMAGE_DONE);
    CHECK(image.slot_id == img_id);
    CHECK(image_show());
    CHECK(screen_is_picture());
    CHECK_EQ(fake_xflash_misuse, 0);
}

/* ESL_IMG_FMT_RLE: img coded as the AP does (esl_img_rle_encode) into code[] */
static uint8_t code[IMG_RAW_SIZE];
static uint16_t code_len;

static void rle_code(void)
{
    uint16_t i = 0, r, l;

    code_len = 0;
    while (i < IMG_RAW_SIZE) {
        for (r = 1; i + r < IMG_RAW_SIZE && r < 129 && img[i + r] == img[i]; r++)
            ;
        if (r >= 2) {
            code[code_len++] = (uint8_t)(0x80 + r - 2);
            code[code_len++] = img[i];
            i += r;
            continue;
        }
        for (l = 1; i + l < IMG_RAW_SIZE && l < 128 &&
                    !(i + l + 2 < IMG_RAW_SIZE && img[i + l] == img[i + l + 1] && img[i + l] == img[i + l + 2]);
             l++)
            ;
        code[code_len++] = (uint8_t)(l - 1);
        memcpy(code + code_len, img + i, l);
        code_len += l;
        i += l;
    }
    img_id = proto_crc32(0, code, code_len);
    if (!img_id)
        img_id = 1;
}

static uint8_t feed_fmt = ESL_IMG_FMT_RLE;   /* test_2d sends ESL_IMG_FMT_2D */

static uint8_t feed_code(uint16_t pos)
{
    proto_reply_t r;
    uint16_t left = code_len - pos;
    uint8_t n = (uint8_t)(left > ESL_IMG_CHUNK_MAX ? ESL_IMG_CHUNK_MAX : left);
    uint8_t len = make_img_block(pl, 0, img_id, code_len, pos, MODEL_CODE, feed_fmt, code + pos, n);

    memset(&r, 0, sizeof(r));
    CHECK_EQ(proto_parse_reply(pl, len, &r), PROTO_OK);
    return image_block(&r, pl + r.data_off);
}

static uint8_t download_code(void)
{
    uint8_t res;
    image_open();
    res = feed_code(0);                     /* the first block, as after a POLL */
    while (res == IMAGE_MORE)
        res = feed_code(image.dl_got);
    image_close();
    return res;
}

static void test_rle(void)
{
    uint16_t x;

    /* mostly white, with text-like columns: codes to a fraction */
    memset(img, 0, sizeof(img));
    for (x = 20; x < 60; x++)
        img[x * DRAW_COL_BYTES + 2] = (uint8_t)(x * 7);
    rle_code();
    CHECK(code_len < IMG_RAW_SIZE / 4);
    setup(true);
    CHECK_EQ(download_code(), IMAGE_DONE);
    CHECK(image.slot_id == img_id);
    CHECK_EQ(image.slot_fmt, ESL_IMG_FMT_RLE);
    CHECK(memcmp(fake_xflash + IMG_SLOT + IMG_DATA_OFF, code, code_len) == 0);
    CHECK(memcmp(fake_xflash + IMG_SLOT + IMG_RAW_OFF, img, IMG_RAW_SIZE) == 0);
    CHECK(image_show());
    CHECK(screen_is_picture());

    /* a restart finds it, and shows the decoded image */
    ota_init();
    image_init(ota.present);
    CHECK(image.slot_id == img_id);
    CHECK_EQ(image.slot_fmt, ESL_IMG_FMT_RLE);
    CHECK(image_show());
    CHECK(screen_is_picture());

    /* a raw image after it is shown from the download area again */
    make_picture(4);
    CHECK_EQ(download(), IMAGE_DONE);
    CHECK_EQ(image.slot_fmt, ESL_IMG_FMT_RAW);
    CHECK(image_show());
    CHECK(screen_is_picture());

    /* a code that does not end at the screen size is refused, though its CRC is right */
    memset(img, 0, sizeof(img));
    rle_code();
    code[code_len - 2] -= 1;               /* the last run one byte short */
    img_id = proto_crc32(0, code, code_len);
    CHECK_EQ(download_code(), IMAGE_STOP);
    CHECK(image.rejected == img_id);

    /* ... one that would go past the screen (the last run one byte longer) */
    rle_code();
    code[code_len - 2] += 1;
    img_id = proto_crc32(0, code, code_len);
    CHECK_EQ(download_code(), IMAGE_STOP);
    CHECK(image.rejected == img_id);

    /* ... one whose last literal is cut short */
    for (x = 0; x < 16; x++)
        img[IMG_RAW_SIZE - 16 + x] = (uint8_t)(x * 13 + 1);
    rle_code();
    code_len -= 3;
    img_id = proto_crc32(0, code, code_len);
    CHECK_EQ(download_code(), IMAGE_STOP);
    CHECK(image.rejected == img_id);

    /* ... and a whole code with bytes after it */
    rle_code();
    code[code_len++] = 0x80;
    code[code_len++] = 0x00;
    img_id = proto_crc32(0, code, code_len);
    CHECK_EQ(download_code(), IMAGE_STOP);
    CHECK(image.rejected == img_id);
    CHECK_EQ(fake_xflash_misuse, 0);
}

/* ESL_IMG_FMT_2D: img coded as the AP does, with its coder (esl_img2d.c) */

static void code_2d(void)
{
    code_len = (uint16_t)esl_img_2d_encode(img, IMG_RAW_SIZE, DRAW_H, code, sizeof(code) - 1);
    CHECK(code_len > 0);
    img_id = proto_crc32(0, code, code_len);
    if (!img_id)
        img_id = 1;
}

static void test_2d(void)
{
    uint16_t x, i;
    uint8_t keep;

    feed_fmt = ESL_IMG_FMT_2D;
    /* text-like columns, a bar at the top and bottom row, a slope */
    memset(img, 0, sizeof(img));
    for (x = 0; x < DRAW_W; x++) {
        img[x * DRAW_COL_BYTES] = 0x80;
        img[x * DRAW_COL_BYTES + DRAW_COL_BYTES - 1] |= 0x80 >> ((DRAW_H - 1) & 7);
        if (x >= 20 && x < 60)
            img[x * DRAW_COL_BYTES + 2] = (uint8_t)(x * 7);
        if (x < DRAW_H)
            img[x * DRAW_COL_BYTES + x / 8] |= 0xFF >> (x & 7);
    }
    code_2d();
    CHECK(code_len < IMG_RAW_SIZE / 4);
    setup(true);
    CHECK_EQ(download_code(), IMAGE_DONE);
    CHECK(image.slot_id == img_id);
    CHECK_EQ(image.slot_fmt, ESL_IMG_FMT_2D);
    CHECK(memcmp(fake_xflash + IMG_SLOT + IMG_RAW_OFF, img, IMG_RAW_SIZE) == 0);
    CHECK(image_show());
    CHECK(screen_is_picture());

    /* a restart finds it */
    ota_init();
    image_init(ota.present);
    CHECK(image.slot_id == img_id);
    CHECK_EQ(image.slot_fmt, ESL_IMG_FMT_2D);
    CHECK(image_show());
    CHECK(screen_is_picture());

    /* every mode: a picture whose columns differ from the one before by a little and by a lot */
    memset(img, 0, sizeof(img));
    for (i = 0; i < IMG_RAW_SIZE / DRAW_COL_BYTES; i++)
        for (x = 0; x < DRAW_H; x++)
            if ((x >= i % 4 && x < i % 4 + 20) ||                  /* moves 1 to 3 rows: vertical */
                (i % 9 == 0 && x >= 60 && x < 70) ||               /* there in one column: pass */
                (i % 13 == 0 && x >= 30 && x < 33 + i % 40))       /* new runs: horizontal */
                img[i * DRAW_COL_BYTES + x / 8] |= 0x80 >> (x & 7);
    code_2d();
    CHECK_EQ(download_code(), IMAGE_DONE);
    CHECK(memcmp(fake_xflash + IMG_SLOT + IMG_RAW_OFF, img, IMG_RAW_SIZE) == 0);
    CHECK(image_show());
    CHECK(screen_is_picture());

    /* a code cut short is refused, though its CRC is right */
    code_len -= 2;
    img_id = proto_crc32(0, code, code_len);
    CHECK_EQ(download_code(), IMAGE_STOP);
    CHECK(image.rejected == img_id);

    /* ... a whole code with a byte after it */
    code_2d();
    code[code_len++] = 0x00;
    img_id = proto_crc32(0, code, code_len);
    CHECK_EQ(download_code(), IMAGE_STOP);
    CHECK(image.rejected == img_id);

    /* ... one bits that are not zero after the last column (white: one bit per column) */
    memset(img, 0, sizeof(img));
    code_2d();
    if ((DRAW_W * (1 + MODEL_HAS_RED)) % 8) {
        code[code_len - 1] |= 1;
        img_id = proto_crc32(0, code, code_len);
        CHECK_EQ(download_code(), IMAGE_STOP);
        CHECK(image.rejected == img_id);
    }

    /* ... and one with a bit flipped: refused, or a picture of the right size (never past the screen) */
    for (i = 0; i < IMG_RAW_SIZE; i++)
        img[i] = (uint8_t)(i % 11 == 0 ? i : 0);
    for (i = 0; i < 40; i++) {
        code_2d();
        keep = (uint8_t)(1 << (i & 7));
        code[(i * 13) % code_len] ^= keep;
        img_id = proto_crc32(0, code, code_len);
        CHECK(download_code() != IMAGE_MORE);
    }
    CHECK_EQ(fake_xflash_misuse, 0);
    feed_fmt = ESL_IMG_FMT_RLE;
}

/* Blocks out of order, repeated, and a download that stops halfway and goes on */
static void test_download_blocks(void)
{
    uint16_t half = (IMG_RAW_SIZE / 2 / ESL_IMG_CHUNK_MAX) * ESL_IMG_CHUNK_MAX;

    make_picture(4);
    setup(true);
    image_open();
    CHECK_EQ(feed(img_id, IMG_RAW_SIZE, 0, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_MORE);
    CHECK_EQ(image.dl_got, ESL_IMG_CHUNK_MAX);
    CHECK_EQ(feed(img_id, IMG_RAW_SIZE, 3 * ESL_IMG_CHUNK_MAX, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_MORE); /* too far ahead */
    CHECK_EQ(image.dl_got, ESL_IMG_CHUNK_MAX);
    CHECK_EQ(feed(img_id, IMG_RAW_SIZE, 0, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_MORE);                     /* repeated */
    CHECK_EQ(image.dl_got, ESL_IMG_CHUNK_MAX);
    while (image.dl_got < half)
        CHECK_EQ(feed(img_id, IMG_RAW_SIZE, image.dl_got, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_MORE);
    image_close();
    CHECK(image.dl_id == img_id);
    CHECK_EQ(image.slot_id, 0);
    /* ... the next wake-up: the AP starts at 0 again, the tag goes on from dl_got */
    image_open();
    CHECK_EQ(feed(img_id, IMG_RAW_SIZE, 0, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_MORE);
    CHECK_EQ(image.dl_got, half);
    image_close();
    CHECK_EQ(download(), IMAGE_DONE);
    CHECK(memcmp(fake_xflash + IMG_SLOT + IMG_DATA_OFF, img, IMG_RAW_SIZE) == 0);

    /* The AP changes the picture halfway: start over */
    make_picture(5);
    image_open();
    CHECK_EQ(feed(img_id, IMG_RAW_SIZE, 0, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_MORE);
    CHECK_EQ(image.dl_got, ESL_IMG_CHUNK_MAX);
    CHECK_EQ(image.slot_id, 0);
    make_picture(6);
    CHECK_EQ(feed(img_id, IMG_RAW_SIZE, ESL_IMG_CHUNK_MAX, MODEL_CODE, ESL_IMG_FMT_RAW), IMAGE_MORE); /* not from 0: ignored */
    CHECK_EQ(image.dl_got, 0);
    image_close();
    CHECK_EQ(download(), IMAGE_DONE);
    CHECK(image.slot_id == img_id);

    /* A block that does not fit its id: the check fails, the image is refused */
    make_picture(7);
    img[10] ^= 1;
    CHECK_EQ(download(), IMAGE_STOP);
    CHECK(image.rejected == img_id);
    CHECK_EQ(image.slot_id, 0);
    CHECK_EQ(fake_xflash_misuse, 0);
}

static void push_img_blocks(void)
{
    uint16_t pos;

    for (pos = 0; pos < IMG_RAW_SIZE; pos += ESL_IMG_CHUNK_MAX) {
        uint8_t n = (uint8_t)(IMG_RAW_SIZE - pos > ESL_IMG_CHUNK_MAX ? ESL_IMG_CHUNK_MAX : IMG_RAW_SIZE - pos);
        uint8_t len = make_img_block(pl, 0, img_id, IMG_RAW_SIZE, pos, MODEL_CODE, ESL_IMG_FMT_RAW, img + pos, n);
        fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, len));
    }
}

static const uint8_t *sent(int i)
{
    return fake_tx[i] + ESL_MAC_HDR_LEN;
}

/* The tag's side over the radio: POLL with bit5, IMG at 0, IMG_REQ for the rest, draw */
static void test_app_download(void)
{
    int blocks = (IMG_RAW_SIZE + ESL_IMG_CHUNK_MAX - 1) / ESL_IMG_CHUNK_MAX, i;
    uint8_t n;

    fake_store_clear();
    fake_radio_reset();
    fake_draw_count = 0;
    fake_epd_ok = true;
    fake_xflash_clear(true);
    app_init();
    CHECK_EQ(fake_draw_count, 1);                           /* the start screen */
    make_picture(8);
    push_img_blocks();
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(fake_tx_count, blocks);
    CHECK(sent(0)[ESL_POLL_OFF_FLAGS] & ESL_POLL_F_IMAGE);
    CHECK_EQ(ESL_GET_U32(sent(0) + ESL_POLL_OFF_CID), 0);
    for (i = 1; i < blocks; i++) {
        CHECK_EQ(sent(i)[0], ESL_MSG_IMG_REQ);
        CHECK_EQ(sent(i)[ESL_IREQ_OFF_MODEL], MODEL_CODE);
        CHECK_EQ(sent(i)[ESL_IREQ_OFF_MAX], ESL_IMG_CHUNK_MAX);
        CHECK_EQ(ESL_GET_U16(sent(i) + ESL_IREQ_OFF_POS), i * ESL_IMG_CHUNK_MAX);
        CHECK(ESL_GET_U32(sent(i) + ESL_IREQ_OFF_ID) == img_id);
    }
    CHECK_EQ(fake_draw_count, 2);                           /* the picture, with the radio off */
    CHECK(screen_is_picture());
    CHECK(app.shown_id == img_id);
    CHECK(!(app.poll_flags & ESL_POLL_F_DRAWFAIL));
    CHECK(!fake_xflash_is_open);
    CHECK_EQ(fake_radio_on, 0);

    /* The next POLL reports the picture; IDLE draws nothing */
    fake_radio_reset();
    n = make_idle(pl, 0);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
    app_poll_once();
    CHECK(ESL_GET_U32(sent(0) + ESL_POLL_OFF_CID) == img_id);
    CHECK_EQ(fake_draw_count, 2);

    /* A restart shows the picture from the slot and reports it */
    fake_radio_reset();
    fake_draw_count = 0;
    app_init();
    CHECK_EQ(fake_draw_count, 1);
    CHECK(screen_is_picture());
    CHECK(app.shown_id == img_id);
    CHECK_EQ(fake_store_len, 0);                            /* nothing to keep: the slot is the record */

    /* Lost answers: the request goes again and the round goes on (as for FW) */
    make_picture(9);
    fake_radio_reset();
    for (i = 0; i < blocks; i++) {
        uint16_t pos = (uint16_t)(i * ESL_IMG_CHUNK_MAX);
        uint8_t k = (uint8_t)(IMG_RAW_SIZE - pos > ESL_IMG_CHUNK_MAX ? ESL_IMG_CHUNK_MAX : IMG_RAW_SIZE - pos);
        uint8_t len = make_img_block(pl, 0, img_id, IMG_RAW_SIZE, pos, MODEL_CODE, ESL_IMG_FMT_RAW, img + pos, k);
        if (i == 4) {
            fake_rx_lose();
            fake_rx_lose();
        }
        fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, len));
    }
    app_poll_once();
    CHECK_EQ(fake_tx_count, blocks + 2);                    /* the request and its retry, then again */
    CHECK(screen_is_picture());
    CHECK(app.shown_id == img_id);

    /* Settings are all the record holds: the slot keeps the picture */
    CHECK_EQ(fake_store_len, 0);

    /* An image for another model: refused, bit1 in the next POLL, one block per round */
    fake_radio_reset();
    n = make_img_block(pl, 0, 0x1234, IMG_RAW_SIZE, 0, MODEL_CODE + 1, ESL_IMG_FMT_RAW, img, 10);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
    app_poll_once();
    CHECK_EQ(fake_tx_count, 1);
    CHECK(app.shown_id == img_id);
    fake_radio_reset();
    n = make_idle(pl, 0);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
    app_poll_once();
    CHECK(sent(0)[ESL_POLL_OFF_FLAGS] & ESL_POLL_F_DRAWFAIL);
    CHECK(ESL_GET_U32(sent(0) + ESL_POLL_OFF_CID) == img_id);

    /* The panel does not answer: bit1, the id stays as it was, the slot keeps the image */
    make_picture(10);
    fake_radio_reset();
    fake_epd_ok = false;
    push_img_blocks();
    app_poll_once();
    CHECK(sent(0)[ESL_POLL_OFF_FLAGS] & ESL_POLL_F_DRAWFAIL);
    CHECK(app.shown_id != img_id);
    CHECK(image.slot_id == img_id);
    fake_epd_ok = true;
    /* ... offered again: shown from the slot without a download */
    fake_radio_reset();
    n = make_img_block(pl, 0, img_id, IMG_RAW_SIZE, 0, MODEL_CODE, ESL_IMG_FMT_RAW, img, ESL_IMG_CHUNK_MAX);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
    app_poll_once();
    CHECK_EQ(fake_tx_count, 1);
    CHECK(app.shown_id == img_id);
    CHECK(screen_is_picture());
    CHECK(!(app.poll_flags & ESL_POLL_F_DRAWFAIL));

    CHECK_EQ(fake_xflash_misuse, 0);
    CHECK(!fake_store_radio_on_write);
    fake_xflash_clear(false);                               /* the other tests run without */
}

/* The picture with the "no AP" mark in its top right corner, and nothing else changed */
static bool screen_is_marked(void)
{
    uint8_t black[DRAW_COL_BYTES], red[DRAW_COL_BYTES];
    uint16_t x;
    uint8_t y;
    int ink = 0;

    for (x = 0; x < DRAW_W; x++) {
        memcpy(black, img + x * DRAW_COL_BYTES, DRAW_COL_BYTES);
        memset(red, 0, sizeof(red));
        status_mark_column(x, black, red);
        for (y = 0; y < DRAW_H; y++) {
            uint8_t b = black[y >> 3] & (uint8_t)(0x80 >> (y & 7));
            if ((fake_fb[y][x] == 1) != (b != 0))
                return false;
            if (x >= DRAW_W - STATUS_MARK_W && y < STATUS_MARK_H && b)
                ink++;
        }
    }
    return ink > 20 && !screen_is_picture();
}

static void lose_rounds_until_draw(void)
{
    int draws = fake_draw_count, rounds = 0;

    while (fake_draw_count == draws && rounds++ < 1000) {
        fake_radio_reset();
        app_poll_once();
    }
}

static void push_idle(void)
{
    uint8_t n = make_idle(pl, 0);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
}

/* #98: the AP gone for lost_ap_s keeps the picture up, with the "no AP" mark */
static void test_app_lost_mark(void)
{
    int draws, i;

    fake_store_clear();
    fake_radio_reset();
    fake_draw_count = 0;
    fake_epd_ok = true;
    fake_xflash_clear(true);
    app_init();
    make_picture(11);
    push_img_blocks();
    app_poll_once();
    CHECK(screen_is_picture());

    /* Gone: one refresh, the picture with the mark, the id still reported */
    draws = fake_draw_count;
    lose_rounds_until_draw();
    CHECK_EQ(fake_draw_count, draws + 1);
    CHECK_EQ(app.lost_s, CFG_LOST_AP_S);
    CHECK(screen_is_marked());
    CHECK(app.shown_id == img_id);
    CHECK_EQ(app.lost_shown, 1);
    for (i = 0; i < 20; i++) {                              /* no refresh per miss */
        fake_radio_reset();
        app_poll_once();
    }
    CHECK_EQ(fake_draw_count, draws + 1);

    /* Back with nothing new: the picture again from the slot, no download, one refresh */
    fake_radio_reset();
    push_idle();
    app_poll_once();
    CHECK(ESL_GET_U32(sent(0) + ESL_POLL_OFF_CID) == img_id);
    CHECK_EQ(fake_tx_count, 1);
    CHECK_EQ(fake_draw_count, draws + 2);
    CHECK(screen_is_picture());
    CHECK(app.shown_id == img_id);
    CHECK_EQ(image.mark, 0);
    CHECK_EQ(app.lost_shown, 0);
    fake_radio_reset();
    push_idle();
    app_poll_once();
    CHECK_EQ(fake_draw_count, draws + 2);

    /* Gone again, then back with a new picture: drawn once, without the mark */
    lose_rounds_until_draw();
    CHECK(screen_is_marked());
    draws = fake_draw_count;
    make_picture(12);
    fake_radio_reset();
    push_img_blocks();
    app_poll_once();
    CHECK_EQ(fake_draw_count, draws + 1);
    CHECK(screen_is_picture());
    CHECK(app.shown_id == img_id);
    CHECK_EQ(image.mark, 0);

    /* Gone, then back with only the first block of a new picture: the marked one stays
       while it comes in ... */
    lose_rounds_until_draw();
    CHECK(screen_is_marked());
    draws = fake_draw_count;
    make_picture(13);
    fake_radio_reset();
    i = make_img_block(pl, 0, img_id, IMG_RAW_SIZE, 0, MODEL_CODE, ESL_IMG_FMT_RAW, img, ESL_IMG_CHUNK_MAX);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, (uint8_t)i));
    app_poll_once();
    CHECK_EQ(fake_draw_count, draws);
    CHECK_EQ(image.mark, 1);
    CHECK_EQ(app.lost_shown, 0);
    /* ... and the AP then sends nothing more: the old picture left the slot, so the
       waiting screen, and no id, so the AP offers its picture again */
    fake_radio_reset();
    push_idle();
    app_poll_once();
    CHECK_EQ(fake_draw_count, draws + 1);
    CHECK_EQ(app.shown_id, 0);
    CHECK_EQ(image.mark, 0);
    CHECK(!screen_is_picture());

    /* Gone with a half-downloaded picture: "No AP found", as without a picture */
    lose_rounds_until_draw();
    CHECK_EQ(app.shown_id, 0);
    CHECK_EQ(image.mark, 0);
    CHECK_EQ(app.lost_shown, 1);

    /* A restart while marked shows the picture without the mark */
    make_picture(14);
    fake_radio_reset();
    push_img_blocks();
    app_poll_once();
    lose_rounds_until_draw();
    CHECK(screen_is_marked());
    fake_radio_reset();
    app_init();
    CHECK(screen_is_picture());
    CHECK_EQ(image.mark, 0);

    /* The panel does not answer when the mark is drawn: "No AP found" is tried, no id */
    fake_epd_ok = false;
    lose_rounds_until_draw();
    CHECK_EQ(image.mark, 0);
    CHECK_EQ(app.shown_id, 0);
    fake_epd_ok = true;

    CHECK_EQ(fake_xflash_misuse, 0);
    CHECK(!fake_store_radio_on_write);
    fake_xflash_clear(false);
}

void test_image(void)
{
    RUN(test_refused);
    RUN(test_crc_zero);
    RUN(test_download);
    RUN(test_download_blocks);
    RUN(test_rle);
    RUN(test_2d);
    RUN(test_app_download);
    RUN(test_app_lost_mark);
}
