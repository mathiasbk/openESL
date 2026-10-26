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
                          .ieee_addr = 0x0123456789ABCDEFull, .cfg = c, .app_version = "0.8.2-dev",
                          .features = ESL_COPROC_FEAT_DISPLAY}, h2;
    n = esl_coproc_encode_hello(f, &h);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK_INT(p.type, ESL_COPROC_HELLO);
    CHECK(esl_coproc_decode_hello(p.payload, p.len, &h2));
    CHECK_INT(h2.version, ESL_COPROC_VERSION);
    CHECK_INT(h2.fw, 0x0102);
    CHECK(h2.ieee_addr == 0x0123456789ABCDEFull);
    CHECK_INT(h2.cfg.pan, PAN);
    CHECK_STR(h2.app_version, "0.8.2-dev");
    CHECK_INT(h2.features, ESL_COPROC_FEAT_DISPLAY);
    /* a radio before link v2 sends the first 19 bytes only: no app version, no features */
    CHECK(esl_coproc_decode_hello(p.payload, 19, &h2));
    CHECK_INT(h2.cfg.pan, PAN);
    CHECK_STR(h2.app_version, "");
    CHECK_INT(h2.features, 0);
    CHECK(!esl_coproc_decode_hello(p.payload, 18, &h2));
    memset(h.app_version, 'v', ESL_COPROC_APP_VERSION_MAX); /* a full one, no zero in the frame */
    n = esl_coproc_encode_hello(f, &h);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK(esl_coproc_decode_hello(p.payload, p.len, &h2));
    CHECK_INT(strlen(h2.app_version), ESL_COPROC_APP_VERSION_MAX);

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

    uint32_t u, off;
    uint8_t st;
    n = esl_coproc_encode_ota_begin(f, 262144);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK_INT(p.type, ESL_COPROC_OTA_BEGIN);
    CHECK(esl_coproc_decode_ota_begin(p.payload, p.len, &u));
    CHECK_INT(u, 262144);
    CHECK(!esl_coproc_decode_ota_begin(p.payload, 3, &u));

    uint8_t img[ESL_COPROC_OTA_CHUNK + 1];
    const uint8_t *dp;
    size_t dl;
    for (size_t i = 0; i < sizeof(img); i++)
        img[i] = (uint8_t)(i ^ 0x5A);
    n = esl_coproc_encode_ota_data(f, 0x12345, img, ESL_COPROC_OTA_CHUNK);
    CHECK(n > 0 && n <= ESL_COPROC_FRAME_MAX);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK_INT(p.type, ESL_COPROC_OTA_DATA);
    CHECK(esl_coproc_decode_ota_data(p.payload, p.len, &off, &dp, &dl));
    CHECK_INT(off, 0x12345);
    CHECK_INT(dl, ESL_COPROC_OTA_CHUNK);
    CHECK(memcmp(dp, img, dl) == 0);
    CHECK_INT(esl_coproc_encode_ota_data(f, 0, img, ESL_COPROC_OTA_CHUNK + 1), 0);
    n = esl_coproc_encode_ota_data(f, 7, img, 0); /* an empty part is still a part */
    CHECK_INT(feed(&p, f, n), 1);
    CHECK(esl_coproc_decode_ota_data(p.payload, p.len, &off, &dp, &dl));
    CHECK_INT(dl, 0);
    CHECK(!esl_coproc_decode_ota_data(p.payload, 3, &off, &dp, &dl));

    n = esl_coproc_encode_ota_ack(f, ESL_COPROC_OTA_OFFSET, 4096);
    CHECK_INT(feed(&p, f, n), 1);
    CHECK_INT(p.type, ESL_COPROC_OTA_ACK);
    CHECK(esl_coproc_decode_ota_ack(p.payload, p.len, &st, &off));
    CHECK_INT(st, ESL_COPROC_OTA_OFFSET);
    CHECK_INT(off, 4096);
    CHECK(!esl_coproc_decode_ota_ack(p.payload, 4, &st, &off));

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

/* ---- new radio firmware through the link: the radio's side, with a buffer for its flash ---- */

typedef struct {
    uint8_t slot[4096];
    uint32_t size, written;
    int begins, aborts, ends;
    int fail_write_at; /* a write that reaches this many bytes fails (0: none) */
    bool bad_image;
} fake_flash_t;

static int ff_begin(void *ctx, uint32_t size)
{
    fake_flash_t *f = ctx;
    f->begins++;
    if (size > sizeof(f->slot))
        return ESL_COPROC_OTA_TOO_BIG;
    f->size = size;
    f->written = 0;
    return ESL_COPROC_OTA_OK;
}

static int ff_write(void *ctx, const uint8_t *data, size_t len)
{
    fake_flash_t *f = ctx;
    if (f->fail_write_at && f->written + len >= (uint32_t)f->fail_write_at)
        return ESL_COPROC_OTA_FLASH;
    memcpy(&f->slot[f->written], data, len);
    f->written += (uint32_t)len;
    return ESL_COPROC_OTA_OK;
}

static int ff_end(void *ctx)
{
    fake_flash_t *f = ctx;
    f->ends++;
    return f->bad_image ? ESL_COPROC_OTA_INVALID : ESL_COPROC_OTA_OK;
}

static void ff_abort(void *ctx)
{
    ((fake_flash_t *)ctx)->aborts++;
}

/* Sends one message to the radio through the wire and back: the OTA_ACK's status
 * and offset, -1 when the radio did not answer */
static int ota_msg(esl_coproc_ota_rx_t *o, const uint8_t *frame, size_t n, uint32_t *offset, bool *done)
{
    static esl_coproc_parser_t radio, host;
    uint8_t ack[ESL_COPROC_FRAME_MAX];
    esl_coproc_parser_init(&radio);
    esl_coproc_parser_init(&host);
    if (feed(&radio, frame, n) != 1)
        return -1;
    size_t an = esl_coproc_ota_handle(o, radio.type, radio.payload, radio.len, ack, done);
    if (!an || feed(&host, ack, an) != 1 || host.type != ESL_COPROC_OTA_ACK)
        return -1;
    uint8_t st;
    if (!esl_coproc_decode_ota_ack(host.payload, host.len, &st, offset))
        return -1;
    return st;
}

static int ota_begin(esl_coproc_ota_rx_t *o, uint32_t size, uint32_t *off, bool *done)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    return ota_msg(o, f, esl_coproc_encode_ota_begin(f, size), off, done);
}

static int ota_data(esl_coproc_ota_rx_t *o, uint32_t at, const uint8_t *d, size_t n, uint32_t *off, bool *done)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    return ota_msg(o, f, esl_coproc_encode_ota_data(f, at, d, n), off, done);
}

static int ota_end(esl_coproc_ota_rx_t *o, uint32_t *off, bool *done)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    return ota_msg(o, f, esl_coproc_frame(f, ESL_COPROC_OTA_END, NULL, 0), off, done);
}

static void test_ota(void)
{
    static fake_flash_t ff;
    static uint8_t img[1000];
    esl_coproc_ota_rx_t o = {.begin = ff_begin, .write = ff_write, .end = ff_end, .abort = ff_abort, .ctx = &ff};
    uint32_t off;
    bool done;
    for (size_t i = 0; i < sizeof(img); i++)
        img[i] = (uint8_t)(i * 7 + 1);

    /* nothing open: a part or an end is answered with where it is (nowhere) */
    CHECK_INT(ota_data(&o, 0, img, 10, &off, &done), ESL_COPROC_OTA_OFFSET);
    CHECK_INT(off, 0);
    CHECK_INT(ota_end(&o, &off, &done), ESL_COPROC_OTA_OFFSET);
    CHECK(!done);

    /* a whole image in parts, with the last part's ACK lost once */
    CHECK_INT(ota_begin(&o, sizeof(img), &off, &done), ESL_COPROC_OTA_OK);
    CHECK_INT(off, 0);
    uint32_t at = 0;
    while (at < sizeof(img)) {
        size_t n = sizeof(img) - at < ESL_COPROC_OTA_CHUNK ? sizeof(img) - at : ESL_COPROC_OTA_CHUNK;
        CHECK_INT(ota_data(&o, at, &img[at], n, &off, &done), ESL_COPROC_OTA_OK);
        CHECK_INT(off, at + n);
        if (at == 256) /* again: written once, answered again */
            CHECK_INT(ota_data(&o, at, &img[at], n, &off, &done), ESL_COPROC_OTA_OK);
        CHECK_INT(off, at + n);
        at = off;
    }
    CHECK_INT(ff.written, sizeof(img));
    CHECK_MEM(ff.slot, img, sizeof(img));
    /* a part from further back, or from beyond, is not the next one */
    CHECK_INT(ota_data(&o, 0, img, 10, &off, &done), ESL_COPROC_OTA_OFFSET);
    CHECK_INT(off, sizeof(img));
    CHECK_INT(ota_end(&o, &off, &done), ESL_COPROC_OTA_OK);
    CHECK(done);
    CHECK_INT(ff.ends, 1);
    CHECK(!o.open);

    /* the end before the last part: not done, and the upload stays open */
    CHECK_INT(ota_begin(&o, 300, &off, &done), ESL_COPROC_OTA_OK);
    CHECK_INT(ota_data(&o, 0, img, ESL_COPROC_OTA_CHUNK, &off, &done), ESL_COPROC_OTA_OK);
    CHECK_INT(ota_end(&o, &off, &done), ESL_COPROC_OTA_OFFSET);
    CHECK_INT(off, ESL_COPROC_OTA_CHUNK);
    CHECK(!done);
    CHECK(o.open);
    CHECK_INT(ota_data(&o, 200, img, 10, &off, &done), ESL_COPROC_OTA_OFFSET); /* a gap */
    CHECK_INT(off, ESL_COPROC_OTA_CHUNK);
    /* more than the size: dropped */
    int aborts = ff.aborts;
    CHECK_INT(ota_data(&o, ESL_COPROC_OTA_CHUNK, img, ESL_COPROC_OTA_CHUNK, &off, &done), ESL_COPROC_OTA_OK);
    CHECK_INT(ota_data(&o, 2 * ESL_COPROC_OTA_CHUNK, img, ESL_COPROC_OTA_CHUNK, &off, &done), ESL_COPROC_OTA_TOO_BIG);
    CHECK(!o.open);
    CHECK_INT(ff.aborts, aborts + 1);

    /* a new BEGIN drops the open upload */
    CHECK_INT(ota_begin(&o, 300, &off, &done), ESL_COPROC_OTA_OK);
    CHECK_INT(ota_data(&o, 0, img, 10, &off, &done), ESL_COPROC_OTA_OK);
    CHECK_INT(ota_begin(&o, 300, &off, &done), ESL_COPROC_OTA_OK);
    CHECK_INT(ff.aborts, aborts + 2);
    CHECK_INT(ota_data(&o, 10, img, 10, &off, &done), ESL_COPROC_OTA_OFFSET);
    CHECK_INT(off, 0);

    /* too big for the slot, and size 0 */
    CHECK_INT(ota_begin(&o, sizeof(ff.slot) + 1, &off, &done), ESL_COPROC_OTA_TOO_BIG);
    CHECK(!o.open);
    CHECK_INT(ota_begin(&o, 0, &off, &done), ESL_COPROC_OTA_INVALID);
    CHECK(!o.open);

    /* the flash fails: the upload ends */
    ff.fail_write_at = 200;
    CHECK_INT(ota_begin(&o, 300, &off, &done), ESL_COPROC_OTA_OK);
    CHECK_INT(ota_data(&o, 0, img, ESL_COPROC_OTA_CHUNK, &off, &done), ESL_COPROC_OTA_OK);
    CHECK_INT(ota_data(&o, ESL_COPROC_OTA_CHUNK, img, ESL_COPROC_OTA_CHUNK, &off, &done), ESL_COPROC_OTA_FLASH);
    CHECK(!o.open);
    ff.fail_write_at = 0;

    /* an image the radio cannot boot: the end says so, nothing restarts */
    ff.bad_image = true;
    CHECK_INT(ota_begin(&o, 10, &off, &done), ESL_COPROC_OTA_OK);
    CHECK_INT(ota_data(&o, 0, img, 10, &off, &done), ESL_COPROC_OTA_OK);
    CHECK_INT(ota_end(&o, &off, &done), ESL_COPROC_OTA_INVALID);
    CHECK(!done);
    CHECK(!o.open);

    /* a malformed message gets no answer */
    uint8_t short_begin[3] = {1, 2, 3}, ack[ESL_COPROC_FRAME_MAX];
    CHECK_INT(esl_coproc_ota_handle(&o, ESL_COPROC_OTA_BEGIN, short_begin, sizeof(short_begin), ack, &done), 0);
    CHECK_INT(esl_coproc_ota_handle(&o, ESL_COPROC_PING, NULL, 0, ack, &done), 0);
}

void test_coproc(void)
{
    test_crc();
    test_messages();
    test_resync();
    test_through_link();
    test_ota();
}
