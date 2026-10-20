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
    return image_block(&r, pl + r.field_off[0]);
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
    CHECK_EQ(image_poll_flags(), ESL_POLL_F_IMAGE);
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

    /* Forgotten: the header is gone, the data stays */
    image_forget();
    CHECK_EQ(image.slot_id, 0);
    image_init(1);
    CHECK_EQ(image.slot_id, 0);
    CHECK(memcmp(fake_xflash + IMG_SLOT + IMG_DATA_OFF, img, IMG_RAW_SIZE) == 0);

    /* A new picture replaces it; the old id is no longer in the slot */
    make_picture(3);
    CHECK(img_id != first);
    CHECK_EQ(download(), IMAGE_DONE);
    CHECK(image.slot_id == img_id);
    CHECK(image_show());
    CHECK(screen_is_picture());
    CHECK_EQ(fake_xflash_misuse, 0);
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
    static const char *const price[] = { "Lettmelk 1 L", "24,90", "kr/l 24,90" };
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

    /* CONTENT from an AP that does not draw (the transition): drawn as before, and the
       slot is forgotten, so a restart shows the content, not the old picture */
    fake_radio_reset();
    n = make_content(pl, 0, 1111, ESL_LAYOUT_PRICE, 0, 3, price);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
    app_poll_once();
    CHECK_EQ(app.shown_id, 1111);
    CHECK_EQ(image.slot_id, 0);
    CHECK(!screen_is_picture());
    fake_draw_count = 0;
    app_init();
    CHECK_EQ(fake_draw_count, 1);
    CHECK_EQ(app.shown_id, 1111);
    CHECK(!screen_is_picture());

    /* The AP offers the picture again (the tag reported 1111): downloaded and shown, the
       content record is dropped */
    fake_radio_reset();
    push_img_blocks();
    app_poll_once();
    CHECK(app.shown_id == img_id);
    CHECK(screen_is_picture());
    CHECK_EQ(fake_store_len, 14);                           /* settings only, no content */
    fake_draw_count = 0;
    app_init();
    CHECK(app.shown_id == img_id);
    CHECK(screen_is_picture());

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

void test_image(void)
{
    RUN(test_refused);
    RUN(test_crc_zero);
    RUN(test_download);
    RUN(test_download_blocks);
    RUN(test_app_download);
}
