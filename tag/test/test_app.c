#include <string.h>
#include "test.h"
#include "fake.h"
#include "helpers.h"
#include "app.h"
#include "config.h"
#include "layout.h"

static uint8_t pl[ESL_MAX_PAYLOAD], fr[128];

static void push_idle(uint16_t next)
{
    uint8_t n = make_idle(pl, next);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
}

static void push_config(uint16_t next, uint16_t ver, uint16_t poll, uint16_t max_poll, uint16_t lost)
{
    uint8_t n = make_config(pl, next, ver, poll, max_poll, lost);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
}

static void push_content(uint32_t cid, uint8_t layout, uint8_t flags, int nf, const char *const *f)
{
    uint8_t n = make_content(pl, 0, cid, layout, flags, nf, f);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
}

/* POLL-nyttelasten i sendt ramme nr i */
static const uint8_t *sent_poll(int i)
{
    return fake_tx[i] + ESL_MAC_HDR_LEN;
}

static void fresh(void)
{
    fake_store_clear();       /* a new tag: nothing in flash */
    fake_radio_reset();
    fake_draw_count = 0;
    fake_epd_ok = true;
    app_init();
}

static const char *const price[] = { "Lettmelk 1 L", "24,90", "kr/l 24,90" };
static const char *const price2[] = { "Lettmelk 1 L", "21,90", "Tilbud" };

static void test_boot(void)
{
    fresh();
    CHECK_EQ(fake_draw_count, 1);              /* oppstartsskjerm (T9) */
    CHECK(fake_ink_count(1) > 100);
    push_idle(0);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(fake_tx_count, 1);
    CHECK_EQ(fake_tx_len[0], ESL_MAC_HDR_LEN + ESL_POLL_LEN_LAYOUT);
    CHECK_EQ(ESL_GET_U16(sent_poll(0) + ESL_POLL_OFF_CFG_VER), 0);      /* built-in settings */
    CHECK_EQ(ESL_GET_U32(sent_poll(0) + ESL_POLL_OFF_LAYOUT_VER), 0);   /* no layout file yet */
    CHECK_EQ(sent_poll(0)[0], ESL_MSG_POLL);
    CHECK_EQ(ESL_GET_U16(sent_poll(0) + ESL_POLL_OFF_FW), FW_VERSION);
    CHECK_EQ(ESL_GET_U16(sent_poll(0) + ESL_POLL_OFF_BATT), fake_batt_mv);
    CHECK_EQ(ESL_GET_U32(sent_poll(0) + ESL_POLL_OFF_CID), 0);
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_FLAGS], ESL_POLL_F_BOOTED);
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_MODEL], MODEL_CODE);   /* T18 */
    CHECK(memcmp(fake_tx[0] + ESL_TA_OFF_SRC, fake_mac, 8) == 0);
    CHECK_EQ(fake_draw_count, 1);              /* IDLE: nothing drawn */
    CHECK_EQ(fake_radio_on, 0);                /* radio is off afterwards */
}

static void test_next_poll_from_ap(void)
{
    fresh();
    push_idle(120);
    CHECK_EQ(app_poll_once(), 120);
    push_idle(0);                              /* 0 = behold */
    CHECK_EQ(app_poll_once(), 120);
}

static void test_content_draws_once(void)
{
    fresh();
    push_content(1111, ESL_LAYOUT_PRICE, ESL_CF_PRICE_HIGHLIGHT, 3, price);
    app_poll_once();
    CHECK_EQ(fake_draw_count, 2);
    CHECK(fake_ink_count(1) > 1000);                 /* name, subtitle and the black price box */
    CHECK_EQ(app.shown_id, 1111);

    /* The next POLL reports the new content_id and no longer 'just booted' */
    fake_radio_reset();
    push_content(1111, ESL_LAYOUT_PRICE, ESL_CF_PRICE_HIGHLIGHT, 3, price);
    app_poll_once();
    CHECK_EQ(ESL_GET_U32(sent_poll(0) + ESL_POLL_OFF_CID), 1111);
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_FLAGS], 0);
    CHECK_EQ(fake_draw_count, 2);              /* same content: no new drawing (T5) */

    fake_radio_reset();
    push_content(2222, ESL_LAYOUT_PRICE, 0, 3, price2);
    app_poll_once();
    CHECK_EQ(fake_draw_count, 3);
    CHECK(fake_ink_count(1) > 100);
    CHECK_EQ(app.shown_id, 2222);
}

static void test_retry_then_answer(void)
{
    fresh();
    /* first listening window: a frame to another tag, so nothing */
    {
        uint8_t other[8];
        uint8_t n = make_idle(pl, 0);
        memcpy(other, fake_mac, 8);
        other[7] ^= 0x80;
        fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, other, pl, n));
    }
    CHECK_EQ(app_poll_once(), app_backoff_s(CFG_POLL_S, 1));
    CHECK_EQ(fake_tx_count, 2);                /* one retry */
    CHECK(fake_delay_ms_total >= CFG_RETRY_MIN_MS);
    CHECK_EQ(app.misses, 1);

    fake_radio_reset();
    push_idle(0);
    app_poll_once();
    CHECK_EQ(fake_tx_count, 1);
    CHECK_EQ(app.misses, 0);
}

static void test_other_tag_frame_skipped(void)
{
    uint8_t other[8];
    uint8_t n;
    fresh();
    memcpy(other, fake_mac, 8);
    other[0] ^= 1;
    n = make_idle(pl, 0);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, other, pl, n));
    push_content(5, ESL_LAYOUT_TEXT, 0, 4, (const char *const[]){ "Hei", "", "", "" });
    app_poll_once();
    CHECK_EQ(fake_tx_count, 1);                /* the reply to us came in the same window */
    CHECK_EQ(app.shown_id, 5);
}

static void test_backoff(void)
{
    CHECK_EQ(app_backoff_s(60, 0), 60);
    CHECK_EQ(app_backoff_s(60, CFG_BACKOFF_AFTER - 1), 60);
    CHECK_EQ(app_backoff_s(60, CFG_BACKOFF_AFTER), 120);
    CHECK_EQ(app_backoff_s(60, CFG_BACKOFF_AFTER + 1), 240);
    CHECK_EQ(app_backoff_s(60, CFG_BACKOFF_AFTER + 2), 480);
    CHECK_EQ(app_backoff_s(60, CFG_BACKOFF_AFTER + 3), CFG_MAX_POLL_S);
    CHECK_EQ(app_backoff_s(60, 255), CFG_MAX_POLL_S);
    CHECK_EQ(app_backoff_s(900, 10), 900);     /* AP-en har bedt om lengre enn maks */
}

static void test_draw_fail_reported(void)
{
    fresh();
    fake_epd_ok = false;
    push_content(77, ESL_LAYOUT_PRICE, 0, 3, price);
    app_poll_once();
    CHECK_EQ(app.shown_id, 0);

    fake_radio_reset();
    fake_epd_ok = true;
    push_content(77, ESL_LAYOUT_PRICE, 0, 3, price);
    app_poll_once();
    CHECK(sent_poll(0)[ESL_POLL_OFF_FLAGS] & ESL_POLL_F_DRAWFAIL);
    CHECK_EQ(app.shown_id, 77);                /* the AP sent it again, drawn now */

    fake_radio_reset();
    push_idle(0);
    app_poll_once();
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_FLAGS] & ESL_POLL_F_DRAWFAIL, 0);
}

static void test_lost_ap(void)
{
    unsigned total = 0;
    int rounds = 0, draws;

    fresh();
    push_content(9, ESL_LAYOUT_PRICE, 0, 3, price);
    app_poll_once();
    draws = fake_draw_count;

    while (total < CFG_LOST_AP_S + CFG_MAX_POLL_S && rounds < 1000) {
        fake_radio_reset();
        total += app_poll_once();
        rounds++;
    }
    CHECK_EQ(fake_draw_count, draws + 1);      /* 'No AP found' drawn once */
    CHECK_EQ(app.shown_id, 0);
    CHECK_EQ(app_backoff_s(app.interval_s, app.misses), CFG_MAX_POLL_S);

    /* The AP is back: POLL reports 0, gets the content again */
    fake_radio_reset();
    push_content(9, ESL_LAYOUT_PRICE, 0, 3, price);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(ESL_GET_U32(sent_poll(0) + ESL_POLL_OFF_CID), 0);
    CHECK_EQ(app.shown_id, 9);
    CHECK_EQ(fake_draw_count, draws + 2);
}

/* ---- Layout download (docs/protokoll.md part 1, "Layout transfer") ---- */

static uint8_t lfile[ESL_LBIN_MAX];
static uint16_t lsize;
static uint32_t lver;

/* Queues the AP's answer: the block of lfile at pos (at most one chunk), version ver */
static void push_block(uint32_t ver, uint16_t pos)
{
    uint8_t n = (uint8_t)(lsize - pos > ESL_LAYOUT_CHUNK_MAX ? ESL_LAYOUT_CHUNK_MAX : lsize - pos);
    uint8_t len = make_layout_block(pl, 0, ver, lsize, pos, lfile + pos, n);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, len));
}

static void push_all_blocks(void)
{
    uint16_t pos;
    for (pos = 0; pos < lsize; pos += ESL_LAYOUT_CHUNK_MAX)
        push_block(lver, pos);
}

/* Payload of sent frame i, and its LAYOUT_REQ position */
static const uint8_t *sent(int i)
{
    return fake_tx[i] + ESL_MAC_HDR_LEN;
}

static const char *const shelf[] = { "Kaffe 500 g", "49,90", "kr 99,80/kg", "Tilbud" };

static void test_layout_download(void)
{
    fresh();
    lsize = make_test_layout_file(lfile, &lver);
    CHECK(lsize > ESL_LAYOUT_CHUNK_MAX && lsize <= 2 * ESL_LAYOUT_CHUNK_MAX);   /* two blocks */

    /* POLL -> LAYOUT@0, LAYOUT_REQ(94) -> LAYOUT@94, then POLL again -> CONTENT with layout 0 */
    push_all_blocks();
    push_content(4242, ESL_LAYOUT_FILE + 0, ESL_CF_PRICE_HIGHLIGHT, 4, shelf);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(fake_tx_count, 3);
    CHECK_EQ(sent(1)[0], ESL_MSG_LAYOUT_REQ);
    CHECK_EQ(fake_tx_len[1], ESL_MAC_HDR_LEN + ESL_LREQ_LEN);
    CHECK_EQ(sent(1)[ESL_LREQ_OFF_MODEL], MODEL_CODE);
    CHECK_EQ(sent(1)[ESL_LREQ_OFF_MAX], ESL_LAYOUT_CHUNK_MAX);
    CHECK(ESL_GET_U32(sent(1) + ESL_LREQ_OFF_VER) == lver);
    CHECK_EQ(ESL_GET_U16(sent(1) + ESL_LREQ_OFF_POS), ESL_LAYOUT_CHUNK_MAX);
    CHECK_EQ(sent(2)[0], ESL_MSG_POLL);
    CHECK(ESL_GET_U32(sent(2) + ESL_POLL_OFF_LAYOUT_VER) == lver);
    CHECK_EQ(sent(2)[ESL_POLL_OFF_FLAGS] & ESL_POLL_F_LAYOUTFAIL, 0);
    CHECK(layout_file_ver == lver);
    CHECK_EQ(layout_file_len, lsize);
    CHECK(memcmp(layout_file, lfile, lsize) == 0);

    /* Drawn with layout 0 of the file: the black band at the top, white text in it */
    CHECK_EQ(app.shown_id, 4242);
    CHECK_EQ(fake_draw_count, 2);
    CHECK_EQ(fake_fb[1][1], 1);                 /* band */
    CHECK_EQ(fake_fb[1][DRAW_W - 2], 1);
    CHECK_EQ(fake_fb[40][1], 0);                /* below the band */

    /* The same version offered again is not downloaded again (T17) */
    fake_radio_reset();
    push_block(lver, 0);
    app_poll_once();
    CHECK_EQ(fake_tx_count, 1);
    CHECK(layout_file_ver == lver);
    CHECK_EQ(fake_draw_count, 2);
}

static void test_layout_resume(void)
{
    fresh();
    lsize = make_test_layout_file(lfile, &lver);

    /* Only the first block arrives: no answer to LAYOUT_REQ (nor to the retry) */
    push_block(lver, 0);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);      /* the AP did answer the POLL */
    CHECK_EQ(app.misses, 0);
    CHECK_EQ(fake_tx_count, 3);                 /* POLL, LAYOUT_REQ, retry */
    CHECK_EQ(ESL_GET_U16(sent(2) + ESL_LREQ_OFF_POS), ESL_LAYOUT_CHUNK_MAX);
    CHECK(layout_file_ver == 0);
    CHECK_EQ(app.dl_got, ESL_LAYOUT_CHUNK_MAX);

    /* Next round the AP offers block 0 again; the tag goes on from where it stopped */
    fake_radio_reset();
    push_block(lver, 0);
    push_block(lver, ESL_LAYOUT_CHUNK_MAX);
    push_idle(0);
    app_poll_once();
    CHECK_EQ(ESL_GET_U32(sent(0) + ESL_POLL_OFF_LAYOUT_VER), 0);
    CHECK_EQ(ESL_GET_U16(sent(1) + ESL_LREQ_OFF_POS), ESL_LAYOUT_CHUNK_MAX);
    CHECK(layout_file_ver == lver);
}

static void test_layout_changed_midway(void)
{
    uint32_t old;
    fresh();
    lsize = make_test_layout_file(lfile, &lver);
    old = lver ^ 0x5A5A;                        /* the version the AP had when the tag started */

    /* Block 0 of the old version, then the AP answers the request with block 0 of the new one */
    push_block(old, 0);
    push_block(lver, 0);
    push_block(lver, ESL_LAYOUT_CHUNK_MAX);
    push_idle(0);
    app_poll_once();
    CHECK_EQ(fake_tx_count, 4);
    CHECK(ESL_GET_U32(sent(1) + ESL_LREQ_OFF_VER) == old);
    CHECK(ESL_GET_U32(sent(2) + ESL_LREQ_OFF_VER) == lver);    /* restarted on the new version */
    CHECK_EQ(ESL_GET_U16(sent(2) + ESL_LREQ_OFF_POS), ESL_LAYOUT_CHUNK_MAX);
    CHECK(layout_file_ver == lver);
}

static void test_layout_rejected(void)
{
    fresh();
    lsize = make_test_layout_file(lfile, &lver);
    lver ^= 1;                                  /* the CRC check fails */
    push_all_blocks();
    push_idle(0);
    app_poll_once();
    CHECK(layout_file_ver == 0);
    CHECK(app.lay_rejected == lver);
    CHECK(sent(2)[ESL_POLL_OFF_FLAGS] & ESL_POLL_F_LAYOUTFAIL);

    /* The same version is not tried again, and the flag stays until a good file arrives */
    fake_radio_reset();
    push_block(lver, 0);
    app_poll_once();
    CHECK_EQ(fake_tx_count, 1);
    CHECK(sent(0)[ESL_POLL_OFF_FLAGS] & ESL_POLL_F_LAYOUTFAIL);

    /* A file for another model is rejected too */
    fresh();
    lsize = make_test_layout_file(lfile, &lver);
    lfile[1] ^= 3;
    lver = proto_crc32(0, lfile, lsize);
    push_all_blocks();
    push_idle(0);
    app_poll_once();
    CHECK(layout_file_ver == 0);
    CHECK(app.lay_rejected == lver);

    /* A good file clears the flag */
    fake_radio_reset();
    lsize = make_test_layout_file(lfile, &lver);
    push_all_blocks();
    push_idle(0);
    app_poll_once();
    CHECK(layout_file_ver == lver);
    CHECK_EQ(app.lay_rejected, 0);
    CHECK_EQ(sent(2)[ESL_POLL_OFF_FLAGS] & ESL_POLL_F_LAYOUTFAIL, 0);

    /* Too big for the tag: rejected without asking for blocks */
    fresh();
    {
        uint8_t len = make_layout_block(pl, 0, 0x1234, ESL_LBIN_MAX + 1, 0, lfile, 10);
        fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, len));
        push_idle(0);
        app_poll_once();
        CHECK_EQ(fake_tx_count, 2);             /* POLL, POLL with the fail flag */
        CHECK(sent(1)[ESL_POLL_OFF_FLAGS] & ESL_POLL_F_LAYOUTFAIL);
    }
}

static void test_layout_new_version_redraws(void)
{
    uint8_t len;
    fresh();
    lsize = make_test_layout_file(lfile, &lver);
    push_all_blocks();
    push_content(77, ESL_LAYOUT_FILE + 1, 0, 1, shelf);
    app_poll_once();
    CHECK_EQ(app.shown_id, 77);
    CHECK_EQ(fake_draw_count, 2);

    /* A new file: the content drawn with the old one is fetched again (POLL with content_id 0) */
    fake_radio_reset();
    lfile[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_H] = 40;   /* band 40 px instead of 30 */
    lver = proto_crc32(0, lfile, lsize);
    push_all_blocks();
    push_content(77, ESL_LAYOUT_FILE + 1, 0, 1, shelf);
    app_poll_once();
    CHECK(layout_file_ver == lver);
    CHECK_EQ(ESL_GET_U32(sent(2) + ESL_POLL_OFF_CID), 0);
    CHECK_EQ(fake_draw_count, 3);
    CHECK_EQ(app.shown_id, 77);

    /* Content with a built-in layout is not fetched again when the file changes */
    fake_radio_reset();
    push_content(78, ESL_LAYOUT_PRICE, 0, 3, price);
    app_poll_once();
    fake_radio_reset();
    lfile[ESL_LBIN_HDR_LEN + 1 + ESL_LE_OFF_H] = 30;
    lver = proto_crc32(0, lfile, lsize);
    push_all_blocks();
    push_idle(0);
    app_poll_once();
    CHECK(ESL_GET_U32(sent(2) + ESL_POLL_OFF_CID) == 78);

    /* The AP has no file any more (version 0): back to the built-in layouts only */
    fake_radio_reset();
    len = make_layout_block(pl, 0, 0, 0, 0, lfile, 0);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, len));
    push_idle(0);
    app_poll_once();
    CHECK(layout_file_ver == 0);
    CHECK_EQ(fake_tx_count, 2);
    CHECK_EQ(ESL_GET_U32(sent(1) + ESL_POLL_OFF_LAYOUT_VER), 0);
}

static void test_file_layout_without_file(void)
{
    /* CONTENT for a file layout the tag does not have: shown with the built-in text layout */
    fresh();
    push_content(99, ESL_LAYOUT_FILE + 3, 0, 2, shelf);
    app_poll_once();
    CHECK_EQ(app.shown_id, 99);
    CHECK_EQ(fake_draw_count, 2);
    CHECK(fake_ink_count(1) > 100);
}

static void test_config_from_ap(void)
{
    fresh();
    push_config(120, 0x5A5A, 120, 0, 0);
    CHECK_EQ(app_poll_once(), 120);            /* uses the new interval at once */
    CHECK_EQ(app.cfg.ver, 0x5A5A);
    CHECK_EQ(fake_draw_count, 1);              /* settings draw nothing */

    /* the next POLL confirms it with cfg_ver; IDLE keeps the interval */
    fake_radio_reset();
    push_idle(0);
    CHECK_EQ(app_poll_once(), 120);
    CHECK_EQ(ESL_GET_U16(sent_poll(0) + ESL_POLL_OFF_CFG_VER), 0x5A5A);

    /* next_poll_s in a later reply still wins for that round, as before */
    fake_radio_reset();
    push_idle(300);
    CHECK_EQ(app_poll_once(), 300);

    /* poll_s 0 = back to the built-in interval; next_poll_s 0 = keep that */
    fake_radio_reset();
    push_config(0, 0, 0, 0, 0);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(app.cfg.ver, 0);

    /* values below the minimum are raised to it */
    fake_radio_reset();
    push_config(0, 7, 1, 2, 3);
    CHECK_EQ(app_poll_once(), ESL_CFG_POLL_MIN_S);
    CHECK_EQ(app.cfg.max_poll_s, ESL_CFG_POLL_MIN_S);
    CHECK_EQ(app.cfg.lost_ap_s, ESL_CFG_LOST_AP_MIN_S);

    /* kept in flash (T12): written once per change, never with the radio on */
    CHECK_EQ(fake_store_writes, 3);            /* 0x5A5A, then 0, then 7 (IDLE/300 wrote nothing) */
    CHECK(!fake_store_radio_on_write);
    fake_radio_reset();
    push_config(0, 7, 1, 2, 3);                /* the same settings again: no write */
    app_poll_once();
    CHECK_EQ(fake_store_writes, 3);

    /* a restart keeps them: the first POLL already reports cfg_ver and uses the interval */
    fake_radio_reset();
    app_init();
    CHECK_EQ(app.cfg.ver, 7);
    CHECK_EQ(app.cfg.lost_ap_s, ESL_CFG_LOST_AP_MIN_S);
    CHECK_EQ(app.interval_s, ESL_CFG_POLL_MIN_S);
    push_idle(0);
    CHECK_EQ(app_poll_once(), ESL_CFG_POLL_MIN_S);
    CHECK_EQ(ESL_GET_U16(sent_poll(0) + ESL_POLL_OFF_CFG_VER), 7);

    /* a damaged or foreign record (other format) is ignored */
    fake_store[0] = 0x55;
    fake_radio_reset();
    app_init();
    CHECK_EQ(app.cfg.ver, 0);
    CHECK_EQ(app.interval_s, CFG_POLL_S);
    fake_store_len = 1;                        /* e.g. the HAL test counter (hwtest.c) */
    app_init();
    CHECK_EQ(app.cfg.ver, 0);

    /* a new tag has nothing in flash */
    fresh();
    CHECK_EQ(app.cfg.ver, 0);
    CHECK_EQ(app.interval_s, CFG_POLL_S);
}

static void test_config_backoff_and_lost(void)
{
    unsigned total = 0;
    int rounds = 0, draws;

    /* max_poll_s caps the backoff instead of CFG_MAX_POLL_S */
    fresh();
    push_config(30, 1, 30, 120, 0);
    app_poll_once();
    CHECK_EQ(app_backoff_s(30, CFG_BACKOFF_AFTER + 5), 120);
    CHECK_EQ(app_backoff_s(60, 255), 120);

    /* lost_ap_s = never: no "No AP found" screen however long the AP is gone */
    fake_radio_reset();
    push_config(0, 2, 60, 0, ESL_CFG_LOST_AP_NEVER);
    app_poll_once();
    draws = fake_draw_count;
    while (total < 4 * CFG_LOST_AP_S && rounds < 1000) {
        fake_radio_reset();
        total += app_poll_once();
        rounds++;
    }
    CHECK_EQ(fake_draw_count, draws);

    /* a short lost_ap_s shows it sooner */
    fresh();
    push_config(0, 3, 60, 0, 600);
    app_poll_once();
    draws = fake_draw_count;
    total = 0;
    rounds = 0;
    while (total < 600 && rounds < 1000) {
        fake_radio_reset();
        total += app_poll_once();
        rounds++;
    }
    CHECK_EQ(fake_draw_count, draws + 1);
    CHECK(rounds < 10);
}

/* Layout, content and CONFIG together: the AP sends them in that order
   (docs/protokoll.md part 1, "Priority"), and the POLL carries both cfg_ver and
   the layout version so the AP can tell what the tag already has. */
static void test_layout_content_config(void)
{
    fresh();
    lsize = make_test_layout_file(lfile, &lver);

    /* Round 1: layout download, then the extra POLL gets the content */
    push_all_blocks();
    push_content(500, ESL_LAYOUT_FILE + 0, 0, 2, shelf);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK(layout_file_ver == lver);
    CHECK_EQ(app.shown_id, 500);
    CHECK_EQ(ESL_GET_U16(sent(2) + ESL_POLL_OFF_CFG_VER), 0);

    /* Round 2: CONFIG; the POLL already reports the layout version */
    fake_radio_reset();
    push_config(0, 0x1357, 90, 0, 0);
    CHECK_EQ(app_poll_once(), 90);
    CHECK_EQ(fake_tx_len[0], ESL_MAC_HDR_LEN + ESL_POLL_LEN_LAYOUT);
    CHECK(ESL_GET_U32(sent(0) + ESL_POLL_OFF_LAYOUT_VER) == lver);
    CHECK_EQ(ESL_GET_U32(sent(0) + ESL_POLL_OFF_CID), 500);
    CHECK_EQ(app.cfg.ver, 0x1357);
    CHECK_EQ(fake_draw_count, 2);              /* CONFIG draws nothing */

    /* Round 3: both versions go back in the same POLL */
    fake_radio_reset();
    push_idle(0);
    CHECK_EQ(app_poll_once(), 90);
    CHECK_EQ(ESL_GET_U16(sent(0) + ESL_POLL_OFF_CFG_VER), 0x1357);
    CHECK(ESL_GET_U32(sent(0) + ESL_POLL_OFF_LAYOUT_VER) == lver);

    /* A new layout file whose extra POLL is answered with CONFIG (no content
       uses the file): the settings are applied in the same round */
    fresh();
    lsize = make_test_layout_file(lfile, &lver);
    push_all_blocks();
    push_config(0, 0x2468, 45, 0, 0);
    CHECK_EQ(app_poll_once(), 45);
    CHECK(layout_file_ver == lver);
    CHECK_EQ(app.cfg.ver, 0x2468);
    CHECK_EQ(fake_tx_count, 3);
}

void test_app(void)
{
    RUN(test_boot);
    RUN(test_next_poll_from_ap);
    RUN(test_content_draws_once);
    RUN(test_retry_then_answer);
    RUN(test_other_tag_frame_skipped);
    RUN(test_backoff);
    RUN(test_draw_fail_reported);
    RUN(test_lost_ap);
    RUN(test_layout_download);
    RUN(test_layout_resume);
    RUN(test_layout_changed_midway);
    RUN(test_layout_rejected);
    RUN(test_layout_new_version_redraws);
    RUN(test_file_layout_without_file);
    RUN(test_config_from_ap);
    RUN(test_config_backoff_and_lost);
    RUN(test_layout_content_config);
}
