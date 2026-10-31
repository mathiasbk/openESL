#include "tinytest.h"
#include "esl_crc32.h"
#include "esl_image.h"
#include "esl_radio.h"
#include "esl_rcache.h"
#include "esl_clock.h"
#include "esl_rhost.h"
#include "esl_render.h"
#include <stdlib.h>
/* tag/src/config.h: CFG_SCAN_CHANNELS (it wants a version from tools/build.py) */
#define FW_VERSION 1
#define FW_VERSION_STR "0"
#include "config.h"

/* A network radio end to end (docs/ap-netradio.md): the host (store, images, tag
 * firmware and esl_rhost, as on the S3) and the radio (esl_rcache, as on the C6), with
 * fake tags that talk to the radio only. A sync is what the link will carry: the
 * radio's log and what it holds one way, esl_rhost_answer() the other. */

#define PAN 0x4553
#define TAGS 16
#define IMAGES 16
#define LOG 128
#define RADIO_CLOCK 777777u /* the radio's clock runs this far ahead of the host's */

static esl_store_t st;
static esl_image_t images;
static esl_tagfw_t fw;
static esl_rhost_t host;
static esl_rtag_state_t sent[TAGS];
static esl_rcache_t rc;
static esl_rtag_t rtags[TAGS];
static esl_rimage_t rimages[IMAGES];
static esl_rlog_t rlog[LOG];

/* the tag firmware store on the host, a slot per model in RAM */
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
/* The radio's copy of the firmware: here it reads the host's (the link will copy it to the C6's flash). */
static bool radio_read_fw(void *ctx, uint8_t model, size_t off, uint8_t *buf, size_t n)
{
    return fw_read(ctx, model, (uint32_t)(ESL_FWF_HDR_LEN + off), buf, n);
}

/* what goes from the host to the radio */
static bool out_tag(void *ctx, const esl_rtag_state_t *s)
{
    return esl_rcache_set_tag(ctx, s);
}
static void out_drop(void *ctx, uint64_t mac)
{
    esl_rcache_drop_tag(ctx, mac);
}
static bool out_image(void *ctx, uint32_t id, uint8_t model, uint8_t fmt, const uint8_t *data, size_t size)
{
    return esl_rcache_put_image(ctx, id, model, fmt, data, size);
}
static void out_fw(void *ctx, uint8_t model, const esl_tagfw_img_t *img)
{
    esl_rcache_set_fw(ctx, model, img->ver, img->size, img->crc);
}
static void out_default(void *ctx, uint16_t poll_s)
{
    esl_rcache_set_default_poll(ctx, poll_s);
}
static void out_clear(void *ctx)
{
    esl_rcache_clear_tags(ctx);
}
static bool lose_end; /* the link breaks before the end of the answer */
static void out_end(void *ctx, uint32_t seq, bool more)
{
    if (!lose_end)
        esl_rcache_sync_end(ctx, seq, more);
}
static const esl_rhost_out_t out = {&rc, out_clear, out_tag, out_drop, out_image, out_fw, out_default, out_end};

/* The radio starts, with n_images image slots and max_tags tags */
static void radio_start(unsigned n_images, unsigned max_tags)
{
    static uint8_t *idata;
    static uint32_t boots;
    static const esl_rcache_ops_t ops = {NULL, radio_read_fw};
    size_t cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    if (!idata)
        idata = malloc(IMAGES * cap);
    for (unsigned i = 0; i < IMAGES; i++)
        rimages[i].data = idata + i * cap;
    esl_rcache_init(&rc, PAN, rtags, max_tags, rimages, n_images, cap, rlog, LOG, &ops);
    rc.boot = 0x5EED0000 + ++boots;
}

static void radio_boot(void)
{
    radio_start(IMAGES, TAGS);
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
    radio_boot();
}

/* A sync at host time now_ms: the log replayed, the answer applied. */
static void sync_at(uint64_t now_ms)
{
    esl_rsync_t req;
    esl_rcache_sync_req(&rc, now_ms + RADIO_CLOCK, &req);
    esl_rhost_begin(&host, &req);
    unsigned drawn = images.drawn;
    for (unsigned i = 0; i < req.log_n; i++)
        esl_rhost_replay(&host, &req, esl_rcache_log_at(&rc, i), now_ms);
    CHECK_INT(images.drawn, drawn); /* the replay draws nothing */
    esl_rhost_answer(&host, &req, &out);
}

/* One wake-up of a fake tag at host time now_ms, against the radio */
static uint8_t wake(esl_faketag_t *t, uint64_t now_ms)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    uint64_t at = now_ms + RADIO_CLOCK;
    size_t n = esl_faketag_poll(t, PAN, rx);
    size_t m = esl_rcache_handle(&rc, rx, n, -61, at, tx);
    uint8_t type = m ? esl_faketag_receive(t, tx, m) : 0;
    for (int req = 0; (type == ESL_MSG_IMG || type == ESL_MSG_FW) && req < ESL_FAKETAG_FW_REQS; req++) {
        n = type == ESL_MSG_IMG ? esl_faketag_img_request(t, PAN, rx) : esl_faketag_fw_request(t, PAN, rx);
        if (!n || !(m = esl_rcache_handle(&rc, rx, n, -61, at, tx)) || esl_faketag_receive(t, tx, m) != type)
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

static void test_rhost_content(void)
{
    esl_faketag_t a;
    esl_tag_t t;
    boot();
    esl_faketag_init(&a, 0xA1, 1, 2900, ESL_MODEL_29);

    /* a new tag: the radio waits, the host learns of it from the log, at the right time */
    CHECK_INT(wake(&a, 1000), ESL_MSG_IDLE);
    CHECK_INT(a.interval_s, ESL_RCACHE_WAIT_S);
    sync_at(2000);
    CHECK(esl_store_get(&st, 0xA1, &t) && t.seen && t.battery_mv == 2900 && t.rssi == -61);
    CHECK_INT((long long)t.last_seen_ms, 1000);
    CHECK(esl_rcache_set_tag(&rc, &rtags[0].s) && rc.n_tags == 1);
    CHECK_INT(wake(&a, 6000), ESL_MSG_IDLE);
    CHECK_INT(a.interval_s, ESL_TAG_BUILTIN_POLL_S);

    /* content set on the host reaches the radio at the next sync, image and all, before
       the tag polls again: it gets the image at once */
    set_price(0xA1, "Melk", "24,90");
    sync_at(8000);
    CHECK_INT(host.stats.images, 1);
    CHECK_INT(wake(&a, 66000), ESL_MSG_IMG);
    CHECK(a.img_shown == 1 && a.shown != 0);
    CHECK_INT(rc.stats.waits, 1);
    CHECK_INT(wake(&a, 126000), ESL_MSG_IDLE); /* it reports the image it shows */
    sync_at(127000);
    CHECK(esl_store_get(&st, 0xA1, &t));
    CHECK_INT((long long)t.shown_content_id, (long long)a.shown);
    CHECK(!esl_tag_pending(&t));

    /* nothing changed: a sync sends nothing */
    unsigned tags = host.stats.tags, imgs = host.stats.images;
    sync_at(137000);
    CHECK_INT(host.stats.tags, tags);
    CHECK_INT(host.stats.images, imgs);

    /* settings and the default interval */
    esl_tag_cfg_t cfg = {300, 0, 0, 0, 0, 0};
    esl_cfg_result_t cr;
    CHECK_INT(esl_store_set_cfg(&st, 0xA1, &cfg, &cr), ESL_STORE_OK);
    esl_store_set_next_poll(&st, 90);
    sync_at(140000);
    CHECK_INT(rc.default_poll_s, 90);
    CHECK_INT(wake(&a, 186000), ESL_MSG_CONFIG);
    CHECK_INT(a.interval_s, 300);
    sync_at(187000);

    /* deleted on the host: gone from the radio too (and back as a new tag when it polls) */
    CHECK(esl_store_delete(&st, 0xA1));
    sync_at(190000);
    CHECK_INT(rc.n_tags, 0);
    CHECK_INT(wake(&a, 490000), ESL_MSG_IDLE);
    CHECK_INT(a.interval_s, ESL_RCACHE_WAIT_S);
    sync_at(491000);
    CHECK(esl_store_get(&st, 0xA1, &t) && !t.has_content);
}

/* Content that reaches the store without PUT /api/tags (the schedule sets it with
   esl_store_set) and does not fit the layout file: the host checks it before it draws,
   so the radio never gets its image (#186) */
static void test_rhost_unfit(void)
{
    static esl_layouts_t ls;
    static esl_layout_bin_t bin;
    const char *f = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"c128\","
                    "\"fields\":[\"code\"],\"elements\":[{\"type\":\"barcode\",\"field\":\"code\","
                    "\"symbology\":\"code128\",\"x\":0,\"y\":0,\"w\":80,\"h\":40}]}]}";
    char err[ESL_LAYOUT_ERR_MAX];
    bool changed;
    esl_faketag_t a;
    esl_tag_t t;
    boot();
    memset(&ls, 0, sizeof(ls));
    CHECK(esl_layout_compile(f, strlen(f), ESL_MODEL_29, &bin, err, sizeof(err)));
    CHECK(esl_layouts_set(&ls, ESL_MODEL_29, f, strlen(f), &bin, &changed) != 0);
    host.ls = &ls;
    esl_faketag_init(&a, 0xA2, 1, 2900, ESL_MODEL_29);
    CHECK_INT(wake(&a, 1000), ESL_MSG_IDLE);
    sync_at(2000);

    esl_content_t c;
    esl_set_result_t r;
    CHECK(esl_content_init_named(&c, "c128", 4, 0) == ESL_CONTENT_OK);
    CHECK(esl_content_add_field(&c, (const uint8_t *)"WXYZ", 4) == ESL_CONTENT_OK);
    CHECK_INT(esl_store_set(&st, 0xA2, &c, &r), ESL_STORE_OK);
    unsigned imgs = host.stats.images;
    sync_at(8000);
    CHECK_INT(host.stats.images, imgs);
    CHECK(esl_store_get(&st, 0xA2, &t) && t.content_err == ESL_CERR_UNFIT && !t.content_check);
    CHECK_INT(wake(&a, 66000), ESL_MSG_IDLE);
    sync_at(67000);
    CHECK(esl_store_get(&st, 0xA2, &t) && t.content_err == ESL_CERR_UNFIT);
    CHECK_INT(host.stats.images, imgs);

    /* content that fits is drawn and sent */
    CHECK(esl_content_init_named(&c, "c128", 4, 0) == ESL_CONTENT_OK);
    CHECK(esl_content_add_field(&c, (const uint8_t *)"A", 1) == ESL_CONTENT_OK);
    CHECK_INT(esl_store_set(&st, 0xA2, &c, &r), ESL_STORE_OK);
    sync_at(70000);
    CHECK_INT(host.stats.images, imgs + 1);
    CHECK(esl_store_get(&st, 0xA2, &t) && t.content_err == ESL_CERR_NONE);
    host.ls = NULL;
    esl_layouts_free(&ls);
}

static void test_rhost_many(void)
{
    esl_faketag_t t[12];
    boot();
    for (unsigned i = 0; i < 12; i++) {
        esl_faketag_init(&t[i], 0xB00 + i, 1, 3000, ESL_MODEL_29);
        wake(&t[i], 1000 + i);
    }
    sync_at(2000);
    CHECK_INT(rc.n_tags, 12);

    /* every price at once: a sync draws and sends only so much, the rest follows */
    for (unsigned i = 0; i < 12; i++) {
        char p[8];
        snprintf(p, sizeof(p), "%u,90", i + 10);
        set_price(0xB00 + i, "Vare", p);
    }
    sync_at(3000);
    CHECK_INT(host.stats.drawn, ESL_RHOST_DRAWS);
    CHECK_INT(host.stats.images, ESL_RHOST_IMAGES);
    /* the radio is told there is more, and comes back soon rather than in 10 s */
    CHECK(rc.want);
    CHECK(esl_rcache_sync_due(&rc, 3000 + RADIO_CLOCK + ESL_RCACHE_SOON_MS, 3000 + RADIO_CLOCK));
    for (int s = 0; s < 4 && rc.want; s++)
        sync_at(4000 + s * 1000);
    CHECK(!rc.want);
    CHECK_INT(host.stats.images, 12);
    for (unsigned i = 0; i < 12; i++)
        CHECK_INT(wake(&t[i], 70000 + i), ESL_MSG_IMG);
    CHECK_INT(rc.stats.waits, 12); /* only their first wake-ups, as new tags */

    /* the radio restarts: it gets every tag again, and the images as they are asked for */
    radio_boot();
    sync_at(80000);
    CHECK_INT(rc.n_tags, 12);
    CHECK_INT(wake(&t[0], 130000), ESL_MSG_IDLE); /* shows its image already */
    CHECK_INT(t[0].interval_s, ESL_TAG_BUILTIN_POLL_S);
}

static void test_rhost_refused_fw(void)
{
    esl_faketag_t a;
    esl_tag_t t;
    char buf[256];
    esl_sb_t sb;
    boot();

    /* a refused image: noted on the host from the log, and kept from the radio after its restart */
    esl_faketag_init(&a, 0xC1, 1, 3000, ESL_MODEL_29);
    a.img_bad = true;
    wake(&a, 1000);
    set_price(0xC1, "Brod", "39,90");
    sync_at(2000);
    CHECK_INT(wake(&a, 62000), ESL_MSG_IMG);
    CHECK(a.img_failed != 0);
    CHECK_INT(wake(&a, 122000), ESL_MSG_IDLE);
    sync_at(123000);
    CHECK(esl_store_get(&st, 0xC1, &t) && t.image_failed == a.img_failed);
    radio_boot();
    sync_at(124000);
    CHECK_INT(wake(&a, 182000), ESL_MSG_IDLE);

    /* tag firmware: uploaded to the host, sent to the radio for the tags that are to get it */
    static uint8_t file[ESL_FWF_HDR_LEN + 2048];
    uint8_t *app = file + ESL_FWF_HDR_LEN;
    for (size_t i = 0; i < 2048; i++)
        app[i] = (uint8_t)(i * 13 + 5);
    uint32_t crc = esl_crc32(app, 2048), sz = 2048, magic = ESL_FWF_MAGIC;
    memset(file, 0, ESL_FWF_HDR_LEN);
    memcpy(file + ESL_FWF_OFF_MAGIC, &magic, 4);
    file[ESL_FWF_OFF_FORMAT] = ESL_FWF_FORMAT;
    file[ESL_FWF_OFF_MODEL] = ESL_MODEL_29;
    file[ESL_FWF_OFF_VER] = 2;
    memcpy(file + ESL_FWF_OFF_SIZE, &sz, 4);
    memcpy(file + ESL_FWF_OFF_CRC, &crc, 4);
    esl_sb_init(&sb, buf, sizeof(buf));
    CHECK_INT(esl_tagfw_part(&fw, 0, sizeof(file), file, sizeof(file), 0, &sb), 200);

    esl_faketag_t b;
    esl_faketag_init(&b, 0xC2, 1, 3000, ESL_MODEL_29);
    b.ota = true;
    b.images = false;
    wake(&b, 200000);
    sync_at(201000);
    CHECK_INT(rc.fw[ESL_MODEL_29 - 1].ver, 2);
    CHECK_INT(wake(&b, 261000), ESL_MSG_FW);
    CHECK_INT(b.fw, 2);
    CHECK_INT(wake(&b, 321000), ESL_MSG_IDLE); /* it reports the version it runs */
    sync_at(322000);
    esl_fw_counts_t c;
    esl_store_fw_counts(&st, ESL_MODEL_29, 2, &c);
    CHECK_INT(c.updated, 1);

    /* deleted on the host: the radio drops it too */
    CHECK(esl_tagfw_delete(&fw, ESL_MODEL_29));
    sync_at(323000);
    CHECK_INT(rc.fw[ESL_MODEL_29 - 1].ver, 0);
}

/* More image tags than the radio has image slots, the way they would run: each tag
   wakes at its interval, the radio syncs when esl_rcache_sync_due() says. Every tag
   gets its image, the images sent stay near one per tag, and once all show theirs the
   radio stops asking. */
static void test_rhost_slots(void)
{
    enum { N = 12 };
    esl_faketag_t t[N];
    uint64_t next[N], last_sync = 0;
    boot();
    radio_start(4, TAGS);
    for (unsigned i = 0; i < N; i++) {
        esl_faketag_init(&t[i], 0xD00 + i, 1, 3000, ESL_MODEL_29);
        next[i] = 1000 + i * 4700; /* spread over the minute */
    }
    bool priced = false;
    unsigned syncs_late = 0, images_late = 0;
    for (uint64_t now = 0; now < 600000; now += 50) {
        for (unsigned i = 0; i < N; i++)
            if (now >= next[i]) {
                wake(&t[i], now);
                next[i] = now + t[i].interval_s * 1000u;
            }
        if (now == 120000 && !priced) {
            for (unsigned i = 0; i < N; i++) {
                char p[8];
                snprintf(p, sizeof(p), "%u,50", i + 20);
                set_price(0xD00 + i, "Vare", p);
            }
            priced = true;
        }
        if (esl_rcache_sync_due(&rc, now + RADIO_CLOCK, last_sync + RADIO_CLOCK)) {
            unsigned before = host.stats.images;
            sync_at(now);
            last_sync = now;
            if (now > 400000) {
                syncs_late++;
                images_late += host.stats.images - before;
            }
        }
    }
    for (unsigned i = 0; i < N; i++)
        CHECK_INT(t[i].img_shown, 1);
    CHECK(host.stats.images <= N + ESL_RHOST_IMAGES);
    CHECK(syncs_late > 0 && syncs_late <= 25); /* every 10 s, not every second */
    CHECK_INT(images_late, 0);
    CHECK(!rc.want);
}

/* What the link can do: an answer lost on the way, so the log comes again; a radio
   with no room for a tag; a host that restarted and forgot what it told the radio. */
static void test_rhost_link(void)
{
    esl_faketag_t a, b;
    esl_tag_t t;
    boot();
    esl_faketag_init(&a, 0xE1, 1, 3000, ESL_MODEL_29);
    wake(&a, 1000);
    sync_at(2000);
    wake(&a, 62000);
    CHECK(esl_store_get(&st, 0xE1, &t));
    unsigned polls = t.polls;

    /* the answer is lost: the radio keeps its log and sends it again; the host replays
       nothing twice, and tells the radio everything again */
    set_price(0xE1, "Ost", "89,00");
    lose_end = true;
    sync_at(63000);
    lose_end = false;
    CHECK(esl_store_get(&st, 0xE1, &t) && t.polls == polls + 1);
    CHECK_INT(esl_rcache_log_count(&rc), 1);
    unsigned resets = host.stats.resets;
    sync_at(64000);
    CHECK_INT(host.stats.skipped, 1);
    CHECK_INT(host.stats.resets, resets + 1);
    CHECK(esl_store_get(&st, 0xE1, &t) && t.polls == polls + 1);
    CHECK_INT(esl_rcache_log_count(&rc), 0);
    CHECK_INT(wake(&a, 122000), ESL_MSG_IMG);

    /* a radio with room for one tag: the second is refused, and sent once there is room */
    radio_start(IMAGES, 1);
    esl_faketag_init(&b, 0xE2, 1, 3000, ESL_MODEL_29);
    wake(&b, 130000);
    sync_at(131000);
    CHECK_INT(rc.n_tags, 1);
    CHECK(host.stats.refused > 0);
    CHECK_INT(wake(&b, 140000), ESL_MSG_IDLE); /* no room: the default interval, no waits */
    CHECK_INT(b.interval_s, ESL_RCACHE_DEFAULT_POLL_S);
    CHECK(esl_store_delete(&st, 0xE1));
    sync_at(141000);
    CHECK_INT(rc.n_tags, 1);
    CHECK_INT((long long)rtags[0].s.mac, 0xE2);

    /* the host restarts (its table survives in NVS on the S3; here the store stays) and
       a tag was deleted meanwhile: the radio forgets its tags and gets the rest again */
    radio_start(IMAGES, TAGS);
    sync_at(150000);
    wake(&a, 151000);
    sync_at(152000);
    CHECK_INT(rc.n_tags, 2);
    static uint8_t img[8192];
    esl_rhost_init(&host, &st, NULL, &images, &fw, PAN, sent, TAGS, img, esl_image_mem_size() / ESL_IMAGE_BUFS);
    CHECK(esl_store_delete(&st, 0xE2));
    sync_at(160000);
    CHECK_INT(rc.n_tags, 1);
    CHECK_INT((long long)rtags[0].s.mac, 0xE1);
}

/* Date fields while the time is unknown: no draws are spent on those tags, so the others
   still get theirs, and they follow once it is known (#183) */
static void test_rhost_clock_unknown(void)
{
    static esl_layouts_t ls;
    static esl_layout_bin_t bin;
    const char *f = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":["
"{\"name\":\"day\",\"fields\":[\"n\"],\"elements\":[{\"type\":\"text\",\"field\":\"n\",\"x\":0,\"y\":0,\"w\":200,\"h\":30,\"fonts\":[\"medium\"]},"
"{\"type\":\"text\",\"text\":\"{date}\",\"x\":0,\"y\":50,\"w\":200,\"h\":30,\"fonts\":[\"medium\"]}]},"
"{\"name\":\"plain\",\"fields\":[\"n\"],\"elements\":[{\"type\":\"text\",\"field\":\"n\",\"x\":0,\"y\":0,\"w\":200,\"h\":30,\"fonts\":[\"medium\"]}]}]}";
    char err[ESL_LAYOUT_ERR_MAX];
    bool changed;
    esl_faketag_t t[12];
    boot();
    esl_clock_set_today(-1);
    memset(&ls, 0, sizeof(ls));
    CHECK(esl_layout_compile(f, strlen(f), ESL_MODEL_29, &bin, err, sizeof(err)));
    CHECK(esl_layouts_set(&ls, ESL_MODEL_29, f, strlen(f), &bin, &changed) != 0);
    host.ls = &ls;
    for (unsigned i = 0; i < 12; i++) {
        esl_faketag_init(&t[i], 0xC00 + i, 1, 3000, ESL_MODEL_29);
        wake(&t[i], 1000 + i);
    }
    sync_at(2000);
    for (unsigned i = 0; i < 12; i++) {
        esl_content_t c;
        esl_set_result_t r;
        const char *n = i < 10 ? "day" : "plain";
        CHECK(esl_content_init_named(&c, n, strlen(n), 0) == ESL_CONTENT_OK);
        CHECK(esl_content_add_field(&c, (const uint8_t *)"X", 1) == ESL_CONTENT_OK);
        CHECK_INT(esl_store_set(&st, 0xC00 + i, &c, &r), ESL_STORE_OK);
    }
    for (int s = 0; s < 6; s++)
        sync_at(3000 + s * 1000);
    CHECK_INT(host.stats.images, 1); /* the plain tags (one image, the same content) */
    CHECK(!rc.want);                 /* no draws spent on the date tags */
    esl_clock_set_today(20755);
    for (int s = 0; s < 6; s++)
        sync_at(10000 + s * 1000);
    CHECK_INT(host.stats.images, 2); /* and the date tags once the time is known */
    esl_clock_set_today(-1);
    host.ls = NULL;
    esl_layouts_free(&ls);
}

/* ---- several radios (docs/ap-netradio.md, "Several radios") ---- */

static esl_rhost_t host2;
static esl_rtag_state_t sent2[TAGS];
static esl_rcache_t rc2;
static esl_rtag_t rtags2[TAGS];
static esl_rimage_t rimages2[IMAGES];
static esl_rlog_t rlog2[LOG];
static esl_rowners_t owners;
static esl_rowner_t owner_slots[TAGS];
static const esl_rhost_out_t out2 = {&rc2, out_clear, out_tag, out_drop, out_image, out_fw, out_default, out_end};

static const esl_rtag_state_t *rfind(const esl_rcache_t *c, uint64_t mac)
{
    for (unsigned i = 0; i < c->n_tags; i++)
        if (c->tags[i].s.mac == mac)
            return &c->tags[i].s;
    return NULL;
}

/* A sync of radio 2 at host time now_ms */
static void sync2_at(uint64_t now_ms)
{
    esl_rsync_t req;
    esl_rcache_sync_req(&rc2, now_ms + RADIO_CLOCK, &req);
    esl_rhost_begin(&host2, &req);
    for (unsigned i = 0; i < req.log_n; i++)
        esl_rhost_replay(&host2, &req, esl_rcache_log_at(&rc2, i), now_ms);
    esl_rhost_answer(&host2, &req, &out2);
}

/* One wake-up of a fake tag against radio 2 */
static uint8_t wake2(esl_faketag_t *t, uint64_t now_ms)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    uint64_t at = now_ms + RADIO_CLOCK;
    size_t n = esl_faketag_poll(t, PAN, rx);
    size_t m = esl_rcache_handle(&rc2, rx, n, -70, at, tx);
    uint8_t type = m ? esl_faketag_receive(t, tx, m) : 0;
    for (int req = 0; type == ESL_MSG_IMG && req < ESL_FAKETAG_IMG_REQS; req++) {
        n = esl_faketag_img_request(t, PAN, rx);
        if (!n || !(m = esl_rcache_handle(&rc2, rx, n, -70, at, tx)) || esl_faketag_receive(t, tx, m) != type)
            break;
    }
    return type;
}

static void boot_two(void)
{
    static uint8_t *idata, *img;
    static const esl_rcache_ops_t ops = {NULL, radio_read_fw};
    size_t cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    boot();
    if (!idata)
        idata = malloc(IMAGES * cap);
    if (!img)
        img = malloc(cap);
    for (unsigned i = 0; i < IMAGES; i++)
        rimages2[i].data = idata + i * cap;
    esl_rcache_init(&rc2, PAN, rtags2, TAGS, rimages2, IMAGES, cap, rlog2, LOG, &ops);
    rc2.boot = 0x2222;
    esl_rhost_init(&host2, &st, NULL, &images, &fw, PAN, sent2, TAGS, img, cap);
    esl_rowners_init(&owners, owner_slots, TAGS);
    owners.radios = 2;
    esl_rhost_share(&host, &owners, 0);
    esl_rhost_share(&host2, &owners, 1);
    esl_store_set_radio_channel(&st, 1, 20);
}

static void test_rhost_two_radios(void)
{
    esl_faketag_t a, b, c;
    esl_tag_t t;
    esl_channel_state_t cs;
    boot_two();
    esl_faketag_init(&a, 0xA1, 1, 2900, ESL_MODEL_29);
    esl_faketag_init(&b, 0xB1, 1, 2900, ESL_MODEL_29);

    /* A polls radio 1 (the AP's channel), B radio 2: each radio gets only its own tag */
    CHECK_INT(wake(&a, 1000), ESL_MSG_IDLE);
    CHECK_INT(wake2(&b, 1000), ESL_MSG_IDLE);
    sync_at(2000);
    sync2_at(2000);
    sync_at(3000); /* B is in the store now, and not radio 1's */
    CHECK_INT(esl_rowners_get(&owners, &st, 0xA1), 0);
    CHECK_INT(esl_rowners_get(&owners, &st, 0xB1), 1);
    CHECK_INT(esl_rowners_get(&owners, &st, 0xC1), ESL_RHOST_NONE);
    CHECK_INT(esl_rowners_count(&owners, 1), 1);
    CHECK(rfind(&rc, 0xA1) && !rfind(&rc, 0xB1));
    CHECK(rfind(&rc2, 0xB1) && !rfind(&rc2, 0xA1));
    CHECK(esl_store_get(&st, 0xB1, &t) && t.radio == 1);

    /* B is told radio 2's channel in CONFIG, A the AP's (0, the built-in one) */
    CHECK_INT(rfind(&rc2, 0xB1)->cfg.channel, 20);
    CHECK_INT(rfind(&rc, 0xA1)->cfg.channel, 0);
    CHECK_INT(wake2(&b, 61000), ESL_MSG_CONFIG);
    CHECK_INT(b.cfg.channel, 20);
    CHECK_INT(wake2(&b, 121000), ESL_MSG_IDLE);
    sync2_at(122000);
    CHECK(esl_store_get(&st, 0xB1, &t));
    {
        esl_tag_cfg_t ap;
        esl_store_tag_defaults(&st, &ap);
        ap = esl_store_tag_ap(&st, &t, &ap);
        CHECK_INT(ap.channel, 20);
        CHECK(!esl_tag_cfg_pending(&t, &ap));
    }
    CHECK(!esl_tag_cfg_pending(&t, &(esl_tag_cfg_t){0, 0, 0, 0, 0, 20}));

    /* the AP's channel cannot move onto radio 2's */
    esl_store_channel_state(&st, 122000, &cs);
    CHECK_INT((long long)cs.taken, 1 << 20);

    /* content: each image goes to the radio of its tag only */
    unsigned i1 = host.stats.images, i2 = host2.stats.images;
    set_price(0xA1, "Melk", "24,90");
    set_price(0xB1, "Brød", "39,90");
    sync_at(130000);
    sync2_at(130000);
    CHECK_INT(host.stats.images, i1 + 1);
    CHECK_INT(host2.stats.images, i2 + 1);
    CHECK_INT(wake(&a, 190000), ESL_MSG_IMG);
    CHECK_INT(wake2(&b, 190000), ESL_MSG_IMG);
    CHECK(a.img_shown == 1 && b.img_shown == 1);

    /* radio 1 goes quiet; A finds radio 2 (channel scan) and moves there: radio 2 asks for it,
       owns it from then on and tells it its channel; radio 1 drops it at its next sync */
    CHECK_INT(wake2(&a, 300000), ESL_MSG_IDLE);
    CHECK_INT(a.interval_s, ESL_RCACHE_WAIT_S);
    sync2_at(301000);
    CHECK_INT(esl_rowners_get(&owners, &st, 0xA1), 1);
    CHECK(rfind(&rc2, 0xA1) && rfind(&rc2, 0xA1)->cfg.channel == 20);
    CHECK_INT(rfind(&rc2, 0xA1)->image_id, a.shown); /* it shows its image: nothing to send */
    CHECK_INT(wake2(&a, 306000), ESL_MSG_CONFIG);
    CHECK_INT(a.cfg.channel, 20);
    sync_at(307000);
    CHECK(!rfind(&rc, 0xA1));
    CHECK_INT(esl_rowners_count(&owners, 0), 0);

    /* a log that comes late from radio 1 (a frame from before the move) does not take it back */
    CHECK_INT(wake(&a, 299000), ESL_MSG_IDLE);
    sync_at(308000);
    CHECK_INT(esl_rowners_get(&owners, &st, 0xA1), 1);

    /* a new price for A now goes through radio 2 */
    i1 = host.stats.images;
    i2 = host2.stats.images;
    set_price(0xA1, "Melk", "19,90");
    sync_at(310000);
    sync2_at(310000);
    CHECK_INT(host.stats.images, i1);
    CHECK_INT(host2.stats.images, i2 + 1);
    CHECK_INT(wake2(&a, 366000), ESL_MSG_IMG);
    CHECK_INT(a.img_shown, 2);

    /* a tag no radio has heard since the host started (here: heard, then forgotten as by a
       restart): both radios get its state, but its image goes ahead to neither while two
       are connected */
    esl_faketag_init(&c, 0xC1, 1, 2900, ESL_MODEL_29);
    CHECK_INT(wake(&c, 390000), ESL_MSG_IDLE);
    sync_at(391000);
    memset(&owner_slots[esl_store_slot_of(&st, 0xC1)], 0, sizeof(owner_slots[0]));
    set_price(0xC1, "Ost", "89,90");
    i1 = host.stats.images;
    i2 = host2.stats.images;
    sync_at(400000);
    sync2_at(400000);
    CHECK(rfind(&rc, 0xC1) && rfind(&rc2, 0xC1));
    CHECK_INT(host.stats.images, i1);
    CHECK_INT(host2.stats.images, i2);
    /* with one radio left, it goes ahead as before */
    owners.radios = 1;
    sync2_at(401000);
    CHECK_INT(host2.stats.images, i2 + 1);
    owners.radios = 2;

    /* radio 2 forgotten: its tags count as radio 1's again, and the channel is free */
    esl_rowners_forget(&owners, 1);
    esl_store_set_radio_channel(&st, 1, 0);
    CHECK(esl_store_get(&st, 0xA1, &t) && t.radio == 0);
    CHECK_INT(esl_rowners_get(&owners, &st, 0xA1), ESL_RHOST_NONE);
    esl_store_channel_state(&st, 402000, &cs);
    CHECK_INT((long long)cs.taken, 0);
    esl_rhost_share(&host, NULL, 0);
}

/* A new radio's channel: the first free one in the order a tag that lost its radio looks */
static void test_rhost_pick_channel(void)
{
    static const uint8_t tag_order[] = {CFG_SCAN_CHANNELS};
    uint32_t taken = 0;
    for (unsigned i = 0; i < sizeof(tag_order); i++) {
        uint8_t c = esl_rhost_pick_channel(taken);
        CHECK_INT(c, tag_order[i]);
        taken |= 1u << c;
    }
    CHECK_INT(esl_rhost_pick_channel(taken), 0);
    CHECK_INT(esl_rhost_pick_channel(1u << 25 | 1u << 15), 20); /* the AP on 25, a radio on 15 */
}

void test_rhost(void)
{
    test_rhost_pick_channel();
    test_rhost_clock_unknown();
    test_rhost_content();
    test_rhost_unfit();
    test_rhost_many();
    test_rhost_refused_fw();
    test_rhost_slots();
    test_rhost_link();
    test_rhost_two_radios();
}
