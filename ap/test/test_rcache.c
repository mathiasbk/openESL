#include "tinytest.h"
#include "esl_crc32.h"
#include "esl_image.h"
#include "esl_radio.h"
#include "esl_rcache.h"
#include "esl_render.h"
#include <stdlib.h>

/* The reply cache of a network radio (esl_rcache, docs/ap-netradio.md): fake tags
 * talk to the cache only, and a host (a store, as on the S3) learns what happened
 * from the log and tells the cache what to answer, as the link will. */

#define PAN 0x4553
#define TAGS 8
#define IMAGES 3
#define LOG 64

static esl_store_t st;
static esl_image_t images;
static esl_rcache_t rc;
static esl_rtag_t rtags[TAGS];
static esl_rimage_t rimages[IMAGES];
static esl_rlog_t rlog[LOG];

/* tag firmware for the cache, as the C6 would keep it in flash */
static uint8_t fw_bytes[2048]; /* a whole number of KB, as the tag checks */
static bool fw_read(void *ctx, uint8_t model, size_t off, uint8_t *buf, size_t n)
{
    (void)ctx;
    if (model != ESL_MODEL_29 || off + n > sizeof(fw_bytes))
        return false;
    memcpy(buf, fw_bytes + off, n);
    return true;
}

static void boot(void)
{
    static uint8_t *mem, *idata;
    static const esl_rcache_ops_t ops = {NULL, fw_read};
    size_t cap = esl_render_size(ESL_MODEL_29) + ESL_IMG_RLE_MARGIN(esl_render_size(ESL_MODEL_29));
    if (!mem)
        mem = malloc(esl_image_mem_size());
    if (!idata)
        idata = malloc(IMAGES * cap);
    esl_image_init(&images, mem, NULL);
    esl_radio_set_image(&images);
    esl_store_free(&st);
    esl_store_init(&st, 16, NULL, 0);
    for (unsigned i = 0; i < IMAGES; i++)
        rimages[i].data = idata + i * cap;
    esl_rcache_init(&rc, PAN, rtags, TAGS, rimages, IMAGES, cap, rlog, LOG, &ops);
}

static esl_content_t price(const char *name, const char *p)
{
    esl_content_t c;
    esl_content_init(&c, ESL_LAYOUT_PRICE, 0);
    esl_content_add_field(&c, (const uint8_t *)name, strlen(name));
    esl_content_add_field(&c, (const uint8_t *)p, strlen(p));
    return c;
}

/* What the host tells the cache about one tag: what esl_radio_handle() would answer
   it with, from the store, and the tag's image when it has one. */
static void host_push(uint64_t mac, uint64_t now_ms, uint16_t fw_ver)
{
    esl_tag_t t;
    esl_rtag_state_t s = {mac, esl_store_tag_poll_s(&st, mac), false, {0}, 0, fw_ver};
    if (esl_store_get(&st, mac, &t)) {
        s.has_cfg = t.seen && t.cfg_capable;
        esl_tag_effective_cfg(&t, esl_store_get_next_poll(&st), &s.cfg);
        bool busy;
        s.image_id = t.image_capable ? esl_image_prepare(&images, &st, NULL, mac, 0, now_ms, &busy) : 0;
        for (unsigned i = 0; i < ESL_IMAGE_BUFS; i++) {
            esl_image_buf_t *b = &images.buf[i];
            if (b->mac == mac && b->id == s.image_id && !esl_rcache_has_image(&rc, b->id))
                CHECK(esl_rcache_put_image(&rc, b->id, b->model, b->fmt, b->data, b->size));
        }
        esl_image_release(&images, mac);
    }
    CHECK(esl_rcache_set_tag(&rc, &s));
}

/* A sync: the host runs the log through esl_radio_handle() (its replies go nowhere),
   then tells the cache about every tag in it. */
static unsigned host_sync(uint64_t now_ms, uint16_t fw_ver)
{
    uint8_t tx[ESL_PSDU_MAX];
    uint64_t macs[LOG];
    unsigned n = esl_rcache_log_count(&rc), nm = 0;
    for (unsigned i = 0; i < n; i++) {
        const esl_rlog_t *e = esl_rcache_log_at(&rc, i);
        esl_rx_frame_t f;
        esl_radio_handle(&st, NULL, PAN, e->psdu, e->len, e->rssi, e->t_ms, tx, NULL);
        if (esl_frame_parse_from_tag(e->psdu, e->len, &f)) {
            unsigned k = 0;
            while (k < nm && macs[k] != f.src_mac)
                k++;
            if (k == nm)
                macs[nm++] = f.src_mac;
        }
    }
    esl_rcache_log_drop(&rc, n);
    for (unsigned k = 0; k < nm; k++)
        host_push(macs[k], now_ms, fw_ver);
    esl_rcache_want_clear(&rc);
    return n;
}

/* One wake-up of a fake tag against the cache, as esl_faketag_round() against the AP. */
static uint8_t round_rc(esl_faketag_t *t, uint64_t now_ms)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    size_t n = esl_faketag_poll(t, PAN, rx);
    size_t m = esl_rcache_handle(&rc, rx, n, -60, now_ms, tx);
    uint8_t type = m ? esl_faketag_receive(t, tx, m) : 0;
    for (int req = 0; (type == ESL_MSG_IMG || type == ESL_MSG_FW) && req < ESL_FAKETAG_FW_REQS; req++) {
        n = type == ESL_MSG_IMG ? esl_faketag_img_request(t, PAN, rx) : esl_faketag_fw_request(t, PAN, rx);
        if (!n)
            break;
        m = esl_rcache_handle(&rc, rx, n, -60, now_ms, tx);
        if (!m || esl_faketag_receive(t, tx, m) != type)
            break;
    }
    return type;
}

static void test_rcache_image(void)
{
    esl_set_result_t r;
    esl_faketag_t a;
    esl_content_t milk = price("Melk", "24,90");
    boot();
    esl_faketag_init(&a, 0x0A, 1, 3000, ESL_MODEL_29);

    /* a tag the cache knows nothing of: IDLE with a short interval, the radio wants the host soon */
    CHECK_INT(round_rc(&a, 1000), ESL_MSG_IDLE);
    CHECK_INT(a.next_poll_s, ESL_RCACHE_WAIT_S);
    CHECK(rc.want && rc.want_mac == 0x0A);
    CHECK_INT(rc.stats.waits, 1);
    CHECK_INT(esl_rcache_log_count(&rc), 1);
    CHECK(!esl_rcache_sync_due(&rc, 1030, 0));  /* the tag may still be talking */
    CHECK(esl_rcache_sync_due(&rc, 1100, 0));   /* quiet, and it lacks something */
    CHECK(!esl_rcache_sync_due(&rc, 1100, 900)); /* not right after the last sync */

    /* the host hears of it through the log: the tag is in its table, as if it had heard it */
    CHECK_INT(esl_store_set(&st, 0x0A, &milk, &r), ESL_STORE_OK);
    CHECK_INT(host_sync(1100, 0), 1);
    esl_tag_t t;
    CHECK(esl_store_get(&st, 0x0A, &t) && t.seen && t.model == ESL_MODEL_29 && t.battery_mv == 3000);
    CHECK(!rc.want);
    CHECK_INT(esl_rcache_log_count(&rc), 0);

    /* now the cache answers with the image, block by block, and the tag shows it */
    CHECK_INT(round_rc(&a, 6000), ESL_MSG_IMG);
    CHECK(a.shown != 0 && a.img_shown == 1);
    CHECK(esl_rcache_has_image(&rc, a.shown));
    CHECK(rc.stats.blocks > 0);
    CHECK_INT(round_rc(&a, 66000), ESL_MSG_IDLE); /* it reports the id: nothing more */

    /* the host replays it all: the same table as if it had served the tag itself */
    host_sync(66100, 0);
    CHECK(esl_store_get(&st, 0x0A, &t));
    CHECK_INT((long long)t.image_id, (long long)a.shown);
    CHECK_INT((long long)t.shown_content_id, (long long)a.shown);
    CHECK(!esl_tag_pending(&t));

    /* new content: drawn on the host, sent to the cache, offered at the next POLL */
    esl_content_t bread = price("Brod", "39,90");
    uint32_t old = a.shown;
    CHECK_INT(esl_store_set(&st, 0x0A, &bread, &r), ESL_STORE_OK);
    host_push(0x0A, 70000, 0);
    CHECK_INT(round_rc(&a, 126000), ESL_MSG_IMG);
    CHECK(a.shown != old && a.img_shown == 2);

    /* an image the cache does not have (the host has not sent it yet): wait, and want it */
    esl_rtag_state_t s = rtags[0].s;
    s.image_id = 0x12345678;
    CHECK(esl_rcache_set_tag(&rc, &s));
    CHECK_INT(round_rc(&a, 186000), ESL_MSG_IDLE);
    CHECK_INT(a.next_poll_s, ESL_RCACHE_WAIT_S);
    CHECK(rc.want && rc.want_image == 0x12345678);
}

static void test_rcache_refused(void)
{
    esl_set_result_t r;
    esl_faketag_t a;
    esl_content_t milk = price("Melk", "24,90");
    boot();
    esl_faketag_init(&a, 0x0B, 1, 3000, ESL_MODEL_29);
    a.img_bad = true; /* every image "fails to draw" */
    round_rc(&a, 0);
    CHECK_INT(esl_store_set(&st, 0x0B, &milk, &r), ESL_STORE_OK);
    host_sync(100, 0);

    /* it takes the image, fails, and reports it: not offered again, as on the AP */
    CHECK_INT(round_rc(&a, 5000), ESL_MSG_IMG);
    uint32_t failed = a.img_failed;
    CHECK(failed != 0 && a.shown == 0);
    CHECK_INT(round_rc(&a, 65000), ESL_MSG_IDLE);
    CHECK_INT(round_rc(&a, 125000), ESL_MSG_IDLE);
    CHECK_INT((long long)a.img_failed, (long long)failed);

    /* the host, replaying the log, notes the refusal too */
    host_sync(125100, 0);
    esl_tag_t t;
    CHECK(esl_store_get(&st, 0x0B, &t) && t.image_failed == t.image_id && t.image_id != 0);
}

static void test_rcache_config(void)
{
    esl_faketag_t a;
    esl_cfg_result_t cr;
    boot();
    esl_faketag_init(&a, 0x0C, 1, 3000, ESL_MODEL_29);
    a.images = false;
    round_rc(&a, 0);
    host_sync(100, 0);
    CHECK_INT(round_rc(&a, 5000), ESL_MSG_IDLE);

    /* new settings on the host: CONFIG from the cache, then IDLE once the tag reports them */
    esl_tag_cfg_t cfg = {300, 0, 0};
    CHECK_INT(esl_store_set_cfg(&st, 0x0C, &cfg, &cr), ESL_STORE_OK);
    host_push(0x0C, 6000, 0);
    CHECK_INT(round_rc(&a, 10000), ESL_MSG_CONFIG);
    CHECK_INT(a.cfg.poll_s, 300);
    CHECK_INT(round_rc(&a, 310000), ESL_MSG_IDLE);
    host_sync(310100, 0);
    esl_tag_t t;
    CHECK(esl_store_get(&st, 0x0C, &t) && !esl_tag_cfg_pending(&t, esl_store_get_next_poll(&st)));

    /* a tag with old firmware (no cfg_ver) never gets CONFIG */
    esl_faketag_t b;
    esl_faketag_init(&b, 0x0D, 1, 3000, ESL_MODEL_29);
    b.legacy = true;
    b.images = false;
    round_rc(&b, 0);
    host_sync(100, 0);
    CHECK_INT(esl_store_set_cfg(&st, 0x0D, &cfg, &cr), ESL_STORE_OK);
    host_push(0x0D, 200, 0);
    CHECK_INT(round_rc(&b, 5000), ESL_MSG_IDLE);
}

static void test_rcache_fw(void)
{
    esl_faketag_t a;
    boot();
    for (size_t i = 0; i < sizeof(fw_bytes); i++)
        fw_bytes[i] = (uint8_t)(i * 7 + 3);
    uint32_t crc = esl_crc32(fw_bytes, sizeof(fw_bytes));
    esl_faketag_init(&a, 0x0E, 1, 3000, ESL_MODEL_29);
    a.ota = true;
    a.images = false;
    round_rc(&a, 0);
    host_sync(100, 2);

    /* version 2 to offer, but the cache has no firmware for the model yet: wait, and want it */
    CHECK_INT(round_rc(&a, 5000), ESL_MSG_IDLE);
    CHECK_INT(a.next_poll_s, ESL_RCACHE_WAIT_S);
    CHECK(rc.want && rc.want_fw_model == ESL_MODEL_29);

    /* with it the tag downloads the whole image from the cache and runs version 2 */
    esl_rcache_set_fw(&rc, ESL_MODEL_29, 2, sizeof(fw_bytes), crc);
    CHECK_INT(round_rc(&a, 10000), ESL_MSG_FW);
    CHECK_INT(a.fw, 2);
    CHECK_INT(a.fw_installs, 1);
    CHECK_INT(round_rc(&a, 70000), ESL_MSG_IDLE);

    /* a version it rolled back is not offered again */
    esl_faketag_t b;
    esl_faketag_init(&b, 0x0F, 1, 3000, ESL_MODEL_29);
    b.ota = true;
    b.images = false;
    b.fw_bad = true;
    round_rc(&b, 0);
    host_sync(100, 2);
    CHECK_INT(round_rc(&b, 5000), ESL_MSG_FW);
    CHECK_INT(b.fw, 1);
    CHECK_INT(round_rc(&b, 65000), ESL_MSG_IDLE);
    CHECK_INT(b.fw_installs, 0);
}

static void test_rcache_bits(void)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX], payload[ESL_POLL_LEN_CFG];
    esl_poll_t p = {1, 3000, 0, 0, ESL_MODEL_29, true, 0};
    boot();

    /* frames to another PAN or address: no reply, not in the log */
    size_t n = esl_frame_build_from_tag(rx, 1, PAN + 1, ESL_AP_SHORT_ADDR, 0x11, payload, esl_poll_build(payload, &p));
    CHECK_INT(esl_rcache_handle(&rc, rx, n, -60, 0, tx), 0);
    n = esl_frame_build_from_tag(rx, 1, PAN, 0x1234, 0x11, payload, esl_poll_build(payload, &p));
    CHECK_INT(esl_rcache_handle(&rc, rx, n, -60, 0, tx), 0);
    CHECK_INT(esl_rcache_log_count(&rc), 0);

    /* the log keeps the newest when the host is far behind */
    for (unsigned i = 0; i < LOG + 5; i++) {
        n = esl_frame_build_from_tag(rx, (uint8_t)i, PAN, ESL_AP_SHORT_ADDR, 0x100 + i, payload,
                                     esl_poll_build(payload, &p));
        CHECK(esl_rcache_handle(&rc, rx, n, -60, i, tx) > 0);
    }
    CHECK_INT(esl_rcache_log_count(&rc), LOG);
    CHECK_INT(rc.stats.log_dropped, 5);
    CHECK_INT(esl_rcache_log_at(&rc, 0)->t_ms, 5);
    esl_rcache_log_drop(&rc, 10);
    CHECK_INT(esl_rcache_log_count(&rc), LOG - 10);
    CHECK_INT(esl_rcache_log_at(&rc, 0)->t_ms, 15);
    CHECK(esl_rcache_log_at(&rc, LOG - 10) == NULL);

    /* when to sync: not while a tag fetches blocks, every ESL_RCACHE_SYNC_MS otherwise,
       and at ESL_RCACHE_SYNC_MAX_MS whatever the tags do */
    esl_rcache_want_clear(&rc);
    rc.last_rx_ms = 100000;
    rc.mid_blocks = true;
    CHECK(!esl_rcache_sync_due(&rc, 100000 + ESL_RCACHE_QUIET_MS, 80000));
    CHECK(esl_rcache_sync_due(&rc, 100000 + ESL_RCACHE_BLOCKS_MS, 80000));
    rc.mid_blocks = false;
    CHECK(esl_rcache_sync_due(&rc, 100000 + ESL_RCACHE_QUIET_MS, 80000));
    CHECK(!esl_rcache_sync_due(&rc, 100000 + ESL_RCACHE_QUIET_MS, 95000));
    CHECK(esl_rcache_sync_due(&rc, 100010, 100010 - ESL_RCACHE_SYNC_MAX_MS));

    /* images: a full cache replaces one that no tag needs before one that a tag needs */
    uint8_t img[100] = {1};
    CHECK(esl_rcache_put_image(&rc, 1, ESL_MODEL_29, 0, img, sizeof(img)));
    CHECK(esl_rcache_put_image(&rc, 2, ESL_MODEL_29, 0, img, sizeof(img)));
    CHECK(esl_rcache_put_image(&rc, 3, ESL_MODEL_29, 0, img, sizeof(img)));
    esl_rtag_state_t s = {0x20, 60, false, {0}, 1, 0};
    CHECK(esl_rcache_set_tag(&rc, &s));
    CHECK(esl_rcache_put_image(&rc, 4, ESL_MODEL_29, 0, img, sizeof(img)));
    CHECK(esl_rcache_has_image(&rc, 1) && !esl_rcache_has_image(&rc, 2) && esl_rcache_has_image(&rc, 4));
    CHECK(!esl_rcache_put_image(&rc, 5, ESL_MODEL_29, 0, img, rc.image_cap + 1));
    CHECK(!esl_rcache_put_image(&rc, 0, ESL_MODEL_29, 0, img, sizeof(img)));

    /* tags: replaced in place, dropped, and a full table says so */
    esl_rcache_drop_tag(&rc, 0x20);
    for (unsigned i = 0; i < TAGS; i++) {
        s.mac = 0x30 + i;
        CHECK(esl_rcache_set_tag(&rc, &s));
    }
    s.mac = 0x99;
    CHECK(!esl_rcache_set_tag(&rc, &s));
    esl_rcache_drop_tag(&rc, 0x30);
    CHECK(esl_rcache_set_tag(&rc, &s));
    s.next_poll_s = 120;
    CHECK(esl_rcache_set_tag(&rc, &s));
    CHECK_INT(rc.n_tags, TAGS);
}

void test_rcache(void)
{
    test_rcache_image();
    test_rcache_refused();
    test_rcache_config();
    test_rcache_fw();
    test_rcache_bits();
}
