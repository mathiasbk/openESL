#include <string.h>
#include "test.h"
#include "fake.h"
#include "helpers.h"
#include "app.h"
#include "config.h"

static uint8_t pl[ESL_MAX_PAYLOAD], fr[128];

static void push_idle(uint16_t next)
{
    uint8_t n = make_idle(pl, next);
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
    CHECK_EQ(fake_tx_len[0], ESL_MAC_HDR_LEN + ESL_POLL_LEN);
    CHECK_EQ(sent_poll(0)[0], ESL_MSG_POLL);
    CHECK_EQ(ESL_GET_U16(sent_poll(0) + ESL_POLL_OFF_FW), FW_VERSION);
    CHECK_EQ(ESL_GET_U16(sent_poll(0) + ESL_POLL_OFF_BATT), fake_batt_mv);
    CHECK_EQ(ESL_GET_U32(sent_poll(0) + ESL_POLL_OFF_CID), 0);
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_FLAGS], ESL_POLL_F_BOOTED);
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
    push_content(1111, ESL_LAYOUT_PRICE, ESL_CF_PRICE_RED, 3, price);
    app_poll_once();
    CHECK_EQ(fake_draw_count, 2);
    CHECK(fake_ink_count(2) > 100);            /* red price */
    CHECK_EQ(app.shown_id, 1111);

    /* The next POLL reports the new content_id and no longer 'just booted' */
    fake_radio_reset();
    push_content(1111, ESL_LAYOUT_PRICE, ESL_CF_PRICE_RED, 3, price);
    app_poll_once();
    CHECK_EQ(ESL_GET_U32(sent_poll(0) + ESL_POLL_OFF_CID), 1111);
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_FLAGS], 0);
    CHECK_EQ(fake_draw_count, 2);              /* same content: no new drawing (T5) */

    fake_radio_reset();
    push_content(2222, ESL_LAYOUT_PRICE, 0, 3, price2);
    app_poll_once();
    CHECK_EQ(fake_draw_count, 3);
    CHECK_EQ(fake_ink_count(2), 0);
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
}
