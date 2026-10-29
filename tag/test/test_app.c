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

static void push_config_long(uint16_t ver, uint16_t long_poll, uint16_t long_after)
{
    uint8_t n = make_config_long(pl, 0, ver, 0, 0, 0, long_poll, long_after);
    fake_rx_push(fr, make_ap_frame(fr, 0, CFG_PAN_ID, fake_mac, pl, n));
}

/* CONFIG that announces the AP's channel (0 = the default) */
static void push_config_channel(uint16_t ver, uint16_t channel)
{
    uint8_t n = make_config(pl, 0, ver, 0, 0, 0);
    ESL_PUT_U16(pl + ESL_CONFIG_OFF_CHANNEL, channel);
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
    fake_ap_channel = 0;
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
    CHECK_EQ(fake_tx_len[0], ESL_MAC_HDR_LEN + ESL_POLL_LEN_LINK);
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

/* The link block in POLL (#261): the signal of the AP's last answer, the rounds lost
   before this one, and whether the last answered round needed its retry. The retry
   itself is the same frame, byte for byte (so a MIC on it, #260, stays valid). */
static void test_link_block(void)
{
    fresh();
    push_idle(0);
    app_poll_once();
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_RX_RSSI], 0);      /* nothing heard yet */
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_RX_LQI], 0);
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_LOST], 0);
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_LINKF], 0);

    /* two rounds without an answer: each sends the POLL twice, unchanged */
    fake_radio_reset();
    app_poll_once();
    CHECK_EQ(fake_tx_count, 2);
    CHECK_EQ(fake_tx_len[0], fake_tx_len[1]);
    CHECK(memcmp(fake_tx[0], fake_tx[1], fake_tx_len[0]) == 0);
    CHECK_EQ(sent_poll(1)[ESL_POLL_OFF_LINKF], 0);
    CHECK_EQ(sent_poll(1)[ESL_POLL_OFF_RX_RSSI], fake_rx_rssi);   /* from the answer before */
    CHECK_EQ(sent_poll(1)[ESL_POLL_OFF_RX_LQI], fake_rx_lqi);
    fake_radio_reset();
    app_poll_once();

    /* answered at the second try: it reports both lost rounds; the retry is not flagged yet */
    fake_radio_reset();
    fake_rx_lose();
    push_idle(0);
    fake_rx_rssi = 0x90;
    app_poll_once();
    CHECK_EQ(fake_tx_count, 2);
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_LOST], 2);
    CHECK(memcmp(fake_tx[0], fake_tx[1], fake_tx_len[0]) == 0);
    CHECK_EQ(app.misses, 0);

    /* the next round says the last one needed its retry, with the new answer's signal */
    fake_radio_reset();
    push_idle(0);
    app_poll_once();
    CHECK_EQ(fake_tx_count, 1);
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_LOST], 0);
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_LINKF], ESL_POLL_LF_RETRY);
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_RX_RSSI], 0x90);

    /* and the one after that starts clean */
    fake_radio_reset();
    push_idle(0);
    app_poll_once();
    CHECK_EQ(sent_poll(0)[ESL_POLL_OFF_LINKF], 0);
    fake_rx_rssi = 0xC8;
}

/* #261: after a day without the AP, one POLL an hour (or as the AP set it) */
static void test_long_backoff(void)
{
    unsigned long total = 0;
    uint16_t s = 0;
    int rounds = 0;

    fresh();
    CHECK_EQ(app_long_poll_s(0), 0);
    CHECK_EQ(app_long_poll_s(CFG_LONG_AFTER_MIN * 60ul - 1), 0);
    CHECK_EQ(app_long_poll_s(CFG_LONG_AFTER_MIN * 60ul), CFG_LONG_POLL_S);

    push_idle(0);
    app_poll_once();
    while (total < 2ul * CFG_LONG_AFTER_MIN * 60 && rounds < 2000) {
        fake_radio_reset();
        s = app_poll_once();
        CHECK(app.lost_s < CFG_LONG_AFTER_MIN * 60ul ? s <= CFG_MAX_POLL_S : s == CFG_LONG_POLL_S);
        total += s;
        rounds++;
    }
    CHECK_EQ(s, CFG_LONG_POLL_S);
    /* a day of 600 s sleeps and then hours: far fewer wake-ups than at 600 s */
    CHECK(rounds < CFG_LONG_AFTER_MIN / 10 + 30);

    /* the AP is back: the first answer ends it */
    fake_radio_reset();
    push_idle(0);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);

    /* set from the AP: 30 min after 10 min, kept in flash; "never" keeps the backoff */
    fake_radio_reset();
    push_config_long(9, 1800, 10);
    app_poll_once();
    CHECK_EQ(app.cfg.long_poll_s, 1800);
    CHECK_EQ(app.cfg.long_after_min, 10);
    CHECK_EQ(app_long_poll_s(600), 1800);
    app_init();
    CHECK_EQ(app.cfg.long_poll_s, 1800);
    CHECK_EQ(app.cfg.long_after_min, 10);
    fake_radio_reset();
    push_config_long(10, 5, ESL_CFG_LONG_NEVER);
    app_poll_once();
    CHECK_EQ(app.cfg.long_poll_s, ESL_CFG_LONG_POLL_MIN_S);      /* raised to the minimum */
    CHECK_EQ(app_long_poll_s(0xFFFFFFul), 0);
    fake_radio_reset();
    push_config_long(11, 600, 1);
    app_poll_once();
    CHECK_EQ(app.cfg.long_after_min, ESL_CFG_LONG_AFTER_MIN_MIN);
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
    memset(fake_store, 0, sizeof(fake_store));
    fake_store[0] = 4;
    ESL_PUT_U16(fake_store + 1, 9);
    ESL_PUT_U16(fake_store + 3, 90);
    fake_store[14] = 30;                       /* not a channel: the built-in one */
    fake_store_len = 15;
    fake_draw_count = 0;
    app_init();
    CHECK_EQ(app.cfg.ver, 9);
    CHECK_EQ(app.interval_s, 90);
    CHECK_EQ(app.shown_id, 0);
    CHECK_EQ(app.channel, CFG_CHANNEL);
    CHECK_EQ(fake_draw_count, 1);

    /* the record before the channel (format 3, 13 bytes) and the one before the long backoff
       (format 1, 9 bytes) are ignored: the AP sends CONFIG again. So is the one of the
       firmware that drew content itself (format 2) */
    fake_store[0] = 3;
    fake_store_len = 13;
    app_init();
    CHECK_EQ(app.cfg.ver, 0);
    fake_store[0] = 1;
    fake_store_len = 9;
    app_init();
    CHECK_EQ(app.cfg.ver, 0);
    fake_store[0] = 2;
    fake_store_len = 60;
    app_init();
    CHECK_EQ(app.cfg.ver, 0);
    CHECK_EQ(app.interval_s, CFG_POLL_S);
    fake_store[0] = 1;
    fake_store_len = 9;
    app_init();
    CHECK_EQ(app.cfg.ver, 0);

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

/* ---- A planned channel move (#256, docs/protokoll.md "Channel") ---- */

static void test_channel_move(void)
{
    fresh();
    CHECK_EQ(fake_channel, CFG_CHANNEL);
    /* CONFIG announces channel 20: the tag stays where it is and confirms it there */
    push_config_channel(0x2020, 20);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(app.cfg.channel, 20);
    CHECK_EQ(app.channel, CFG_CHANNEL);
    CHECK_EQ(fake_store_writes, 1);
    fake_radio_reset();
    push_idle(0);
    app_poll_once();
    CHECK_EQ(fake_tx_channel[0], CFG_CHANNEL);
    CHECK_EQ(ESL_GET_U16(sent_poll(0) + ESL_POLL_OFF_CFG_VER), 0x2020);

    /* the AP moves: no answer on 25, the POLL on 20 (with its retry) is answered */
    fake_radio_reset();
    fake_ap_channel = 20;
    push_idle(0);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(app.misses, 0);                   /* found in the same round: not a miss */
    CHECK_EQ(fake_tx_count, 3);                /* 25, its retry, then 20 */
    CHECK_EQ(fake_tx_channel[1], CFG_CHANNEL);
    CHECK_EQ(fake_tx_channel[2], 20);
    CHECK_EQ(app.channel, 20);
    CHECK_EQ(fake_store_writes, 2);            /* kept for the next start */
    CHECK(!fake_store_radio_on_write);

    /* from now on it starts on 20 */
    fake_radio_reset();
    push_idle(0);
    app_poll_once();
    CHECK_EQ(fake_tx_count, 1);
    CHECK_EQ(fake_tx_channel[0], 20);
    fake_radio_reset();
    app_init();
    CHECK_EQ(app.channel, 20);
    CHECK_EQ(fake_channel, 20);

    /* an AP that is just gone: the built-in channel only from the third miss in a row (another
       AP there would keep a tag that lost one reply to noise) */
    fake_radio_reset();
    fake_ap_channel = 11;
    app_poll_once();
    CHECK_EQ(fake_tx_count, 2);                /* 20 (also announced) twice */
    CHECK_EQ(app.misses, 1);
    fake_radio_reset();
    app_poll_once();
    CHECK_EQ(fake_tx_count, 2);
    CHECK_EQ(app.misses, 2);
    fake_radio_reset();
    app_poll_once();
    CHECK_EQ(fake_tx_count, 4);                /* then 25 twice too */
    CHECK_EQ(fake_tx_channel[2], CFG_CHANNEL);
    CHECK_EQ(app.misses, 3);
    CHECK_EQ(app.channel, 20);

    /* an AP that went back to the built-in channel before the tag heard of it ("channel_now") */
    fake_radio_reset();
    fake_ap_channel = CFG_CHANNEL;
    push_idle(0);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(app.misses, 0);
    CHECK_EQ(app.channel, CFG_CHANNEL);
    CHECK_EQ(fake_channel, CFG_CHANNEL);
    app_init();
    CHECK_EQ(app.channel, CFG_CHANNEL);        /* kept */
    fake_radio_reset();
    fake_ap_channel = 20;                      /* and back to 20, where it was announced */
    push_idle(0);
    app_poll_once();
    CHECK_EQ(app.channel, 20);
    fake_radio_reset();
    fake_ap_channel = 20;
    push_config_channel(0x2525, 0);            /* back to the default channel */
    app_poll_once();
    CHECK_EQ(app.cfg.channel, 0);
    CHECK_EQ(app.channel, 20);
    fake_radio_reset();
    fake_ap_channel = 11;
    app_poll_once();
    CHECK_EQ(fake_tx_count, 4);                /* 20 twice, then 25 twice */
    CHECK_EQ(fake_tx_channel[3], CFG_CHANNEL);
    CHECK_EQ(fake_channel, 20);                /* the next round starts on 20 again */
    CHECK_EQ(app.channel, 20);
    fake_ap_channel = 0;
}

/* A tag whose AP is on a channel it was never told of finds it by looking on the others (#258) */
static void test_channel_scan(void)
{
    uint8_t i;

    fresh();
    fake_ap_channel = 20;                      /* the AP moved without telling this tag */
    for (i = 0; i < 2; i++) {                  /* two misses: only its own channel, POLL and retry */
        fake_radio_reset();
        app_poll_once();
        CHECK_EQ(fake_tx_count, 2);
    }
    CHECK_EQ(app.misses, 2);
    /* the third looks on 15, then 20, where the AP answers */
    fake_radio_reset();
    push_idle(0);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(fake_tx_count, 4);
    CHECK_EQ(fake_tx_channel[2], 15);
    CHECK_EQ(fake_tx_channel[3], 20);
    CHECK_EQ(app.misses, 0);
    CHECK_EQ(app.channel, 20);
    app_init();
    CHECK_EQ(app.channel, 20);                 /* kept in flash */

    /* the AP is gone: the third miss looks on every channel of the list once (not on 20
       and 25 again), each later one on the next channel only */
    fake_ap_channel = 13;
    for (i = 0; i < 2; i++) {
        fake_radio_reset();
        app_poll_once();
        CHECK_EQ(fake_tx_count, 4);            /* 20 twice, the built-in 25 twice */
    }
    fake_radio_reset();
    app_poll_once();
    CHECK_EQ(fake_tx_count, 7);                /* and 15, 26, 11 */
    CHECK_EQ(fake_tx_channel[4], 15);
    CHECK_EQ(fake_tx_channel[5], 26);
    CHECK_EQ(fake_tx_channel[6], 11);
    CHECK_EQ(fake_channel, 20);                /* the next round starts on 20 again */
    fake_radio_reset();
    app_poll_once();
    CHECK_EQ(fake_tx_count, 5);                /* one more: 15 */
    CHECK_EQ(fake_tx_channel[4], 15);
    fake_radio_reset();
    app_poll_once();
    CHECK_EQ(fake_tx_count, 5);                /* 20 is skipped: 26 */
    CHECK_EQ(fake_tx_channel[4], 26);
    CHECK_EQ(app.channel, 20);

    /* found on 11 by the rotation */
    fake_ap_channel = 11;
    fake_radio_reset();
    push_idle(0);
    CHECK_EQ(app_poll_once(), CFG_POLL_S);
    CHECK_EQ(fake_tx_channel[4], 11);
    CHECK_EQ(app.channel, 11);
    CHECK_EQ(app.misses, 0);
    fake_ap_channel = 0;
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
    RUN(test_link_block);
    RUN(test_long_backoff);
    RUN(test_channel_move);
    RUN(test_channel_scan);
}
