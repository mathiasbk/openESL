/* test_ota.c - firmware update over the radio (docs/tag-ota.md): the FW messages, the
 * download into the SPI flash (ota.c), the install and the bootloader's decisions
 * (boot/boot_core.c), all over the fake SPI flash and chip flash in fake_xflash.c. */
#include <string.h>
#include "test.h"
#include "fake.h"
#include "helpers.h"
#include "app.h"
#include "config.h"
#include "model.h"
#include "ota.h"
#include "otaslot.h"
#include "boot_core.h"

#define NEW_VER  (FW_VERSION + 1)

static uint8_t img[ESL_FW_MAX];
static uint16_t img_size;
static uint32_t img_crc;
static uint8_t pl[ESL_MAX_PAYLOAD], fr[128];

static void make_image(uint16_t size, uint8_t seed)
{
    for (uint32_t i = 0; i < size; i++)
        img[i] = (uint8_t)(i * 7 + seed + (i >> 8));
    img_size = size;
    img_crc = proto_crc32(0, img, size);
}

/* The running application: a pattern in 0x0400-0xF3FF */
static void fill_code(void)
{
    for (uint32_t i = 0; i < sizeof(fake_code); i++)
        fake_code[i] = (uint8_t)(i ^ 0x5A);
}

static void setup(bool present)
{
    fake_xflash_clear(present);
    fill_code();
    ota_init();
}

static uint8_t st(uint8_t flag)
{
    return fake_xflash[OTA_STATE + flag];
}

/* One FW block of img at pos, parsed as the tag does */
static uint8_t feed(uint16_t ver, uint16_t pos, uint8_t model, uint32_t crc)
{
    proto_reply_t r;
    uint16_t left = img_size - pos;
    uint8_t n = (uint8_t)(left > ESL_FW_CHUNK_MAX ? ESL_FW_CHUNK_MAX : left);
    uint8_t len = make_fw_block(pl, 0, ver, img_size, pos, crc, model, img + pos, n);

    memset(&r, 0, sizeof(r));
    CHECK_EQ(proto_parse_reply(pl, len, &r), PROTO_OK);
    return ota_block(&r, pl + r.field_off[0]);
}

/* The whole image, block by block as the tag asks for it */
static uint8_t download(void)
{
    uint8_t res;
    ota_open();
    do {
        res = feed(NEW_VER, ota.dl_got, MODEL_CODE, img_crc);
    } while (res == OTA_MORE);
    ota_close();
    return res;
}

static void test_fw_messages(void)
{
    proto_reply_t r;
    uint8_t d[3] = { 1, 2, 3 }, out[ESL_MAX_PAYLOAD];
    uint8_t len = make_fw_block(pl, 300, 0x1234, 2048, 91, 0xDEADBEEF, MODEL_CODE, d, 3);

    memset(&r, 0, sizeof(r));
    CHECK_EQ(proto_parse_reply(pl, len, &r), PROTO_OK);
    CHECK_EQ(r.type, ESL_MSG_FW);
    CHECK_EQ(r.next_poll_s, 300);
    CHECK_EQ(r.fw_ver, 0x1234);
    CHECK_EQ(r.lay_size, 2048);
    CHECK_EQ(r.lay_pos, 91);
    CHECK(r.fw_crc == 0xDEADBEEF);
    CHECK_EQ(r.fw_model, MODEL_CODE);
    CHECK_EQ(r.field_len[0], 3);
    CHECK_EQ(pl[r.field_off[0] + 2], 3);
    CHECK(proto_parse_reply(pl, ESL_FW_HDR_LEN - 1, &r) != PROTO_OK);

    len = proto_build_fw_req(out, 0x1234, 182, 0xCAFEF00D, ESL_FW_CHUNK_MAX);
    CHECK_EQ(len, ESL_FREQ_LEN);
    CHECK_EQ(out[0], ESL_MSG_FW_REQ);
    CHECK_EQ(out[ESL_FREQ_OFF_MODEL], MODEL_CODE);
    CHECK_EQ(out[ESL_FREQ_OFF_MAX], ESL_FW_CHUNK_MAX);
    CHECK_EQ(ESL_GET_U16(out + ESL_FREQ_OFF_VER), 0x1234);
    CHECK_EQ(ESL_GET_U16(out + ESL_FREQ_OFF_POS), 182);
    CHECK(ESL_GET_U32(out + ESL_FREQ_OFF_CRC) == 0xCAFEF00D);
}

static void test_refused(void)
{
    /* No SPI flash: no OTA flag, nothing taken */
    setup(false);
    make_image(2048, 1);
    CHECK_EQ(ota_poll_flags(), 0);
    CHECK_EQ(feed(NEW_VER, 0, MODEL_CODE, img_crc), OTA_STOP);

    setup(true);
    CHECK_EQ(ota_poll_flags(), ESL_POLL_F_OTA);
    ota_open();
    CHECK_EQ(feed(NEW_VER, 0, MODEL_CODE + 1, img_crc), OTA_STOP);   /* other model */
    CHECK_EQ(feed(FW_VERSION, 0, MODEL_CODE, img_crc), OTA_STOP);    /* what runs */
    make_image(1000, 1);                                             /* not whole pages */
    CHECK_EQ(feed(NEW_VER, 0, MODEL_CODE, img_crc), OTA_STOP);
    ota_close();
    CHECK_EQ(ota.dl_ver, 0);
    CHECK_EQ(fake_xflash_misuse, 0);
}

static void test_download(void)
{
    setup(true);
    make_image(3072, 2);
    CHECK_EQ(download(), OTA_DONE);
    CHECK_EQ(ota.dl_got, 3072);
    CHECK(memcmp(fake_xflash + OTA_SLOT_NEW + OTA_DATA_OFF, img, 3072) == 0);
    CHECK(ESL_GET_U32(fake_xflash + OTA_SLOT_NEW + ESL_FWF_OFF_MAGIC) == ESL_FWF_MAGIC);
    CHECK_EQ(fake_xflash[OTA_SLOT_NEW + ESL_FWF_OFF_MODEL], MODEL_CODE);
    CHECK_EQ(ESL_GET_U16(fake_xflash + OTA_SLOT_NEW + ESL_FWF_OFF_VER), NEW_VER);
    CHECK(ESL_GET_U32(fake_xflash + OTA_SLOT_NEW + ESL_FWF_OFF_SIZE) == 3072);
    CHECK(ESL_GET_U32(fake_xflash + OTA_SLOT_NEW + ESL_FWF_OFF_CRC) == img_crc);
    CHECK_EQ(fake_xflash_misuse, 0);
    CHECK(!fake_xflash_is_open);

    /* A block that is not the one asked for changes nothing; a repeat is skipped */
    setup(true);
    ota_open();
    CHECK_EQ(feed(NEW_VER, 0, MODEL_CODE, img_crc), OTA_MORE);
    CHECK_EQ(ota.dl_got, ESL_FW_CHUNK_MAX);
    CHECK_EQ(feed(NEW_VER, 2 * ESL_FW_CHUNK_MAX, MODEL_CODE, img_crc), OTA_MORE);
    CHECK_EQ(ota.dl_got, ESL_FW_CHUNK_MAX);
    CHECK_EQ(feed(NEW_VER, 0, MODEL_CODE, img_crc), OTA_MORE);
    CHECK_EQ(ota.dl_got, ESL_FW_CHUNK_MAX);
    /* The image changed on the AP: start again */
    CHECK_EQ(feed(NEW_VER, 0, MODEL_CODE, img_crc ^ 1), OTA_MORE);
    CHECK(ota.dl_crc == (img_crc ^ 1));
    CHECK_EQ(ota.dl_got, ESL_FW_CHUNK_MAX);
    ota_close();

    /* A wrong CRC: refused and reported, and that version is not taken again */
    setup(true);
    make_image(1024, 3);
    img_crc ^= 0x100;
    CHECK_EQ(download(), OTA_STOP);
    CHECK_EQ(ota.failed_ver, NEW_VER);
    CHECK_EQ(ota_poll_flags(), ESL_POLL_F_OTA | ESL_POLL_F_FWFAIL);
    CHECK(fake_xflash[OTA_SLOT_NEW] == 0xFF);               /* no header */
    img_crc ^= 0x100;
    ota_open();
    CHECK_EQ(feed(NEW_VER, 0, MODEL_CODE, img_crc), OTA_STOP);
    ota_close();
}

/* Download, install and the first boot of img */
static void install_and_boot(void)
{
    CHECK_EQ(download(), OTA_DONE);
    ota_install();
    CHECK_EQ(fake_resets, 1);
    CHECK(st(OTA_ST_INSTALL) == 0);
    /* The old application is in the rollback slot, with its header */
    CHECK(memcmp(fake_xflash + OTA_SLOT_OLD + OTA_DATA_OFF, fake_code + ESL_FW_APP_BASE, ESL_FW_MAX) == 0);
    CHECK_EQ(ESL_GET_U16(fake_xflash + OTA_SLOT_OLD + ESL_FWF_OFF_VER), FW_VERSION);
    boot_core_run();
}

static void test_install_confirm(void)
{
    uint8_t old[ESL_FW_MAX];

    setup(true);
    memcpy(old, fake_code + ESL_FW_APP_BASE, ESL_FW_MAX);
    make_image(2048, 4);
    install_and_boot();
    CHECK(st(OTA_ST_COPIED) == 0);
    CHECK(memcmp(fake_code + ESL_FW_APP_BASE, img, 2048) == 0);
    CHECK(memcmp(fake_code + ESL_FW_APP_BASE + 2048, old + 2048, ESL_FW_MAX - 2048) == 0);
    CHECK_EQ(fake_code[0], 0x5A);                           /* page 0 untouched */
    CHECK_EQ(fake_code[ESL_FW_APP_END], (uint8_t)(ESL_FW_APP_END ^ 0x5A));

    /* The new image starts on trial, and the first answer keeps it. (On the PC the
       code still reports FW_VERSION, not the image's NEW_VER: a mismatch, which is
       reported as failed so the AP stops offering it.) */
    ota_init();
    CHECK_EQ(ota.trial, 1);
    CHECK_EQ(ota.failed_ver, NEW_VER);
    CHECK_EQ(ota_poll_flags(), ESL_POLL_F_OTA | ESL_POLL_F_FWFAIL);
    ota_confirm();
    CHECK(st(OTA_ST_CONFIRMED) == 0);
    ota_init();
    CHECK_EQ(ota.trial, 0);
    CHECK_EQ(ota.failed_ver, 0);
    for (int i = 0; i < 20; i++)
        boot_core_run();
    CHECK(memcmp(fake_code + ESL_FW_APP_BASE, img, 2048) == 0);
    CHECK(st(OTA_ST_TRIES) == 0xFF);
    CHECK(st(OTA_ST_ROLLEDBACK) == 0xFF);
    CHECK_EQ(fake_xflash_misuse, 0);
}

static void test_rollback(void)
{
    uint8_t old[ESL_FW_MAX];

    setup(true);
    memcpy(old, fake_code + ESL_FW_APP_BASE, ESL_FW_MAX);
    make_image(4096, 5);
    install_and_boot();
    /* Never reaches the AP: each start uses a try, then the old image comes back */
    for (int i = 0; i < OTA_ST_TRIES_N; i++) {
        boot_core_run();
        CHECK(st(OTA_ST_TRIES + i) == 0);
        CHECK(st(OTA_ST_ROLLEDBACK) == 0xFF);
    }
    CHECK(memcmp(fake_code + ESL_FW_APP_BASE, img, 4096) == 0);
    boot_core_run();
    CHECK(st(OTA_ST_ROLLEDBACK) == 0);
    CHECK(memcmp(fake_code + ESL_FW_APP_BASE, old, ESL_FW_MAX) == 0);
    boot_core_run();                                        /* stays */
    CHECK(memcmp(fake_code + ESL_FW_APP_BASE, old, ESL_FW_MAX) == 0);

    /* The old firmware tells the AP, and does not take that version again */
    ota_init();
    CHECK_EQ(ota.failed_ver, NEW_VER);
    CHECK_EQ(ota.trial, 0);
    CHECK_EQ(ota_poll_flags(), ESL_POLL_F_OTA | ESL_POLL_F_FWFAIL);
    ota_open();
    CHECK_EQ(feed(NEW_VER, 0, MODEL_CODE, img_crc), OTA_STOP);
    /* A newer one is taken, and clears the report */
    CHECK_EQ(feed(NEW_VER + 1, 0, MODEL_CODE, img_crc), OTA_MORE);
    ota_close();
    CHECK_EQ(ota.failed_ver, 0);
    CHECK(st(OTA_ST_INSTALL) == 0xFF);
}

/* An image whose header has the version the code reports is not "failed" */
static void test_version_match(void)
{
    setup(true);
    make_image(1024, 9);
    ota_open();
    uint8_t res;
    do {
        res = feed(FW_VERSION + 2, ota.dl_got, MODEL_CODE, img_crc);
    } while (res == OTA_MORE);
    ota_close();
    CHECK_EQ(res, OTA_DONE);
    ota_install();
    boot_core_run();
    /* pretend the code now is that version: rewrite the header's version to FW_VERSION */
    fake_xflash[OTA_SLOT_NEW + ESL_FWF_OFF_VER] = (uint8_t)FW_VERSION;
    fake_xflash[OTA_SLOT_NEW + ESL_FWF_OFF_VER + 1] = (uint8_t)(FW_VERSION >> 8);
    ota_init();
    CHECK_EQ(ota.trial, 1);
    CHECK_EQ(ota.failed_ver, 0);
    CHECK_EQ(ota_poll_flags(), ESL_POLL_F_OTA);
}

static void test_boot_cases(void)
{
    uint8_t old[ESL_FW_MAX];

    /* Nothing asked: nothing happens */
    setup(true);
    memcpy(old, fake_code + ESL_FW_APP_BASE, ESL_FW_MAX);
    boot_core_run();
    CHECK(memcmp(fake_code + ESL_FW_APP_BASE, old, ESL_FW_MAX) == 0);
    CHECK(st(OTA_ST_ROLLEDBACK) == 0xFF);

    /* Power cut while copying: COPIED is not set yet, so the next start copies again */
    make_image(2048, 6);
    install_and_boot();
    fake_xflash[OTA_STATE + OTA_ST_COPIED] = 0xFF;
    memset(fake_code + ESL_FW_APP_BASE + 1024, 0xFF, 1024);
    boot_core_run();
    CHECK(st(OTA_ST_COPIED) == 0);
    CHECK(memcmp(fake_code + ESL_FW_APP_BASE, img, 2048) == 0);

    /* The new slot went bad before the copy: nothing copied, marked as rolled back */
    setup(true);
    make_image(2048, 7);
    CHECK_EQ(download(), OTA_DONE);
    ota_install();
    fake_xflash[OTA_SLOT_NEW + OTA_DATA_OFF + 1500] ^= 0x01;
    boot_core_run();
    CHECK(st(OTA_ST_COPIED) == 0xFF);
    CHECK(st(OTA_ST_ROLLEDBACK) == 0);
    CHECK(memcmp(fake_code + ESL_FW_APP_BASE, old, ESL_FW_MAX) == 0);
    ota_init();
    CHECK_EQ(ota.failed_ver, NEW_VER);

    /* Header damage is caught too */
    setup(true);
    CHECK_EQ(download(), OTA_DONE);
    ota_install();
    fake_xflash[OTA_SLOT_NEW + ESL_FWF_OFF_SIZE + 2] = 0;   /* still 0: fine */
    fake_xflash[OTA_SLOT_NEW + ESL_FWF_OFF_MAGIC] &= 0x0F;
    boot_core_run();
    CHECK(st(OTA_ST_ROLLEDBACK) == 0);
    CHECK(memcmp(fake_code + ESL_FW_APP_BASE, old, ESL_FW_MAX) == 0);
}

/* The tag's side over the radio: POLL with F_OTA, FW at 0, FW_REQ for the rest, install */
static void test_app_download(void)
{
    uint16_t pos;

    fake_store_clear();
    fake_radio_reset();
    fake_draw_count = 0;
    fake_epd_ok = true;
    fake_xflash_clear(true);
    fill_code();
    app_init();
    make_image(1024, 8);                                    /* 12 blocks */
    for (pos = 0; pos < img_size; pos += ESL_FW_CHUNK_MAX) {
        uint8_t n = (uint8_t)(img_size - pos > ESL_FW_CHUNK_MAX ? ESL_FW_CHUNK_MAX : img_size - pos);
        uint8_t len = make_fw_block(pl, 0, NEW_VER, img_size, pos, img_crc, MODEL_CODE, img + pos, n);
        fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, len));
    }
    app_poll_once();
    CHECK(fake_xflash_present);
    CHECK_EQ(fake_tx_count, 12);
    CHECK(fake_tx[0][ESL_MAC_HDR_LEN + ESL_POLL_OFF_FLAGS] & ESL_POLL_F_OTA);
    for (int i = 1; i < 12; i++) {
        const uint8_t *q = fake_tx[i] + ESL_MAC_HDR_LEN;
        CHECK_EQ(q[0], ESL_MSG_FW_REQ);
        CHECK_EQ(ESL_GET_U16(q + ESL_FREQ_OFF_POS), i * ESL_FW_CHUNK_MAX);
        CHECK_EQ(ESL_GET_U16(q + ESL_FREQ_OFF_VER), NEW_VER);
        CHECK(ESL_GET_U32(q + ESL_FREQ_OFF_CRC) == img_crc);
    }
    CHECK_EQ(fake_draw_count, 1);                           /* only the start screen */
    CHECK_EQ(fake_resets, 1);
    CHECK(st(OTA_ST_INSTALL) == 0);
    CHECK(!fake_xflash_is_open);
    CHECK_EQ(fake_radio_on, 0);
    CHECK_EQ(fake_xflash_misuse, 0);
    fake_xflash_clear(false);                               /* the other tests run without */
}

void test_ota(void)
{
    RUN(test_fw_messages);
    RUN(test_refused);
    RUN(test_download);
    RUN(test_install_confirm);
    RUN(test_rollback);
    RUN(test_version_match);
    RUN(test_boot_cases);
    RUN(test_app_download);
}
