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

static void push_config(uint16_t next, uint16_t ver, uint16_t poll, uint16_t max_poll, uint16_t lost)
{
    uint8_t n = make_config(pl, next, ver, poll, max_poll, lost);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
}

/* A reply of the old firmware's kinds (CONTENT, LAYOUT): only its type and length matter */
static void push_old(uint8_t type)
{
    memset(pl, 0, 20);
    pl[ESL_OFF_TYPE] = type;
    pl[1] = ESL_PROTO_VER;
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, 20));
}

/* POLL payload in sent frame i */
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

static void test_boot(void)
{
    fresh();
    CHECK_EQ(fake_draw_count, 1);              /* oppstartsskjerm (T9) */
    CHECK(fake_ink_count(1) > 100);
    CHECK_EQ(fake_radio_on, 0);                /* drawn with the radio off: radio_init does not power it */
    push_idle(0);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(fake_tx_count, 1);
    CHECK_EQ(fake_tx_len[0], ESL_MAC_HDR_LEN + ESL_POLL_LEN_CFG);
    CHECK_EQ(ESL_GET_U16(sent_poll(0) + ESL_POLL_OFF_CFG_VER), 0);      /* built-in settings */
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

static void test_old_replies_ignored(void)
{
    /* CONTENT (0x03) and LAYOUT (0x04), retired since the AP draws the screen, are not
       answers: the tag tries again and then counts the round as missed, and draws nothing */
    fresh();
    push_old(0x03);
    push_old(0x04);
    CHECK_EQ(app_poll_once(), app_backoff_s(CFG_POLL_S, 1));
    CHECK_EQ(fake_tx_count, 2);
    CHECK_EQ(app.misses, 1);
    CHECK_EQ(fake_draw_count, 1);

    /* in the same window as a real answer, the answer is taken */
    fake_radio_reset();
    push_old(0x03);
    push_idle(77);
    CHECK_EQ(app_poll_once(), 77);
    CHECK_EQ(fake_tx_count, 1);
    CHECK_EQ(app.misses, 0);
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
    push_idle(55);
    CHECK_EQ(app_poll_once(), 55);
    CHECK_EQ(fake_tx_count, 1);                /* the reply to us came in the same window */
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

static void test_lost_ap(void)
{
    unsigned total = 0;
    int rounds = 0, draws;

    fresh();
    push_idle(0);
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

    /* The AP is back with nothing for us: the start screen again */
    fake_radio_reset();
    push_idle(0);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(ESL_GET_U32(sent_poll(0) + ESL_POLL_OFF_CID), 0);
    CHECK_EQ(fake_draw_count, draws + 2);
    CHECK_EQ(app.lost_shown, 0);
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

/* T9: "No AP found" when CFG_LOST_AP_S have passed since the last answer, not a sleep earlier */
static void test_lost_ap_on_time(void)
{
    unsigned total = 0;
    int rounds = 0, draws;

    fresh();
    push_idle(0);
    total = app_poll_once();         /* the last answer; time counts from here */
    draws = fake_draw_count;

    while (fake_draw_count == draws && rounds < 1000) {
        fake_radio_reset();
        total += app_poll_once();
        rounds++;
    }
    CHECK_EQ(fake_draw_count, draws + 1);
    /* drawn in the round that woke at exactly CFG_LOST_AP_S; total includes the sleep after it */
    CHECK_EQ(total - app_backoff_s(app.interval_s, app.misses), CFG_LOST_AP_S);
    CHECK_EQ(app.lost_s, CFG_LOST_AP_S);
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

/* ---- Settings in flash (requirement T12) ---- */

static void test_restart_bad_flash(void)
{
    /* A record with settings: kept over a restart, with the start screen (no image) */
    fresh();
    fake_store[0] = 1;
    ESL_PUT_U16(fake_store + 1, 9);
    ESL_PUT_U16(fake_store + 3, 90);
    ESL_PUT_U16(fake_store + 5, 0);
    ESL_PUT_U16(fake_store + 7, 0);
    fake_store_len = 9;
    fake_draw_count = 0;
    app_init();
    CHECK_EQ(app.cfg.ver, 9);
    CHECK_EQ(app.interval_s, 90);
    CHECK_EQ(app.shown_id, 0);
    CHECK_EQ(fake_draw_count, 1);

    /* the record of the firmware that drew content itself (format 2): ignored */
    fake_store[0] = 2;
    fake_store_len = 60;
    app_init();
    CHECK_EQ(app.cfg.ver, 0);
    CHECK_EQ(app.interval_s, CFG_POLL_S);

    /* the start screen cannot be drawn: reported in the first POLL */
    fake_store_clear();
    fake_epd_ok = false;
    app_init();
    fake_epd_ok = true;
    fake_radio_reset();
    push_idle(0);
    app_poll_once();
    CHECK(sent_poll(0)[ESL_POLL_OFF_FLAGS] & ESL_POLL_F_DRAWFAIL);
}

void test_app(void)
{
    RUN(test_boot);
    RUN(test_next_poll_from_ap);
    RUN(test_old_replies_ignored);
    RUN(test_retry_then_answer);
    RUN(test_other_tag_frame_skipped);
    RUN(test_backoff);
    RUN(test_lost_ap);
    RUN(test_lost_ap_on_time);
    RUN(test_config_from_ap);
    RUN(test_config_backoff_and_lost);
    RUN(test_restart_bad_flash);
}
