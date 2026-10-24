#include "tinytest.h"
#include "esl_crc32.h"
#include "esl_image.h"
#include "esl_nlink.h"
#include "esl_radio.h"
#include "esl_render.h"
#include <stdlib.h>

/* The link between a network radio and its host (esl_nlink), as on the devices but
 * with two byte queues for the TCP connection: host (store, images, tag firmware,
 * esl_rhost) on one end, radio (esl_rcache) on the other, fake tags at the radio. */

#define PAN 0x4553
#define TAGS 8
#define IMAGES 4
#define LOG 64

/* ---- the connection: a queue each way ---- */

typedef struct {
    uint8_t b[96 * 1024];
    size_t n;
    size_t cut;      /* drop everything past this many bytes (the link breaks), 0 = never */
} pipe_t;

static pipe_t to_host, to_radio;

static bool pipe_write(void *ctx, const uint8_t *d, size_t n)
{
    pipe_t *p = ctx;
    if (p->n + n > sizeof(p->b))
        return false;
    memcpy(p->b + p->n, d, n);
    p->n += n;
    return true;
}

/* ---- the host ---- */

static esl_store_t st;
static esl_image_t images;
static esl_tagfw_t fw;
static esl_rhost_t host;
static esl_rtag_state_t sent[TAGS];
static esl_nlink_host_t hl;
static esl_nlink_rx_t hrx;
static uint8_t hrx_buf[8192];
static uint64_t host_ms;
static uint64_t host_now(void)
{
    return host_ms;
}

static uint8_t fwmem[ESL_MODEL_COUNT - 1][ESL_TAGFW_SLOT];
static bool fw_erase(void *ctx, uint8_t model)
{
    (void)ctx;
    memset(fwmem[model - 1], 0xFF, ESL_TAGFW_SLOT);
    return true;
}
static bool fw_write(void *ctx, uint8_t model, uint32_t off, const void *d, size_t n)
{
    (void)ctx;
    memcpy(fwmem[model - 1] + off, d, n);
    return true;
}
static bool fw_read(void *ctx, uint8_t model, uint32_t off, void *d, size_t n)
{
    (void)ctx;
    if (off + n > ESL_TAGFW_SLOT)
        return false;
    memcpy(d, fwmem[model - 1] + off, n);
    return true;
}

/* ---- the radio ---- */

static esl_rcache_t rc;
static esl_rtag_t rtags[TAGS];
static esl_rimage_t rimages[IMAGES];
static esl_rlog_t rlog[LOG];
static esl_nlink_radio_t rl;
static esl_nlink_rx_t rrx;
static uint8_t rrx_buf[8192];
static uint8_t radio_flash[ESL_MODEL_COUNT - 1][ESL_TAGFW_SLOT]; /* its own copy of the tag firmware */
static uint8_t radio_channel;
static uint16_t radio_pan;

static void radio_config(void *ctx, uint8_t channel, uint16_t pan)
{
    (void)ctx;
    radio_channel = channel;
    radio_pan = pan;
}
static bool radio_write_fw(void *ctx, uint8_t model, uint32_t off, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (off + n > ESL_TAGFW_SLOT)
        return false;
    memcpy(radio_flash[model - 1] + off, d, n);
    return true;
}
static bool radio_read_fw(void *ctx, uint8_t model, size_t off, uint8_t *buf, size_t n)
{
    (void)ctx;
    if (off + n > ESL_TAGFW_SLOT)
        return false;
    memcpy(buf, radio_flash[model - 1] + off, n);
    return true;
}

static void radio_start(uint32_t boot)
{
    static uint8_t *idata;
    static const esl_rcache_ops_t ops = {NULL, radio_read_fw};
    static const esl_nlink_radio_ops_t lops = {NULL, radio_config, radio_write_fw};
    size_t cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    if (!idata)
        idata = malloc(IMAGES * cap);
    for (unsigned i = 0; i < IMAGES; i++)
        rimages[i].data = idata + i * cap;
    esl_rcache_init(&rc, 0, rtags, TAGS, rimages, IMAGES, cap, rlog, LOG, &ops);
    rc.boot = boot;
    memset(radio_flash, 0xFF, sizeof(radio_flash));
    esl_nlink_radio_init(&rl, &rc, &lops, pipe_write, &to_host);
}

/* A new TCP connection: empty queues, fresh parsers, HELLO */
static void connect(void)
{
    to_host.n = to_radio.n = 0;
    esl_nlink_rx_init(&hrx, hrx_buf, sizeof(hrx_buf));
    esl_nlink_rx_init(&rrx, rrx_buf, sizeof(rrx_buf));
    esl_nlink_host_init(&hl, &host, &fw, 20, PAN, pipe_write, &to_radio, host_now);
    esl_nlink_hello_t h = {rc.boot, 10000, 0xC6C6C6C6C6ull, TAGS, IMAGES, (uint32_t)rc.image_cap};
    CHECK(esl_nlink_radio_hello(&rl, &h));
}

static bool host_on(void *ctx, uint8_t type, const uint8_t *p, size_t len)
{
    return esl_nlink_host_on(ctx, type, p, len);
}
static bool radio_on(void *ctx, uint8_t type, const uint8_t *p, size_t len)
{
    return esl_nlink_radio_on(ctx, type, p, len);
}

/* Moves the bytes both ways until both queues are empty, a byte at a time to the host
   (frames split anywhere). With a cut, the radio gets that many bytes and then nothing:
   the connection broke. */
static void pump(void)
{
    bool broken = false;
    while (to_host.n || to_radio.n) {
        pipe_t h = to_host, r = to_radio;
        to_host.n = to_radio.n = 0;
        for (size_t i = 0; i < h.n; i++)
            CHECK(esl_nlink_feed(&hrx, h.b + i, 1, host_on, &hl));
        size_t n = broken ? 0 : r.n;
        if (to_radio.cut && n >= to_radio.cut) {
            n = to_radio.cut;
            broken = true;
        } else if (to_radio.cut) {
            to_radio.cut -= n;
        }
        CHECK(esl_nlink_feed(&rrx, r.b, n, radio_on, &rl));
    }
}

static void boot(void)
{
    static uint8_t *mem, *img;
    static const esl_tagfw_ops_t fwops = {NULL, NULL, NULL, fw_erase, fw_write, fw_read, NULL, NULL};
    size_t cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    if (!mem)
        mem = malloc(esl_image_mem_size());
    if (!img)
        img = malloc(cap);
    esl_image_init(&images, mem, NULL);
    esl_radio_set_image(&images);
    memset(fwmem, 0xFF, sizeof(fwmem));
    esl_tagfw_init(&fw, &fwops);
    esl_radio_set_tagfw(&fw);
    esl_store_free(&st);
    esl_store_init(&st, TAGS, NULL, 0);
    esl_rhost_init(&host, &st, NULL, &images, &fw, PAN, sent, TAGS, img, cap);
    radio_start(0xB0070001);
    to_radio.cut = 0;
    connect();
    pump();
}

static void sync_at(uint64_t now_ms)
{
    host_ms = now_ms;
    CHECK(esl_nlink_radio_sync(&rl, now_ms + 5000));
    pump();
}

static uint8_t wake(esl_faketag_t *t, uint64_t now_ms)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    size_t n = esl_faketag_poll(t, PAN, rx);
    size_t m = esl_rcache_handle(&rc, rx, n, -55, now_ms + 5000, tx);
    uint8_t type = m ? esl_faketag_receive(t, tx, m) : 0;
    for (int req = 0; (type == ESL_MSG_IMG || type == ESL_MSG_FW) && req < ESL_FAKETAG_FW_REQS; req++) {
        n = type == ESL_MSG_IMG ? esl_faketag_img_request(t, PAN, rx) : esl_faketag_fw_request(t, PAN, rx);
        if (!n || !(m = esl_rcache_handle(&rc, rx, n, -55, now_ms + 5000, tx)) || esl_faketag_receive(t, tx, m) != type)
            break;
    }
    return type;
}

static void set_price(uint64_t mac, const char *name, const char *p)
{
    esl_content_t c;
    esl_set_result_t r;
    esl_content_init(&c, ESL_LAYOUT_PRICE, 0);
    esl_content_add_field(&c, (const uint8_t *)name, strlen(name));
    esl_content_add_field(&c, (const uint8_t *)p, strlen(p));
    CHECK_INT(esl_store_set(&st, mac, &c, &r), ESL_STORE_OK);
}

static void test_nlink_flow(void)
{
    esl_faketag_t a;
    esl_tag_t t;
    boot();

    /* HELLO is answered with the channel and PAN */
    CHECK(hl.hello && hl.radio.max_tags == TAGS && hl.radio.id == 0xC6C6C6C6C6ull);
    CHECK_INT(radio_channel, 20);
    CHECK_INT(rc.pan, PAN);

    /* a tag through the link: heard by the radio, known on the host, given its image */
    esl_faketag_init(&a, 0xF1, 1, 2800, ESL_MODEL_29);
    CHECK_INT(wake(&a, 1000), ESL_MSG_IDLE);
    sync_at(2000);
    CHECK_INT(rl.ends, 1);
    CHECK(esl_store_get(&st, 0xF1, &t) && t.seen && t.battery_mv == 2800 && t.rssi == -55);
    CHECK_INT((long long)t.last_seen_ms, 1000);
    set_price(0xF1, "Kaffe", "79,90");
    sync_at(3000);
    CHECK_INT(host.stats.images, 1);
    CHECK_INT(wake(&a, 61000), ESL_MSG_IMG);
    CHECK_INT(a.img_shown, 1);
    CHECK_INT(wake(&a, 121000), ESL_MSG_IDLE);
    sync_at(122000);
    CHECK(esl_store_get(&st, 0xF1, &t) && t.shown_content_id == a.shown && !esl_tag_pending(&t));

    /* the channel changes on the host: CONFIG with the next answer */
    esl_nlink_host_set_config(&hl, 25, PAN);
    sync_at(130000);
    CHECK_INT(radio_channel, 25);

    /* tag firmware, part by part into the radio's flash, then to the tag */
    static uint8_t file[ESL_FWF_HDR_LEN + 3072];
    char buf[256];
    esl_sb_t sb;
    uint8_t *app = file + ESL_FWF_HDR_LEN;
    for (size_t i = 0; i < 3072; i++)
        app[i] = (uint8_t)(i * 7 + 1);
    uint32_t crc = esl_crc32(app, 3072), sz = 3072, magic = ESL_FWF_MAGIC;
    memset(file, 0, ESL_FWF_HDR_LEN);
    memcpy(file + ESL_FWF_OFF_MAGIC, &magic, 4);
    file[ESL_FWF_OFF_FORMAT] = ESL_FWF_FORMAT;
    file[ESL_FWF_OFF_MODEL] = ESL_MODEL_29;
    file[ESL_FWF_OFF_VER] = 3;
    memcpy(file + ESL_FWF_OFF_SIZE, &sz, 4);
    memcpy(file + ESL_FWF_OFF_CRC, &crc, 4);
    esl_sb_init(&sb, buf, sizeof(buf));
    CHECK_INT(esl_tagfw_part(&fw, 0, sizeof(file), file, sizeof(file), 0, &sb), 200);
    esl_faketag_t b;
    esl_faketag_init(&b, 0xF2, 1, 3000, ESL_MODEL_29);
    b.ota = true;
    b.images = false;
    wake(&b, 140000);
    sync_at(141000);
    CHECK_INT(rc.fw[ESL_MODEL_29 - 1].ver, 3);
    CHECK(memcmp(radio_flash[ESL_MODEL_29 - 1], app, 3072) == 0);
    CHECK_INT(wake(&b, 201000), ESL_MSG_FW);
    CHECK_INT(b.fw, 3);
}

static void test_nlink_broken(void)
{
    esl_faketag_t a;
    esl_tag_t t;
    boot();
    esl_faketag_init(&a, 0xF3, 1, 3000, ESL_MODEL_29);
    wake(&a, 1000);
    sync_at(2000);
    set_price(0xF3, "Te", "29,90");

    /* the link breaks in the middle of the answer (the image): the radio keeps its log */
    wake(&a, 61000);
    to_radio.cut = 40;
    sync_at(62000);
    CHECK(rl.in_sync);
    CHECK_INT(esl_rcache_log_count(&rc), 1);
    CHECK(esl_store_get(&st, 0xF3, &t));
    unsigned polls = t.polls;

    /* a new connection: the log comes again and is not counted twice, and the radio is
       told everything again */
    to_radio.cut = 0;
    connect();
    pump();
    unsigned resets = host.stats.resets;
    sync_at(63000);
    CHECK_INT(host.stats.skipped, 1);
    CHECK_INT(host.stats.resets, resets + 1);
    CHECK(esl_store_get(&st, 0xF3, &t) && t.polls == polls);
    CHECK(!rl.in_sync);
    CHECK_INT(wake(&a, 121000), ESL_MSG_IMG);

    /* the radio restarts: a new boot number, and it gets everything again */
    radio_start(0xB0070002);
    connect();
    pump();
    sync_at(130000);
    CHECK_INT(rc.n_tags, 1);
    CHECK_INT(rc.pan, PAN);

    /* frames the link will not take: too large, or empty */
    uint8_t big[3] = {0xFF, 0xFF, ESL_NLINK_LOG}, empty[2] = {0, 0};
    esl_nlink_rx_init(&hrx, hrx_buf, sizeof(hrx_buf));
    CHECK(!esl_nlink_feed(&hrx, big, sizeof(big), host_on, &hl));
    esl_nlink_rx_init(&hrx, hrx_buf, sizeof(hrx_buf));
    CHECK(!esl_nlink_feed(&hrx, empty, sizeof(empty), host_on, &hl));

    /* a SYNC before HELLO, a LOG with no SYNC: the connection is closed */
    esl_nlink_host_t fresh;
    esl_nlink_host_init(&fresh, &host, &fw, 20, PAN, pipe_write, &to_radio, host_now);
    uint8_t syncmsg[1] = {0};
    CHECK(!esl_nlink_host_on(&fresh, ESL_NLINK_SYNC, syncmsg, 1));
    CHECK(!esl_nlink_host_on(&hl, ESL_NLINK_LOG, syncmsg, 1));
}

void test_nlink(void)
{
    test_nlink_flow();
    test_nlink_broken();
}
