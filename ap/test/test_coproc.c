#include "tinytest.h"
#include "esl_coproc.h"
#include "esl_radio.h"
#include <stdlib.h>

/* The UART link between the AP host and the radio coprocessor (esl_coproc.h):
 * framing, every message both ways, resync after garbage, and a fake tag
 * served through the link the way radio_uart.c and ap/radio/ do it. */

#define PAN 0x4553

/* Feeds a byte stream; returns how many frames completed, the last in *p. */
static int feed(esl_coproc_parser_t *p, const uint8_t *b, size_t n)
{
    int frames = 0;
    for (size_t i = 0; i < n; i++)
        frames += esl_coproc_parser_feed(p, b[i]);
    return frames;
}

static void test_crc(void)
{
    /* CRC-16/CCITT-FALSE check value */
    CHECK_INT(esl_coproc_crc16(0xFFFF, (const uint8_t *)"123456789", 9), 0x29B1);
}

static void test_messages(void)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    esl_coproc_parser_t p;
    esl_coproc_parser_init(&p);

    esl_coproc_config_t c = {.channel = 25, .pan = PAN, .short_addr = 0, .tx_power = -3, .flags = ESL_COPROC_CFG_SNIFFER}, c2;
    size_t n = esl_coproc_encode_config(f, &c);
    CHECK_INT(n, 7 + ESL_COPROC_OVERHEAD);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK_INT(p.type, ESL_COPROC_CONFIG);
    CHECK(esl_coproc_decode_config(p.payload, p.len, &c2));
    CHECK_INT(c2.channel, 25);
    CHECK_INT(c2.pan, PAN);
    CHECK_INT(c2.tx_power, -3);
    CHECK_INT(c2.flags, ESL_COPROC_CFG_SNIFFER);
    c2.channel = 10;
    esl_coproc_encode_config(f, &c2);
    feed(&p, f, n);
    CHECK(!esl_coproc_decode_config(p.payload, p.len, &c2)); /* no such channel */

    esl_coproc_hello_t h = {.version = ESL_COPROC_VERSION, .configured = 1, .fw = 0x0102,
                          .ieee_addr = 0x0123456789ABCDEFull, .cfg = c}, h2;
    n = esl_coproc_encode_hello(f, &h);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK_INT(p.type, ESL_COPROC_HELLO);
    CHECK(esl_coproc_decode_hello(p.payload, p.len, &h2));
    CHECK_INT(h2.fw, 0x0102);
    CHECK(h2.ieee_addr == 0x0123456789ABCDEFull);
    CHECK_INT(h2.cfg.pan, PAN);
    CHECK(!esl_coproc_decode_hello(p.payload, p.len - 1, &h2));

    esl_coproc_rx_t rx = {.ref = 7, .rssi = -71, .channel = 25, .len = ESL_COPROC_PSDU_MAX}, rx2;
    for (int i = 0; i < rx.len; i++)
        rx.psdu[i] = (uint8_t)(i * 3);
    n = esl_coproc_encode_rx(f, &rx);
    CHECK_INT(n, 3 + ESL_COPROC_PSDU_MAX + ESL_COPROC_OVERHEAD);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK(esl_coproc_decode_rx(p.payload, p.len, &rx2));
    CHECK_INT(rx2.ref, 7);
    CHECK_INT(rx2.rssi, -71);
    CHECK_INT(rx2.len, ESL_COPROC_PSDU_MAX);
    CHECK(memcmp(rx2.psdu, rx.psdu, rx.len) == 0);
    rx.len = ESL_COPROC_PSDU_MAX + 1;
    CHECK_INT(esl_coproc_encode_rx(f, &rx), 0);

    uint8_t psdu[20] = {1, 2, 3, 4, 5};
    uint8_t ref;
    const uint8_t *pp;
    size_t pl;
    n = esl_coproc_encode_tx(f, 9, psdu, sizeof(psdu));
    CHECK_INT(feed(&p, f, n), 1);
    CHECK_INT(p.type, ESL_COPROC_TX);
    CHECK(esl_coproc_decode_tx(p.payload, p.len, &ref, &pp, &pl));
    CHECK_INT(ref, 9);
    CHECK_INT(pl, sizeof(psdu));
    CHECK(memcmp(pp, psdu, pl) == 0);
    CHECK_INT(esl_coproc_encode_tx(f, 9, psdu, 2), 0);

    esl_coproc_tx_done_t d = {.ref = 9, .status = ESL_COPROC_TX_FAILED, .resends = 6, .rx_to_tx_us = 2345}, d2;
    n = esl_coproc_encode_tx_done(f, &d);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK(esl_coproc_decode_tx_done(p.payload, p.len, &d2));
    CHECK_INT(d2.status, ESL_COPROC_TX_FAILED);
    CHECK_INT(d2.resends, 6);
    CHECK_INT(d2.rx_to_tx_us, 2345);

    esl_coproc_stats_t s = {.rx = 1, .tx = 2, .tx_failed = 3, .tx_resent = 4, .tx_late = 5,
                          .dropped = 6, .link_errors = 7, .uptime_s = 0xDEADBEEF}, s2;
    n = esl_coproc_encode_stats(f, &s);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK(esl_coproc_decode_stats(p.payload, p.len, &s2));
    CHECK(memcmp(&s, &s2, sizeof(s)) == 0);

    esl_coproc_screen_t sc = {.color = {2, 3, 1, 6}}, sc2;
    strcpy(sc.text[0], "OpenESL 0.5.0   ch 25");
    strcpy(sc.text[1], "12 tags, 2 pending");
    strcpy(sc.text[2], "192.168.1.40");
    memset(sc.text[3], 'x', ESL_COPROC_SCREEN_TEXT_MAX); /* a full line */
    n = esl_coproc_encode_screen(f, &sc);
    CHECK(n > 0 && n <= ESL_COPROC_FRAME_MAX);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK_INT(p.type, ESL_COPROC_SCREEN);
    CHECK(esl_coproc_decode_screen(p.payload, p.len, &sc2));
    CHECK(memcmp(&sc, &sc2, sizeof(sc)) == 0);
    CHECK(!esl_coproc_decode_screen(p.payload, p.len - 1, &sc2)); /* cut short */
    p.payload[1] = ESL_COPROC_SCREEN_TEXT_MAX + 1;
    CHECK(!esl_coproc_decode_screen(p.payload, p.len, &sc2)); /* line too long */

    n = esl_coproc_frame(f, ESL_COPROC_PING, NULL, 0);
    CHECK_INT(n, ESL_COPROC_OVERHEAD);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK_INT(p.type, ESL_COPROC_PING);
    CHECK_INT(p.len, 0);

    CHECK_INT(p.bad_crc, 0);
    CHECK_INT(p.skipped, 0);
}

static void test_resync(void)
{
    uint8_t buf[600], f[ESL_COPROC_FRAME_MAX];
    esl_coproc_parser_t p;
    esl_coproc_parser_init(&p);
    size_t n = 0;

    /* ROM boot text, then a frame */
    const char *boot = "ESP-ROM:esp32c6-20220919\r\nrst:0x1 (POWERON)\r\n";
    memcpy(buf, boot, strlen(boot));
    n += strlen(boot);
    n += esl_coproc_frame(&buf[n], ESL_COPROC_PING, NULL, 0);
    CHECK_INT(feed(&p, buf, n), 1);
    CHECK_INT(p.type, ESL_COPROC_PING);

    /* A frame cut off by a reset, then a whole one: the cut one's bytes are
       taken as payload until a CRC fails, and the parser finds the next sync */
    esl_coproc_rx_t rx = {.ref = 1, .rssi = -50, .channel = 11, .len = 30};
    memset(rx.psdu, ESL_COPROC_SYNC, rx.len); /* sync bytes inside the payload, too */
    size_t fl = esl_coproc_encode_rx(f, &rx);
    n = 0;
    memcpy(buf, f, 12);
    n += 12;
    for (int i = 0; i < 4; i++) {
        memcpy(&buf[n], f, fl);
        n += fl;
    }
    int got = feed(&p, buf, n);
    CHECK(got >= 2); /* the cut frame swallows at most the start of the next */
    CHECK_INT(p.type, ESL_COPROC_RX);
    CHECK(p.bad_crc >= 1);

    /* The same with the quiet line after the cut noticed: nothing is lost but the cut frame */
    esl_coproc_parser_init(&p);
    CHECK_INT(feed(&p, f, 12), 0);
    esl_coproc_parser_gap(&p);
    CHECK_INT(p.cut, 1);
    CHECK_INT(feed(&p, &buf[12], n - 12), 4);
    CHECK_INT(p.bad_crc, 0);
    esl_coproc_parser_gap(&p); /* between frames: nothing to drop */
    CHECK_INT(p.cut, 1);

    /* A flipped bit is caught */
    esl_coproc_parser_init(&p);
    fl = esl_coproc_encode_rx(f, &rx);
    f[10] ^= 0x10;
    CHECK_INT(feed(&p, f, fl), 0);
    CHECK_INT(p.bad_crc, 1);

    /* A length over the maximum is dropped at once */
    esl_coproc_parser_init(&p);
    uint8_t bad[] = {ESL_COPROC_SYNC, ESL_COPROC_RX, ESL_COPROC_PAYLOAD_MAX + 1};
    CHECK_INT(feed(&p, bad, sizeof(bad)), 0);
    CHECK_INT(p.too_long, 1);
    n = esl_coproc_frame(f, ESL_COPROC_PING, NULL, 0);
    CHECK_INT(feed(&p, f, n), 1);
}

/* A fake tag polls an AP whose radio sits behind the link: the "radio" wraps
 * each frame in RX, the "host" decodes it, answers with esl_radio_handle()
 * and sends TX, and the "radio" hands the PSDU back to the tag. */
static void test_through_link(void)
{
    static esl_store_t st0;
    esl_store_t *st = &st0;
    esl_store_init(st, 4, NULL, 0);
    const uint64_t m = 0x00000123456789A8ull;
    esl_faketag_t *t = malloc(sizeof(*t));
    esl_faketag_init(t, m, 0x0100, 3000, ESL_MODEL_29);

    esl_coproc_parser_t host, radio;
    esl_coproc_parser_init(&host);
    esl_coproc_parser_init(&radio);
    uint8_t f[ESL_COPROC_FRAME_MAX], psdu[ESL_PSDU_MAX], reply[ESL_PSDU_MAX];
    int replies = 0;

    for (int round = 0; round < 3; round++) {
        /* radio side: a POLL arrives, goes up as RX */
        esl_coproc_rx_t rx = {.ref = (uint8_t)round, .rssi = -60, .channel = 11};
        rx.len = (uint8_t)esl_faketag_poll(t, PAN, rx.psdu);
        size_t n = esl_coproc_encode_rx(f, &rx);
        CHECK_INT(feed(&host, f, n), 1);

        /* host side */
        esl_coproc_rx_t got;
        CHECK(esl_coproc_decode_rx(host.payload, host.len, &got));
        size_t len = esl_radio_handle(st, NULL, PAN, got.psdu, got.len, got.rssi, 1000u * (round + 1), reply, NULL);
        CHECK(len > 0);
        n = esl_coproc_encode_tx(f, got.ref, reply, len);
        CHECK(n > 0);

        /* radio side: TX back to the tag */
        CHECK_INT(feed(&radio, f, n), 1);
        uint8_t ref;
        const uint8_t *pp;
        size_t pl;
        CHECK(esl_coproc_decode_tx(radio.payload, radio.len, &ref, &pp, &pl));
        CHECK_INT(ref, round);
        memcpy(psdu, pp, pl);
        if (esl_faketag_receive(t, psdu, pl))
            replies++;
    }
    CHECK_INT(replies, 3);
    esl_tag_t tag;
    CHECK(esl_store_get(st, m, &tag));
    CHECK_INT(esl_store_count(st), 1);
    esl_store_free(st);
    free(t);
}

void test_coproc(void)
{
    test_crc();
    test_messages();
    test_resync();
    test_through_link();
}
