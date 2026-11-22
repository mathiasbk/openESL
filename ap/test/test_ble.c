#include "tinytest.h"
#include "esl_ble.h"

/* The BLE transport adapter: connect, pack, esl_gicisky_send, disconnect, with a fake
   backend. The protocol itself is covered by test_gicisky. */

static uint8_t planes[ESL_GICISKY_PLANES_SIZE];
static uint8_t fb[ESL_GICISKY_FB_SIZE];

static struct {
    unsigned connects, disconnects, writes, waits, data_writes;
    bool connect_ok, write_ok;
    uint8_t first[4 + ESL_GICISKY_CHUNK];
    size_t first_len;
    esl_push_info_t info;
} fake;

static void reset_fake(void)
{
    memset(&fake, 0, sizeof fake);
    fake.connect_ok = true;
    fake.write_ok = true;
    fake.info.battery_mv = 2900;
    fake.info.rssi = -55;
    memset(planes, 0, sizeof planes); /* no ink: the tag shows white */
}

static bool fake_connect(void *ctx, uint64_t mac)
{
    (void)ctx;
    (void)mac;
    fake.connects++;
    return fake.connect_ok;
}

static void fake_disconnect(void *ctx)
{
    (void)ctx;
    fake.disconnects++;
}

static bool fake_write(void *ctx, esl_gicisky_chan_t chan, const uint8_t *data, size_t len)
{
    (void)ctx;
    fake.writes++;
    if (!fake.write_ok)
        return false;
    if (chan == ESL_GICISKY_DATA) {
        if (fake.data_writes == 0) {
            size_t n = len < sizeof fake.first ? len : sizeof fake.first;
            memcpy(fake.first, data, n);
            fake.first_len = len;
        }
        fake.data_writes++;
    }
    return true;
}

static bool fake_wait(void *ctx, const uint8_t *prefix, size_t len)
{
    (void)ctx;
    (void)prefix;
    (void)len;
    fake.waits++;
    return true;
}

static bool fake_info(void *ctx, esl_push_info_t *info)
{
    (void)ctx;
    *info = fake.info;
    return true;
}

static void test_ble_send(void)
{
    esl_ble_t b = {
        .ctx = NULL,
        .fb = fb,
        .fb_cap = sizeof fb,
        .model = 0,
        .connect = fake_connect,
        .disconnect = fake_disconnect,
        .write = fake_write,
        .wait = fake_wait,
        .info = fake_info,
    };
    esl_push_info_t info = {0};

    reset_fake();
    CHECK_INT(esl_ble_send(&b, 0x1234, 0, planes, sizeof planes, &info), ESL_PUSH_SENT);
    CHECK_INT(fake.connects, 1);
    CHECK_INT(fake.disconnects, 1);
    CHECK_INT(fake.writes, 3 + ESL_GICISKY_CHUNKS);
    CHECK_INT(fake.waits, 3 + ESL_GICISKY_CHUNKS);
    CHECK_INT(fake.data_writes, ESL_GICISKY_CHUNKS);
    CHECK_INT(fake.first_len, 4 + ESL_GICISKY_CHUNK);
    CHECK_INT(fake.first[0], 0);
    CHECK_INT(fake.first[1], 0);
    CHECK_INT(fake.first[2], 0);
    CHECK_INT(fake.first[3], 0);
    for (unsigned i = 0; i < ESL_GICISKY_CHUNK; i++)
        CHECK_INT(fake.first[4 + i], 0x55);
    CHECK_INT(info.battery_mv, 2900);
    CHECK_INT(info.rssi, -55);

    /* connect fails: no disconnect, nothing written */
    reset_fake();
    fake.connect_ok = false;
    CHECK_INT(esl_ble_send(&b, 0x1234, 0, planes, sizeof planes, &info), ESL_PUSH_AWAY);
    CHECK_INT(fake.connects, 1);
    CHECK_INT(fake.disconnects, 0);
    CHECK_INT(fake.writes, 0);

    /* a write fails after the connection: it is disconnected */
    reset_fake();
    fake.write_ok = false;
    CHECK_INT(esl_ble_send(&b, 0x1234, 0, planes, sizeof planes, &info), ESL_PUSH_AWAY);
    CHECK_INT(fake.connects, 1);
    CHECK_INT(fake.disconnects, 1);
    CHECK_INT(fake.writes, 1);

    /* a tag of another model is refused before connecting */
    reset_fake();
    b.model = 7;
    CHECK_INT(esl_ble_send(&b, 0x1234, 8, planes, sizeof planes, &info), ESL_PUSH_REFUSED);
    CHECK_INT(fake.connects, 0);
    b.model = 0;

    /* bad arguments */
    reset_fake();
    CHECK_INT(esl_ble_send(NULL, 0x1234, 0, planes, sizeof planes, &info), ESL_PUSH_AWAY);
    CHECK_INT(fake.connects, 0);
    esl_ble_t bad = b;
    bad.fb_cap = ESL_GICISKY_FB_SIZE - 1;
    CHECK_INT(esl_ble_send(&bad, 0x1234, 0, planes, sizeof planes, &info), ESL_PUSH_AWAY);
    CHECK_INT(fake.connects, 0);
    CHECK_INT(esl_ble_send(&b, 0x1234, 0, planes, sizeof planes - 1, &info), ESL_PUSH_AWAY);
    CHECK_INT(fake.connects, 0);

    /* info is optional */
    reset_fake();
    esl_ble_t no_info = b;
    no_info.info = NULL;
    info.battery_mv = 1;
    info.rssi = 1;
    CHECK_INT(esl_ble_send(&no_info, 0x1234, 0, planes, sizeof planes, &info), ESL_PUSH_SENT);
    CHECK_INT(info.battery_mv, 1);
    CHECK_INT(info.rssi, 1);
}

void test_ble(void)
{
    test_ble_send();
}
