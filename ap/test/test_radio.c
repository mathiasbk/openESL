#include "tinytest.h"
#include "esl_api.h"
#include "esl_clock.h"
#include "esl_persist.h"
#include "esl_image.h"
#include "esl_img2d.h"
#include "esl_link.h"
#include "esl_radio.h"
#include "esl_crc32.h"
#include "esl_render.h"
#include "esl_png.h"
#include "png_fixtures.h"
#include "cmac.h"
#include <stdlib.h>

/* End to end without hardware: client -> API -> store -> radio frames -> fake
 * tag, and back. Follows the test matrix in docs/fremdriftsplan.md phase 5. */

#define CAP 24 /* krav G3: at least 20 tags */
#define PAN 0x4553
#define AUTH "Bearer t"

/* Fake NVS: one blob per slot, in the real persist format. */
typedef struct {
    bool used[CAP];
    uint8_t blob[CAP][ESL_PERSIST_BLOB_MAX];
    size_t len[CAP];
    int writes;
    bool cfg_used[CAP];
    uint8_t cfg[CAP][ESL_PERSIST_CFG_LEN];
    bool has_next_poll;
    uint16_t next_poll_s;
    uint8_t img[CAP][ESL_IMAGE_STORED_MAX(5000)]; /* pictures (ops.persist_image) */
    size_t img_len[CAP];
    int img_writes;
} nvs_t;

static bool nvs_save(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    nvs_t *n = ctx;
    n->len[slot] = esl_persist_encode(mac, c, n->blob[slot]);
    n->used[slot] = true;
    n->writes++;
    return true;
}

static bool nvs_load(void *ctx, unsigned slot, uint64_t mac, esl_content_t *c)
{
    nvs_t *n = ctx;
    uint64_t m;
    return n->used[slot] && esl_persist_decode(n->blob[slot], n->len[slot], &m, c) && m == mac;
}

static void nvs_erase(void *ctx, unsigned slot)
{
    nvs_t *n = ctx;
    n->used[slot] = false;
}

static void nvs_cfg(void *ctx, unsigned slot, uint64_t mac, const esl_tag_cfg_t *cfg)
{
    nvs_t *n = ctx;
    n->cfg_used[slot] = cfg != NULL;
    if (cfg)
        esl_persist_cfg_encode(mac, cfg, n->cfg[slot]);
}

static void nvs_next_poll(void *ctx, const uint16_t *next_poll_s)
{
    nvs_t *n = ctx;
    n->has_next_poll = next_poll_s != NULL;
    n->next_poll_s = next_poll_s ? *next_poll_s : 0;
}

static bool nvs_image(void *ctx, unsigned slot, const uint8_t *data, size_t len)
{
    nvs_t *n = ctx;
    if (len > sizeof(n->img[slot]))
        return false;
    if (data)
        memcpy(n->img[slot], data, len);
    n->img_len[slot] = data ? len : 0;
    n->img_writes++;
    return true;
}

static size_t nvs_load_image(void *ctx, unsigned slot, uint8_t *buf, size_t cap)
{
    nvs_t *n = ctx;
    if (!n->img_len[slot] || n->img_len[slot] > cap)
        return 0;
    memcpy(buf, n->img[slot], n->img_len[slot]);
    return n->img_len[slot];
}

static nvs_t nvs;
static esl_layouts_t layouts; /* empty unless a test uploads a file */
static esl_store_t st;
static esl_api_t api = {&st, "t", "0.1.0", 25, &layouts, NULL, NULL, NULL, NULL, NULL, NULL, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
static char resp[ESL_API_RESP_SIZE(CAP)];
static esl_sb_t out;

/* Tags that show what the AP draws (POLL bit5): the image is drawn when the tag polls
 * with another image than its content gives, sent block by block, and never stored.
 * docs/protokoll.md "Image transfer"; plan D in design-sending-alternativer. */
static esl_image_t images;

static void images_boot(void)
{
    static uint8_t *mem;
    if (!mem)
        mem = malloc(esl_image_mem_size());
    esl_image_init(&images, mem, NULL);
    esl_radio_set_image(&images);
}

/* AP boot: empty table, then everything from fake NVS; no image drawn yet. */
static void ap_boot(void)
{
    images_boot();
    static const esl_store_ops_t ops = {&nvs, NULL, NULL, nvs_save, nvs_erase, nvs_cfg, nvs_next_poll, NULL, NULL, nvs_load, NULL, NULL, NULL, nvs_image, nvs_load_image};
    esl_store_free(&st);
    esl_store_init(&st, CAP, &ops, 0);
    if (nvs.has_next_poll)
        esl_store_restore_next_poll(&st, nvs.next_poll_s);
    for (unsigned s = 0; s < CAP; s++) {
        uint64_t mac;
        esl_content_t c;
        esl_tag_cfg_t cfg;
        if (nvs.used[s] && esl_persist_decode(nvs.blob[s], nvs.len[s], &mac, &c))
            esl_store_restore(&st, s, mac, &c);
        if (nvs.cfg_used[s] && esl_persist_cfg_decode(nvs.cfg[s], ESL_PERSIST_CFG_LEN, &mac, &cfg))
            esl_store_restore_cfg(&st, s, mac, &cfg);
    }
}

/* One wake-up of the tag (POLL, then IMG_REQ or FW_REQ until done), or with lose_reply
 * only the POLL, whose reply is lost. Returns the reply type the tag accepted (0 = none). */
static uint8_t air(esl_faketag_t *t, uint64_t now_ms, bool lose_reply)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    if (!lose_reply)
        return esl_faketag_round(t, &st, &layouts, PAN, -60, now_ms);
    size_t n = esl_faketag_poll(t, PAN, rx);
    size_t m = esl_radio_handle(&st, &layouts, PAN, rx, n, -60, now_ms, tx, NULL);
    if (!m || lose_reply)
        return 0;
    return esl_faketag_receive(t, tx, m);
}

static uint32_t json_u32(const char *s, const char *key)
{
    const char *p = strstr(s, key);
    return p ? (uint32_t)strtoul(p + strlen(key), NULL, 10) : 0;
}

static int put(const char *mac, const char *body)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "/api/tags/%s", mac);
    return esl_api_handle(&api, "PUT", uri, AUTH, body, strlen(body), 0, &out);
}

static void test_handle_filters(void)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX], payload[ESL_POLL_LEN];
    esl_poll_t p = {1, 3000, 0, 0, ESL_MODEL_29, false, 0, false, 0, 0, 0, 0};
    esl_radio_event_t ev;
    size_t n;

    ap_boot();
    esl_poll_build(payload, &p);

    /* a POLL for us: IDLE echoing the sequence number, tag discovered */
    n = esl_frame_build_from_tag(rx, 42, PAN, ESL_AP_SHORT_ADDR, 0x1122, payload, sizeof(payload));
    size_t m = esl_radio_handle(&st, &layouts, PAN, rx, n, -70, 5000, tx, &ev);
    CHECK_INT(m, ESL_MHR_LEN + ESL_IDLE_LEN + ESL_FCS_LEN);
    CHECK(ev.mac == 0x1122);
    CHECK_INT(ev.reply_type, ESL_MSG_IDLE);
    CHECK_INT(ev.poll.model, ESL_MODEL_29);
    esl_ap_frame_t f;
    CHECK(esl_frame_parse_to_tag(tx, m, &f));
    CHECK_INT(f.seq, 42);
    CHECK_INT(f.dst_pan, PAN);
    CHECK(f.dst_mac == 0x1122);
    CHECK_INT(f.src_short, ESL_AP_SHORT_ADDR);
    CHECK_INT(esl_store_count(&st), 1);

    /* ignored: other PAN, other short address, not a POLL, broken POLL, AP->tag frame */
    n = esl_frame_build_from_tag(rx, 1, 0x1234, ESL_AP_SHORT_ADDR, 0x3344, payload, sizeof(payload));
    CHECK_INT(esl_radio_handle(&st, &layouts, PAN, rx, n, 0, 0, tx, NULL), 0);
    n = esl_frame_build_from_tag(rx, 1, PAN, 0x0001, 0x3344, payload, sizeof(payload));
    CHECK_INT(esl_radio_handle(&st, &layouts, PAN, rx, n, 0, 0, tx, NULL), 0);
    uint8_t idle[ESL_IDLE_LEN];
    esl_idle_build(idle, 0);
    n = esl_frame_build_from_tag(rx, 1, PAN, ESL_AP_SHORT_ADDR, 0x3344, idle, sizeof(idle));
    CHECK_INT(esl_radio_handle(&st, &layouts, PAN, rx, n, 0, 0, tx, NULL), 0);
    n = esl_frame_build_from_tag(rx, 1, PAN, ESL_AP_SHORT_ADDR, 0x3344, payload, ESL_POLL_LEN - 1);
    CHECK_INT(esl_radio_handle(&st, &layouts, PAN, rx, n, 0, 0, tx, NULL), 0);
    n = esl_frame_build_to_tag(rx, 1, PAN, ESL_AP_SHORT_ADDR, 0x3344, payload, sizeof(payload));
    CHECK_INT(esl_radio_handle(&st, &layouts, PAN, rx, n, 0, 0, tx, NULL), 0);
    CHECK_INT(esl_radio_handle(&st, &layouts, PAN, rx, 5, 0, 0, tx, NULL), 0);
    CHECK_INT(esl_store_count(&st), 1);
}

static void test_fake_tag_filters(void)
{
    uint8_t tx[ESL_PSDU_MAX], idle[ESL_IDLE_LEN];
    esl_faketag_t t;
    esl_faketag_init(&t, 0xAA, 1, 3000, ESL_MODEL_29);
    uint8_t rx[ESL_PSDU_MAX];
    esl_faketag_poll(&t, PAN, rx);
    esl_idle_build(idle, 0);

    /* another tag's reply, an old sequence number, garbage */
    size_t m = esl_frame_build_to_tag(tx, t.seq, PAN, ESL_AP_SHORT_ADDR, 0xBB, idle, sizeof(idle));
    CHECK_INT(esl_faketag_receive(&t, tx, m), 0);
    m = esl_frame_build_to_tag(tx, (uint8_t)(t.seq - 1), PAN, ESL_AP_SHORT_ADDR, 0xAA, idle, sizeof(idle));
    CHECK_INT(esl_faketag_receive(&t, tx, m), 0);
    m = esl_frame_build_to_tag(tx, t.seq, PAN, ESL_AP_SHORT_ADDR, 0xAA, idle, 3);
    CHECK_INT(esl_faketag_receive(&t, tx, m), 0);
    CHECK_INT(t.rejected, 3);
    CHECK_INT(t.flags, ESL_POLL_F_BOOTED); /* still waiting for a first answer */
    m = esl_frame_build_to_tag(tx, t.seq, PAN, ESL_AP_SHORT_ADDR, 0xAA, idle, sizeof(idle));
    CHECK_INT(esl_faketag_receive(&t, tx, m), ESL_MSG_IDLE);
    CHECK_INT(t.flags, 0);

    /* reply parser: the retired CONTENT, an IMG block past the end of its image, a long
       IDLE, another protocol version */
    esl_reply_t r;
    CHECK(!esl_reply_parse((const uint8_t *)"\x03\x01\x3C\x00\x01\x00\x00\x00\x02\x02\x02hi\x00", 14, &r));
    CHECK(!esl_reply_parse((const uint8_t *)"\x09\x01\x00\x00\x01\x00\x00\x00\x02\x00\x01\x00\x01\x00ab", 16, &r));
    CHECK(esl_reply_parse((const uint8_t *)"\x09\x01\x3C\x00\x01\x00\x00\x00\x03\x00\x01\x00\x01\x00ab", 16, &r));
    CHECK_INT(r.next_poll_s, 60);
    CHECK_INT(r.len, 2);
    CHECK(!esl_reply_parse((const uint8_t *)"\x02\x01\x00\x00\x00", 5, &r));
    CHECK(!esl_reply_parse((const uint8_t *)"\x02\x02\x00\x00", 4, &r));
}

static void test_end_to_end(void)
{
    esl_faketag_t t;
    const char *body = "{\"layout\":\"price\",\"fields\":[\"Gr\\u00f8nnsaker\",\"19,90\",\"kr/kg\"],\"price_highlight\":true}";

    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_faketag_init(&t, 0x00000123456789A8ull, 1, 2900, ESL_MODEL_29);

    /* new tag: IDLE, shows up in the API with content_id 0 and its model */
    CHECK_INT(air(&t, 1000, false), ESL_MSG_IDLE);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A8", AUTH, NULL, 0, 2000, &out), 200);
    CHECK(HAS(resp, "\"model\":\"2.9\""));
    CHECK(HAS(resp, "\"content_id\":0,\"shown_content_id\":0,\"pending\":false"));

    /* client sets content: the next wake-up gets its image, the one after confirms it */
    CHECK_INT(put("00000123456789A8", body), 200);
    CHECK(HAS(resp, "\"pending\":true"));
    uint32_t cid = json_u32(resp, "\"content_id\":");
    CHECK_INT(air(&t, 61000, false), ESL_MSG_IMG);
    CHECK_INT(t.img_shown, 1);
    CHECK(t.shown != 0 && t.shown != cid); /* the image id, not the content id */
    CHECK_INT(air(&t, 121000, false), ESL_MSG_IDLE);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A8", AUTH, NULL, 0, 121000, &out), 200);
    CHECK_INT((int)json_u32(resp, "\"shown_content_id\":"), (int)cid);
    CHECK_INT(put("00000123456789A8", body), 200);
    CHECK(HAS(resp, "\"changed\":false,\"pending\":false")); /* same content: no redraw */
    CHECK_INT(air(&t, 181000, false), ESL_MSG_IDLE);
    CHECK_INT(t.img_shown, 1);

    /* the first IMG block lost on the air: the AP sends it again on the next poll */
    CHECK_INT(put("00000123456789A8", "{\"layout\":\"text\",\"fields\":[\"Stengt\"]}"), 200);
    CHECK_INT(air(&t, 241000, true), 0);
    CHECK_INT(air(&t, 301000, false), ESL_MSG_IMG);
    CHECK_INT(t.img_shown, 2);

    /* AP loses power: content comes back from NVS, the tag gets no new image (the
       same content draws the same image, so the id it reports is the right one) */
    uint32_t shown = t.shown;
    ap_boot();
    CHECK_INT(esl_store_count(&st), 1);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A8", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"pending\":true")); /* not seen since boot */
    CHECK_INT(air(&t, 1000, false), ESL_MSG_IDLE);
    CHECK_INT(t.shown, shown);
    CHECK_INT(t.img_shown, 2);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A8", AUTH, NULL, 0, 2000, &out), 200);
    CHECK(HAS(resp, "\"pending\":false"));

    /* content set while the AP was off for the tag is still delivered after a reboot */
    CHECK_INT(put("00000123456789A8", "{\"layout\":\"text\",\"fields\":[\"\\u00c5pent\"]}"), 200);
    ap_boot();
    CHECK_INT(air(&t, 1000, false), ESL_MSG_IMG);
    CHECK_INT(t.img_shown, 3);

    /* tag restarts (shows nothing, flag booted): gets its image again */
    esl_faketag_t again;
    esl_faketag_init(&again, t.mac, 2, 2800, ESL_MODEL_29);
    CHECK_INT(air(&again, 2000, false), ESL_MSG_IMG);
    CHECK_INT(again.shown, t.shown);

    /* a tag without images (no SPI flash) gets no content, and the API says why */
    esl_faketag_t flat;
    esl_faketag_init(&flat, 0x00000123456789A9ull, 2, 2800, ESL_MODEL_29);
    flat.images = false;
    CHECK_INT(put("00000123456789A9", "{\"layout\":\"text\",\"fields\":[\"Hei\"]}"), 200);
    CHECK_INT(air(&flat, 2000, false), ESL_MSG_IDLE);
    CHECK_INT(flat.img_blocks, 0);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A9", AUTH, NULL, 0, 2000, &out), 200);
    CHECK(HAS(resp, "\"pending\":true"));
    CHECK(HAS(resp, "\"content_error\":\"the tag cannot show images drawn by the AP (old firmware, or no SPI flash)\""));

    /* next_poll_s reaches the tag in every reply; a tag that understands
     * CONFIG also gets it once as its base interval */
    esl_store_set_next_poll(&st, 300);
    CHECK_INT(air(&again, 3000, false), ESL_MSG_CONFIG);
    CHECK_INT(again.next_poll_s, 300);
    CHECK_INT(again.cfg.poll_s, 300);
    CHECK_INT(air(&again, 4000, false), ESL_MSG_IDLE);
    CHECK_INT(again.interval_s, 300);
}

static int put_cfg(const char *mac, const char *body)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "/api/tags/%s/config", mac);
    return esl_api_handle(&api, "PUT", uri, AUTH, body, strlen(body), 0, &out);
}

static void test_config(void)
{
    esl_faketag_t t, old;
    const char *body = "{\"layout\":\"text\",\"fields\":[\"Hei\"]}";

    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_faketag_init(&t, 0xC0FFEE01, 2, 3000, ESL_MODEL_154);
    esl_faketag_init(&old, 0xC0FFEE02, 1, 3000, ESL_MODEL_29);
    old.legacy = true;

    /* nothing set: no CONFIG, the tag keeps its built-in 60 s */
    CHECK_INT(air(&t, 1000, false), ESL_MSG_IDLE);
    CHECK_INT(air(&old, 1000, false), ESL_MSG_IDLE);
    CHECK_INT(t.interval_s, ESL_FAKETAG_POLL_S);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000000C0FFEE01", AUTH, NULL, 0, 2000, &out), 200);
    CHECK(HAS(resp, "\"poll_s\":null,\"config\":{\"poll_s\":null,\"max_poll_s\":null,\"lost_ap_s\":null,\"long_poll_s\":null,\"long_after_min\":null},"
                    "\"config_pending\":false"));

    /* per-tag settings: the tag gets CONFIG, uses the new interval and confirms */
    CHECK_INT(put_cfg("00000000C0FFEE01", "{\"poll_s\":120,\"lost_ap_s\":65535}"), 200);
    CHECK(HAS(resp, "\"poll_s\":120,\"config\":{\"poll_s\":120,\"max_poll_s\":null,\"lost_ap_s\":65535,\"long_poll_s\":null,\"long_after_min\":null}"));
    CHECK(HAS(resp, "\"config_pending\":true,\"changed\":true"));
    CHECK_INT(air(&t, 61000, false), ESL_MSG_CONFIG);
    CHECK_INT(t.interval_s, 120);
    CHECK_INT(t.cfg.poll_s, 120);
    CHECK_INT(t.cfg.max_poll_s, 0);
    CHECK_INT(t.cfg.lost_ap_s, ESL_CFG_LOST_AP_NEVER);
    CHECK(t.cfg_ver != 0);
    CHECK_INT(air(&t, 181000, false), ESL_MSG_IDLE); /* confirmed by cfg_ver in POLL */
    CHECK_INT(t.configs, 1);
    CHECK_INT(t.interval_s, 120);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000000C0FFEE01/config", AUTH, NULL, 0, 2000, &out), 200);
    CHECK(HAS(resp, "\"config_pending\":false"));
    CHECK_INT(put_cfg("00000000C0FFEE01", "{\"poll_s\":120,\"lost_ap_s\":65535}"), 200);
    CHECK(HAS(resp, "\"changed\":false"));
    CHECK_INT(air(&t, 301000, false), ESL_MSG_IDLE);

    /* the settings go first, the image on the next poll; next_poll_s in IMG is the tag's own */
    CHECK_INT(put("00000000C0FFEE01", body), 200);
    CHECK_INT(put_cfg("00000000C0FFEE01", "{\"poll_s\":30}"), 200);
    CHECK_INT(air(&t, 331000, false), ESL_MSG_CONFIG);
    CHECK_INT(t.cfg.lost_ap_s, 0);
    CHECK_INT(air(&t, 361000, false), ESL_MSG_IMG);
    CHECK_INT(t.interval_s, 30);
    CHECK_INT(air(&t, 391000, false), ESL_MSG_IDLE);
    CHECK_INT(t.img_shown, 1);

    /* old firmware: never gets CONFIG, but next_poll_s still carries its interval */
    CHECK_INT(put_cfg("00000000C0FFEE02", "{\"poll_s\":900}"), 200);
    CHECK(HAS(resp, "\"config_pending\":false"));
    CHECK_INT(air(&old, 2000, false), ESL_MSG_IDLE);
    CHECK_INT(old.interval_s, 900);
    CHECK_INT(old.rejected, 0);

    /* the AP restarts: settings come back from NVS, no new CONFIG for a tag that has them */
    ap_boot();
    CHECK_INT(air(&t, 1000, false), ESL_MSG_IDLE);
    CHECK_INT(t.interval_s, 30);
    CHECK_INT(air(&old, 1000, false), ESL_MSG_IDLE);
    CHECK_INT(old.interval_s, 900);

    /* the tag restarts with built-in settings: gets them again */
    esl_faketag_t again;
    esl_faketag_init(&again, t.mac, 2, 3000, ESL_MODEL_154);
    CHECK_INT(air(&again, 2000, false), ESL_MSG_CONFIG);
    CHECK_INT(air(&again, 3000, false), ESL_MSG_IMG);
    CHECK_INT(again.cfg_ver, t.cfg_ver);

    /* a run-time default reaches tags without their own poll_s, is persisted, and null resets it */
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/config", AUTH, "{\"poll_s\":240}", 14, 0, &out), 200);
    CHECK_STR(resp, "{\"poll_s\":240,\"builtin_poll_s\":0,\"webhook\":null}");
    CHECK(nvs.has_next_poll);
    CHECK_INT(air(&again, 4000, false), ESL_MSG_IDLE);       /* has its own poll_s */
    CHECK_INT(again.interval_s, 30);
    CHECK_INT(put_cfg("00000000C0FFEE01", "{}"), 200);       /* back to defaults */
    CHECK(!nvs.cfg_used[0] && nvs.cfg_used[1]);      /* slot 0 erased, the old tag keeps its own */
    CHECK_INT(air(&again, 5000, false), ESL_MSG_CONFIG);
    CHECK_INT(again.interval_s, 240);
    CHECK_INT(again.cfg.poll_s, 240);
    ap_boot();
    CHECK_INT(esl_store_get_next_poll(&st), 240);
    CHECK_INT(air(&again, 6000, false), ESL_MSG_IDLE);
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/config", AUTH, "{\"poll_s\":null}", 15, 0, &out), 200);
    CHECK(!nvs.has_next_poll);
    CHECK_INT(air(&again, 7000, false), ESL_MSG_CONFIG);     /* back to all defaults: cfg_ver 0 */
    CHECK_INT(again.cfg_ver, 0);
    CHECK_INT(again.interval_s, ESL_FAKETAG_POLL_S);
    CHECK_INT(air(&again, 8000, false), ESL_MSG_IDLE);

    /* DELETE forgets the stored settings */
    CHECK_INT(put_cfg("00000000C0FFEE02", "{\"poll_s\":900}"), 200);
    CHECK_INT(esl_api_handle(&api, "DELETE", "/api/tags/00000000C0FFEE02", AUTH, NULL, 0, 0, &out), 200);
    bool any = false;
    for (unsigned i = 0; i < CAP; i++)
        any |= nvs.cfg_used[i];
    CHECK(!any);
}

static void test_many_tags(void)
{
    static esl_faketag_t t[CAP];
    char mac[17], body[128];

    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    for (unsigned i = 0; i < CAP; i++) {
        esl_faketag_init(&t[i], 0xF000 + i, 1, 3000, i % 2 ? ESL_MODEL_154 : ESL_MODEL_29);
        CHECK_INT(air(&t[i], 0, false), ESL_MSG_IDLE);
    }
    CHECK_INT(esl_store_count(&st), CAP);

    /* one batch for all of them, each with its own price */
    esl_sb_t sb;
    static char batch[8192];
    esl_sb_init(&sb, batch, sizeof(batch));
    esl_sb_puts(&sb, "{\"tags\":[");
    for (unsigned i = 0; i < CAP; i++) {
        esl_mac_format(t[i].mac, mac);
        esl_sb_printf(&sb, "%s{\"mac\":\"%s\",\"layout\":\"price\",\"fields\":[\"Vare %u\",\"%u,90\"]}", i ? "," : "",
                      mac, i, i);
    }
    esl_sb_puts(&sb, "]}");
    CHECK(!sb.overflow);
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/tags", AUTH, batch, sb.len, 0, &out), 200);
    CHECK_INT(nvs.writes, CAP);

    /* each gets its image; the next POLL confirms it and frees the AP's buffer */
    for (unsigned i = 0; i < CAP; i++) {
        CHECK_INT(air(&t[i], 60000, false), ESL_MSG_IMG);
        CHECK_INT(air(&t[i], 61000, false), ESL_MSG_IDLE);
    }
    for (unsigned i = 0; i < CAP; i++) {
        esl_mac_format(t[i].mac, mac);
        snprintf(body, sizeof(body), "/api/tags/%s", mac);
        CHECK_INT(esl_api_handle(&api, "GET", body, AUTH, NULL, 0, 120000, &out), 200);
        CHECK(HAS(resp, "\"pending\":false"));
        CHECK_INT((int)json_u32(resp, "\"image_id\":"), (int)t[i].shown);
        for (unsigned k = 0; k < i; k++)
            CHECK(t[k].shown != t[i].shown); /* each its own price */
    }
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags", AUTH, NULL, 0, 120000, &out), 200);
    CHECK(!HAS(resp, "\"pending\":true"));
    CHECK(HAS(resp, "\"model\":\"1.54\""));
}

/* ---- layout files: the AP draws with them ---- */

char *read_example_layout(const char *name, size_t *len); /* test_layout.c */

static int put_layout(const char *model, const char *file, size_t len)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "/api/layouts/%s", model);
    return esl_api_handle(&api, "PUT", uri, AUTH, file, len, 0, &out);
}

static const char *const SHELF =
    "{\"layout\":\"shelf\",\"fields\":[\"Kaffe 500 g\",\"49,90\",\"kr 99,80/kg\",\"Tilbud\"],\"price_highlight\":true}";

static uint32_t image_id_of(const char *mac)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "/api/tags/%s", mac);
    CHECK_INT(esl_api_handle(&api, "GET", uri, AUTH, NULL, 0, 0, &out), 200);
    return json_u32(resp, "\"image_id\":");
}

static void test_image_send(void)
{
    size_t l29;
    char *f29 = read_example_layout("2.9.json", &l29);
    CHECK(f29 != NULL);
    if (!f29)
        return;

    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_layouts_init(&layouts, NULL);
    images_boot();
    esl_faketag_t a, b;
    esl_faketag_init(&a, 0x29A, 1, 3000, ESL_MODEL_29);
    a.ota = true;
    a.img_rle = a.img_2d = false; /* raw images: the block count below */
    esl_faketag_init(&b, 0x29B, 1, 3000, ESL_MODEL_29);
    b.images = false; /* no SPI flash */

    /* no content: IDLE, nothing drawn */
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 0), ESL_MSG_IDLE);
    CHECK_INT(images.drawn, 0);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"image\":true,\"image_id\":null"));

    /* built-in content: drawn at the POLL, block 0 in the answer, the rest on IMG_REQ, all in one wake-up */
    CHECK_INT(put("000000000000029A", "{\"layout\":\"price\",\"fields\":[\"Melk\",\"24,90\"]}"), 200);
    CHECK_INT(put("000000000000029B", "{\"layout\":\"price\",\"fields\":[\"Melk\",\"24,90\"]}"), 200);
    uint32_t cid = json_u32(resp, "\"content_id\":");
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 1000), ESL_MSG_IMG);
    CHECK_INT(images.drawn, 1);
    uint32_t id = image_id_of("000000000000029A");
    CHECK(id != 0 && a.shown == id);
    CHECK_INT(a.img_shown, 1);
    CHECK_INT(a.img_blocks, (esl_render_size(ESL_MODEL_29) + ESL_IMG_CHUNK_MAX - 1) / ESL_IMG_CHUNK_MAX);
    CHECK_INT(a.img_size_sent, esl_render_size(ESL_MODEL_29) % ESL_IMG_CHUNK_MAX);
    CHECK(HAS(resp, "\"pending\":true")); /* until the next POLL reports the id */
    CHECK_INT(esl_faketag_round(&b, &st, &layouts, PAN, -60, 1000), ESL_MSG_IDLE);
    CHECK_INT(b.shown, 0);

    /* the next POLL reports the id: shown, the buffer is free, nothing more is sent */
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 61000), ESL_MSG_IDLE);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"pending\":false"));
    CHECK_INT((int)json_u32(resp, "\"shown_content_id\":"), (int)cid);
    CHECK_INT((int)json_u32(resp, "\"shown_image_id\":"), (int)id);
    CHECK_INT(images.buf[0].mac, 0);
    CHECK_INT(images.buf[1].mac, 0);
    CHECK_INT(images.drawn, 1);

    /* GET .../image: the same bytes, for the dashboard; also for a tag without images */
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A/image", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"model\":\"2.9\",\"width\":296,\"height\":128"));
    CHECK_INT((int)json_u32(resp, "\"image_id\":"), (int)id);
    {
        const char *d = strstr(resp, "\"data\":\"");
        const char *e = d ? strchr(d + 8, '"') : NULL;
        CHECK(e && (size_t)(e - d - 8) == (esl_render_size(ESL_MODEL_29) + 2) / 3 * 4);
    }
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029B/image", AUTH, NULL, 0, 0, &out), 200);
    CHECK_INT((int)json_u32(resp, "\"image_id\":"), (int)id); /* the same content */
    CHECK_INT(images.drawn, 1);                                  /* not through the buffers */
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/0000000000000999/image", AUTH, NULL, 0, 0, &out), 404);
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/tags/000000000000029A/image", AUTH, "{}", 2, 0, &out), 415);
    CHECK_INT(esl_api_handle(&api, "DELETE", "/api/tags/000000000000029A/image", AUTH, NULL, 0, 0, &out), 405);

    /* the same content again gives the same image: nothing to send */
    CHECK_INT(put("000000000000029A", "{\"layout\":\"price\",\"fields\":[\"Melk\",\"24,90\"]}"), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 121000), ESL_MSG_IDLE);
    CHECK_INT(images.drawn, 1); /* the same content id: the image id is known */
    CHECK_INT(a.img_shown, 1);

    /* named layout: drawn on the AP from the file; until it shows
       the new image, shown_content_id is 0, not the old image's id */
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    CHECK_INT(put("000000000000029A", SHELF), 200);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"shown_content_id\":0,\"pending\":true"));
    CHECK_INT((int)json_u32(resp, "\"shown_image_id\":"), (int)id);
    uint32_t blocks = a.img_blocks;
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 181000), ESL_MSG_IMG);
    CHECK_INT(a.img_shown, 2);
    CHECK(a.img_blocks > blocks && a.shown == image_id_of("000000000000029A"));
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 241000), ESL_MSG_IDLE);

    /* a new file for the model: the tag's image is drawn again with it, and back */
    {
        char *f2 = malloc(l29 + 1);
        memcpy(f2, f29, l29);
        f2[l29] = 0;
        char *band = strstr(f2, "\"h\": 30, \"color\": \"black\"");
        CHECK(band != NULL);
        if (band)
            band[6] = '2'; /* the black band 20 px instead of 30 */
        uint32_t old = a.shown;
        CHECK_INT(put_layout("2.9", f2, l29), 200);
        CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 251000), ESL_MSG_IMG);
        CHECK(a.shown != old && a.shown == image_id_of("000000000000029A"));
        CHECK_INT(a.img_shown, 3);
        CHECK_INT(put_layout("2.9", f29, l29), 200);
        CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 261000), ESL_MSG_IMG);
        CHECK(a.shown == old);
        CHECK_INT(a.img_shown, 4);
        CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 271000), ESL_MSG_IDLE);
        free(f2);
    }

    /* the file is deleted: the tag keeps the image it shows (it needs no file), and the API
       says its content could not be drawn again */
    CHECK_INT(esl_api_handle(&api, "DELETE", "/api/layouts/2.9", AUTH, NULL, 0, 0, &out), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 301000), ESL_MSG_IDLE);
    CHECK_INT(a.img_shown, 4);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"pending\":false"));
    CHECK(HAS(resp, "\"content_error\":\"the layout is not in the layout file for this tag's model\""));
    /* named content set while the file was there, which is gone when the tag polls: held back,
       like for a tag that draws itself */
    CHECK_INT(put("000000000000029A", "{\"layout\":\"sign\",\"fields\":[\"Velkommen\",\"Stengt\"]}"), 422);
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    CHECK_INT(put("000000000000029A", "{\"layout\":\"sign\",\"fields\":[\"Velkommen\",\"Stengt\"]}"), 200);
    CHECK_INT(esl_api_handle(&api, "DELETE", "/api/layouts/2.9", AUTH, NULL, 0, 0, &out), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 331000), ESL_MSG_IDLE);
    CHECK_INT(a.img_shown, 4);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"content_error\":\"the layout is not in the layout file for this tag's model\""));
    CHECK(HAS(resp, "\"pending\":true"));

    /* content that reaches the tag without PUT /api/tags (the schedule sets it with
       esl_store_set) is checked against the file at the POLL: held back when it does not fit */
    {
        const char *c128 = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"c128\","
                           "\"fields\":[\"code\"],\"elements\":[{\"type\":\"barcode\",\"field\":\"code\","
                           "\"symbology\":\"code128\",\"x\":0,\"y\":0,\"w\":60,\"h\":40}]}]}";
        CHECK_INT(put_layout("2.9", c128, strlen(c128)), 200);
        esl_content_t c;
        esl_set_result_t r;
        CHECK(esl_content_init_named(&c, "c128", 4, 0) == ESL_CONTENT_OK);
        CHECK(esl_content_add_field(&c, (const uint8_t *)"WXYZ", 4) == ESL_CONTENT_OK);
        CHECK(esl_store_set(&st, 0x29A, &c, &r) == ESL_STORE_OK);
        uint32_t d = images.drawn;
        CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 341000), ESL_MSG_IDLE);
        CHECK_INT(images.drawn, d);
        CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
        CHECK(HAS(resp, "does not fit its box"));
        CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 351000), ESL_MSG_IDLE); /* and stays so */
        CHECK_INT(images.drawn, d);
        CHECK_INT(put_layout("2.9", f29, l29), 200);
        CHECK_INT(put("000000000000029A", "{\"layout\":\"sign\",\"fields\":[\"Velkommen\",\"Stengt\"]}"), 200);
    }

    /* a tag that shows its image when a file comes that its content no longer fits: it keeps
       that image, and the API and health say why it gets no new one */
    {
        const char *wide = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"c128\","
                           "\"fields\":[\"code\"],\"elements\":[{\"type\":\"barcode\",\"field\":\"code\","
                           "\"symbology\":\"code128\",\"x\":0,\"y\":0,\"w\":290,\"h\":40}]}]}";
        const char *narrow = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"c128\","
                             "\"fields\":[\"code\"],\"elements\":[{\"type\":\"barcode\",\"field\":\"code\","
                             "\"symbology\":\"code128\",\"x\":0,\"y\":0,\"w\":60,\"h\":40}]}]}";
        CHECK_INT(put_layout("2.9", wide, strlen(wide)), 200);
        CHECK_INT(put("000000000000029A", "{\"layout\":\"c128\",\"fields\":[\"WXYZ\"]}"), 200);
        CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 353000), ESL_MSG_IMG);
        CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 354000), ESL_MSG_IDLE); /* it shows it */
        CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
        CHECK(HAS(resp, "\"pending\":false"));
        CHECK(HAS(resp, "\"content_error\":null"));
        uint32_t d = images.drawn;
        CHECK_INT(put_layout("2.9", narrow, strlen(narrow)), 200);
        CHECK(HAS(resp, "\"content_errors_total\":1"));
        CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 356000), ESL_MSG_IDLE);
        CHECK_INT(images.drawn, d);
        CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
        CHECK(HAS(resp, "does not fit its box"));
        CHECK_INT(esl_api_handle(&api, "GET", "/api/health", AUTH, NULL, 0, 0, &out), 200);
        const char *h = strstr(resp, "{\"mac\":\"000000000000029A\",\"problems\":[");
        CHECK(h && strstr(h, "content_error") && strstr(h, "content_error") < strchr(h, ']'));
        /* the wide file again: it fits, and the error is gone */
        CHECK_INT(put_layout("2.9", wide, strlen(wide)), 200);
        CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
        CHECK(HAS(resp, "\"content_error\":null"));
        CHECK_INT(put_layout("2.9", f29, l29), 200);
        CHECK_INT(put("000000000000029A", "{\"layout\":\"sign\",\"fields\":[\"Velkommen\",\"Stengt\"]}"), 200);
    }

    /* the file is back, with the answer to the POLL lost: the retry gets block 0 again */
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    uint32_t drawn = images.drawn;
    CHECK_INT(air(&a, 361000, true), 0);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 361000), ESL_MSG_IMG);
    CHECK_INT(images.drawn, drawn + 1);
    CHECK_INT(a.img_shown, 6);

    /* the content changes while the tag downloads: the next block it asks for is block 0 of the new image */
    CHECK_INT(put("000000000000029A", "{\"layout\":\"price\",\"fields\":[\"Ost\",\"99,00\"]}"), 200);
    {
        uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
        size_t n = esl_faketag_poll(&a, PAN, rx);
        size_t m = esl_radio_handle(&st, &layouts, PAN, rx, n, -60, 421000, tx, NULL);
        CHECK_INT(esl_faketag_receive(&a, tx, m), ESL_MSG_IMG);
        uint32_t first = a.img_dl_id;
        CHECK(first != 0 && a.img_dl_got == ESL_IMG_CHUNK_MAX);
        CHECK_INT(put("000000000000029A", "{\"layout\":\"price\",\"fields\":[\"Ost\",\"89,00\"]}"), 200);
        n = esl_faketag_img_request(&a, PAN, rx);
        m = esl_radio_handle(&st, &layouts, PAN, rx, n, -60, 421100, tx, NULL);
        CHECK_INT(esl_faketag_receive(&a, tx, m), ESL_MSG_IMG);
        CHECK(a.img_dl_id != first && a.img_dl_got == ESL_IMG_CHUNK_MAX);
        for (int req = 0; req < ESL_FAKETAG_IMG_REQS && (n = esl_faketag_img_request(&a, PAN, rx)) != 0; req++) {
            m = esl_radio_handle(&st, &layouts, PAN, rx, n, -60, 421200 + req, tx, NULL);
            if (!m || esl_faketag_receive(&a, tx, m) != ESL_MSG_IMG)
                break;
        }
        CHECK_INT(a.img_shown, 7);
        CHECK(a.shown == image_id_of("000000000000029A"));
    }
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 481000), ESL_MSG_IDLE);

    /* the panel fails: DRAWFAIL with the id, not offered again until the content changes */
    a.img_bad = true;
    CHECK_INT(put("000000000000029A", "{\"layout\":\"price\",\"fields\":[\"Smør\",\"49,90\"]}"), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 541000), ESL_MSG_IMG);
    CHECK_INT(a.img_shown, 7);
    CHECK(a.img_failed != 0);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 601000), ESL_MSG_IDLE);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"draw_failed\":true"));
    CHECK(HAS(resp, "\"pending\":true"));
    drawn = images.drawn;
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 661000), ESL_MSG_IDLE);
    CHECK_INT(images.drawn, drawn);
    a.img_bad = false;
    CHECK_INT(put("000000000000029A", "{\"layout\":\"price\",\"fields\":[\"Smør\",\"39,90\"]}"), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 721000), ESL_MSG_IMG);
    CHECK_INT(a.img_shown, 8);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 781000), ESL_MSG_IDLE);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"draw_failed\":false"));

    /* the AP restarts: the tag shows its image, the AP draws it once to know the id and sends nothing */
    ap_boot();
    images_boot();
    drawn = images.drawn;
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 841000), ESL_MSG_IDLE);
    CHECK_INT(images.drawn, drawn + 1);
    CHECK_INT(a.img_shown, 8);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"pending\":false"));

    /* every buffer busy: the third tag is told to come back in ESL_IMAGE_BUSY_S */
    esl_faketag_t c, d;
    esl_faketag_init(&c, 0x29C, 1, 3000, ESL_MODEL_29);
    esl_faketag_init(&d, 0x29D, 1, 3000, ESL_MODEL_29);
    c.images = d.images = true;
    c.img_2d = d.img_2d = false; /* RLE: more than one block, so the buffers stay busy */
    CHECK_INT(put("000000000000029A", "{\"layout\":\"price\",\"fields\":[\"A\",\"1\"]}"), 200);
    CHECK_INT(put("000000000000029C", "{\"layout\":\"price\",\"fields\":[\"C\",\"3\"]}"), 200);
    CHECK_INT(put("000000000000029D", "{\"layout\":\"price\",\"fields\":[\"D\",\"4\"]}"), 200);
    {
        uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
        size_t n = esl_faketag_poll(&a, PAN, rx);
        CHECK(esl_radio_handle(&st, &layouts, PAN, rx, n, -60, 901000, tx, NULL) != 0); /* buffer 1 */
        n = esl_faketag_poll(&c, PAN, rx);
        CHECK(esl_radio_handle(&st, &layouts, PAN, rx, n, -60, 901100, tx, NULL) != 0); /* buffer 2 */
    }
    CHECK_INT(esl_faketag_round(&d, &st, &layouts, PAN, -60, 901200), ESL_MSG_IDLE);
    CHECK_INT(d.next_poll_s, ESL_IMAGE_BUSY_S);
    CHECK_INT(d.img_blocks, 0);
    /* a gets the last block: its buffer is free before it polls again, d gets it, and
       its interval back (a reply with 0 would leave it at ESL_IMAGE_BUSY_S) */
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 902000), ESL_MSG_IMG);
    CHECK_INT(esl_faketag_round(&d, &st, &layouts, PAN, -60, 902100), ESL_MSG_IMG);
    CHECK_INT(d.img_shown, 1);
    CHECK_INT(d.interval_s, ESL_TAG_BUILTIN_POLL_S);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 903000), ESL_MSG_IDLE);
    CHECK_INT(esl_faketag_round(&d, &st, &layouts, PAN, -60, 903200), ESL_MSG_IDLE);
    CHECK_INT(d.interval_s, ESL_TAG_BUILTIN_POLL_S);
    /* a tag told to wait that then has nothing to fetch (it showed its image already,
       as after an AP restart) gets its interval back from the plain IDLE too */
    d.interval_s = ESL_IMAGE_BUSY_S;
    CHECK_INT(esl_faketag_round(&d, &st, &layouts, PAN, -60, 903400), ESL_MSG_IDLE);
    CHECK_INT(d.interval_s, ESL_TAG_BUILTIN_POLL_S);
    /* c never came back: its buffer is free again after ESL_IMAGE_HOLD_MS */
    CHECK_INT(esl_faketag_round(&c, &st, &layouts, PAN, -60, 901100 + ESL_IMAGE_HOLD_MS + 1), ESL_MSG_IMG);
    CHECK_INT(c.img_shown, 1);

    /* a tag that is deleted: its buffer goes when it polls again, with no content */
    CHECK_INT(put("000000000000029A", "{\"layout\":\"price\",\"fields\":[\"A\",\"2\"]}"), 200);
    {
        uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
        size_t n = esl_faketag_poll(&a, PAN, rx);
        CHECK(esl_radio_handle(&st, &layouts, PAN, rx, n, -60, 1101000, tx, NULL) != 0);
    }
    CHECK(images.buf[0].mac == 0x29A || images.buf[1].mac == 0x29A);
    CHECK_INT(esl_api_handle(&api, "DELETE", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 1102000), ESL_MSG_IDLE);
    CHECK(images.buf[0].mac != 0x29A && images.buf[1].mac != 0x29A);

    esl_radio_set_image(NULL);
    free(f29);
}

/* ESL_IMG_FMT_RLE: decodes back to the raw image, in place too, and a tag that takes
   it gets the coded image, a fraction of the blocks */
static size_t rle_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t c = in[i++];
        size_t k = c < 0x80 ? (size_t)c + 1 : (size_t)c - 0x80 + 2;
        if (o + k > cap || (c < 0x80 ? i + k > n : i >= n))
            return 0;
        if (c < 0x80) {
            memcpy(out + o, in + i, k);
            i += k;
        } else
            memset(out + o, in[i++], k);
        o += k;
    }
    return o;
}

static void test_image_rle(void)
{
    static uint8_t raw[8192], buf[8192 + 128], back[8192];
    /* edge cases: empty runs, 128/129 boundaries, all alike, all different */
    for (int pass = 0; pass < 6; pass++) {
        size_t n = pass == 0 ? 1 : pass == 1 ? 129 : pass == 2 ? 130 : 5000;
        for (size_t i = 0; i < n; i++)
            raw[i] = pass == 3 ? 0 : pass == 4 ? (uint8_t)(i * 7 + (i >> 3)) : (uint8_t)((i / 37) & 1 ? 0xFF : i % 3);
        size_t m = ESL_IMG_RLE_MARGIN(n);
        memcpy(buf + m, raw, n);
        CHECK_INT((int)esl_img_rle_encode(raw, n, NULL, n + m), (int)esl_img_rle_encode(raw, n, back, n + m));   /* counted */
        size_t c = esl_img_rle_encode(buf + m, n, buf, n + m);   /* in place */
        CHECK(c > 0);
        CHECK_INT((int)rle_decode(buf, c, back, sizeof(back)), (int)n);
        CHECK(memcmp(back, raw, n) == 0);
    }
    CHECK_INT((int)esl_img_rle_encode(raw, 5000, buf, 10), 0);   /* does not fit */
    /* "x yy z ww" stretches then white, coded in place: shorter and exact, or 0 (raw);
       never a code that fits but decodes to something else */
    uint32_t seed = 1;
    int coded = 0;
    for (int k = 0; k < 20000; k++) {
        size_t n = 4736;
        memset(raw, 0, n);
        int parts = 1 + k % 6;
        for (int p = 0; p < parts; p++) {
            seed = seed * 1103515245u + 12345u;
            /* the output overtakes the input only near the start, before white has
               put it behind */
            size_t at = p == 0 ? (seed >> 8) % 64 : (seed >> 8) % (n - 400), len = 30 + (seed >> 20) % 300;
            for (size_t j = 0; j < len; j += 3) {
                raw[at + j] = (uint8_t)(0x11 + j);
                raw[at + j + 1] = raw[at + j + 2] = (uint8_t)(0xA0 ^ (j >> 1) ^ (seed >> 3));
            }
        }
        size_t m = ESL_IMG_RLE_MARGIN(n);
        memcpy(buf + m, raw, n);
        size_t c = esl_img_rle_encode(buf + m, n, buf, n - 1);
        if (c) {
            coded++;
            if (rle_decode(buf, c, back, sizeof(back)) != n || memcmp(back, raw, n) != 0) {
                CHECK(!"in-place code decodes to the image");
                break;
            }
        }
    }
    CHECK(coded > 0);

    size_t l29;
    char *f29 = read_example_layout("2.9.json", &l29);
    CHECK(f29 != NULL);
    if (!f29)
        return;
    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_layouts_init(&layouts, NULL);
    images_boot();
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    esl_faketag_t a;
    esl_faketag_init(&a, 0x29E, 1, 3000, ESL_MODEL_29);
    a.ota = a.images = a.img_rle = true;
    a.img_2d = false;
    CHECK_INT(put("000000000000029E", SHELF), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 1000), ESL_MSG_IMG);
    CHECK_INT(a.img_shown, 1);
    CHECK_INT(images.buf[0].fmt, ESL_IMG_FMT_RLE);
    size_t raw29 = esl_render_size(ESL_MODEL_29);
    /* about a third smaller: 4736 -> 3208 bytes, 35 blocks instead of 52 */
    CHECK(images.buf[0].size < raw29 * 3 / 4);
    CHECK(a.img_blocks < (raw29 + ESL_IMG_CHUNK_MAX - 1) / ESL_IMG_CHUNK_MAX * 3 / 4);
    CHECK(a.shown == image_id_of("000000000000029E"));
    /* the coded bytes are the drawn screen */
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029E", AUTH, NULL, 0, 0, &out), 200);
    CHECK_INT((int)rle_decode(images.buf[0].data, images.buf[0].size, back, sizeof(back)), (int)raw29);
    esl_content_t c;
    CHECK(esl_store_get_content(&st, 0x29E, &c));
    CHECK_INT((int)esl_layouts_render(&layouts, ESL_MODEL_29, &c, raw, sizeof(raw)), (int)raw29);
    CHECK(memcmp(back, raw, raw29) == 0);
    printf("  RLE: 2.9\" shelf screen %zu -> %u bytes, %u blocks\n", raw29, (unsigned)images.buf[0].size,
           (unsigned)a.img_blocks);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 61000), ESL_MSG_IDLE);
    esl_radio_set_image(NULL);
    free(f29);
}

/* ---- pictures from clients (PUT /api/tags/{mac}/image) ---- */

static size_t decode_2d(const uint8_t *in, size_t n, int h, uint8_t *out, size_t cols);

/* The screen in an image buffer, whatever its format, into out */
static size_t screen_of(const esl_image_buf_t *b, uint8_t *out, size_t cap)
{
    uint16_t w, h;
    bool red;
    if (b->fmt == ESL_IMG_FMT_RLE)
        return rle_decode(b->data, b->size, out, cap);
    if (b->fmt == ESL_IMG_FMT_2D) {
        if (!esl_render_model(b->model, &w, &h, &red))
            return 0;
        return decode_2d(b->data, b->size, h, out, esl_render_size(b->model) / ((h + 7u) / 8));
    }
    if (b->size > cap)
        return 0;
    memcpy(out, b->data, b->size);
    return b->size;
}

static void be32_put(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static size_t png_chunk(uint8_t *out, const char *type, const uint8_t *data, size_t len)
{
    be32_put(out, (uint32_t)len);
    memcpy(out + 4, type, 4);
    if (len)
        memcpy(out + 8, data, len);
    be32_put(out + 8 + len, esl_crc32(out + 4, 4 + len));
    return 12 + len;
}

/* An 8-bit grey PNG of w x h with grey(x, y), in stored deflate blocks */
static size_t make_png(uint8_t *out, uint32_t w, uint32_t h, uint8_t (*grey)(uint32_t x, uint32_t y))
{
    static uint8_t rows[200 * 301], z[200 * 301 + 1024];
    size_t n = 0, o = 0;
    for (uint32_t y = 0; y < h; y++) {
        rows[n++] = 0;
        for (uint32_t x = 0; x < w; x++)
            rows[n++] = grey(x, y);
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + rows[i]) % 65521;
        b = (b + a) % 65521;
    }
    z[o++] = 0x78;
    z[o++] = 0x01;
    for (size_t i = 0; i < n; i += 65535) {
        size_t l = n - i < 65535 ? n - i : 65535;
        z[o++] = i + l == n;
        z[o++] = (uint8_t)l;
        z[o++] = (uint8_t)(l >> 8);
        z[o++] = (uint8_t)~l;
        z[o++] = (uint8_t)(~l >> 8);
        memcpy(z + o, rows + i, l);
        o += l;
    }
    be32_put(z + o, b << 16 | a);
    o += 4;
    uint8_t hdr[13] = {0};
    be32_put(hdr, w);
    be32_put(hdr + 4, h);
    hdr[8] = 8;
    size_t m = 8;
    memcpy(out, esl_png_signature, 8);
    m += png_chunk(out + m, "IHDR", hdr, 13);
    m += png_chunk(out + m, "IDAT", z, o);
    m += png_chunk(out + m, "IEND", NULL, 0);
    return m;
}

/* esl_api_image_put's hold: counts, and deletes hold_delete while the lock is let go */
static unsigned hold_calls;
static uint64_t hold_delete;

static void test_hold(bool take)
{
    hold_calls++;
    if (!take && hold_delete)
        esl_store_delete(&st, hold_delete);
}

static uint8_t grey_half(uint32_t x, uint32_t y)
{
    (void)x;
    (void)y;
    return 128;
}

static uint8_t grey_box(uint32_t x, uint32_t y)
{
    return x >= 10 && x < 50 && y >= 20 && y < 30 ? 0 : 255;
}

static int put_picture(const char *mac, const char *query, const uint8_t *body, size_t len)
{
    char uri[96];
    snprintf(uri, sizeof(uri), "/api/tags/%s/image%s", mac, query);
    return esl_api_handle(&api, "PUT", uri, AUTH, (const char *)body, len, 0, &out);
}

/* The tag image of a PNG as the API should make it without dither: ink where the
   pixel is nearer black than white */
static uint8_t *want_img;
static size_t want_cb;

static bool want_header(void *ctx, uint32_t w, uint32_t h)
{
    (void)ctx;
    (void)w;
    want_cb = ESL_RENDER_COL_BYTES(h);
    return true;
}

static bool want_row(void *ctx, uint32_t y, const uint8_t *rgb)
{
    uint32_t w = *(uint32_t *)ctx;
    for (uint32_t x = 0; x < w; x++)
        if (3 * rgb[3 * x] + 4 * rgb[3 * x + 1] + 2 * rgb[3 * x + 2] < 9 * 128)
            want_img[x * want_cb + y / 8] |= (uint8_t)(0x80 >> (y % 8));
    return true;
}

static const uint8_t *src_p;
static size_t src_left;

static bool src_read(void *ctx, uint8_t *buf, size_t n)
{
    (void)ctx;
    if (n > src_left)
        return false;
    memcpy(buf, src_p, n);
    src_p += n;
    src_left -= n;
    return true;
}

static size_t count_ink(const uint8_t *img, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        for (uint8_t b = img[i]; b; b &= (uint8_t)(b - 1))
            k++;
    return k;
}

/* The image buffer a tag's image went out of */
static const esl_image_buf_t *buf_of(uint64_t mac)
{
    static const esl_image_buf_t none;
    for (unsigned i = 0; i < ESL_IMAGE_BUFS; i++)
        if (images.buf[i].mac == mac)
            return &images.buf[i];
    return &none;
}

static void test_picture(void)
{
    static uint8_t want[5000], back[5000], png[200 * 301 + 2048];
    const size_t raw29 = esl_render_size(ESL_MODEL_29), raw154 = esl_render_size(ESL_MODEL_154);
    memset(&nvs, 0, sizeof(nvs));
    esl_layouts_init(&layouts, NULL);
    ap_boot();

    /* screen29 in png_fixtures.h: 296 x 128, 1-bit grey */
    const uint8_t *p29 = png_good[sizeof(png_good) / sizeof(png_good[0]) - 1].data;
    size_t l29 = png_good[sizeof(png_good) / sizeof(png_good[0]) - 1].len;
    CHECK_INT(png_good[sizeof(png_good) / sizeof(png_good[0]) - 1].w, 296);
    uint32_t w29 = 296;
    want_img = want;
    memset(want, 0, sizeof(want));
    src_p = p29;
    src_left = l29;
    esl_png_io_t io = {&w29, src_read, want_header, want_row};
    CHECK_INT(esl_png_decode(&io), ESL_PNG_OK);

    /* a tag must have polled: its model gives the size */
    CHECK_INT(put_picture("00000000000002A1", "", p29, l29), 404);
    CHECK(HAS(resp, "unknown tag"));
    CHECK_INT(put("00000000000002A1", "{\"layout\":\"text\",\"fields\":[\"x\"]}"), 200);
    CHECK_INT(put_picture("00000000000002A1", "", p29, l29), 409);

    esl_faketag_t a, b;
    esl_faketag_init(&a, 0x2A1, 1, 3000, ESL_MODEL_29);
    a.images = a.img_rle = true;
    esl_faketag_init(&b, 0x2A2, 1, 3000, ESL_MODEL_29);
    b.images = true;
    b.img_rle = b.img_2d = false; /* takes raw images only */
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 1000), ESL_MSG_IMG);
    CHECK_INT(esl_faketag_round(&b, &st, &layouts, PAN, -60, 1000), ESL_MSG_IDLE);

    /* errors: the size, the format, a broken file, the query */
    CHECK_INT(put_picture("00000000000002A1", "", png_good[0].data, png_good[0].len), 422);
    CHECK(HAS(resp, "296x128"));
    CHECK_INT(put_picture("00000000000002A1", "", (const uint8_t *)"hello", 5), 415);
    CHECK_INT(put_picture("00000000000002A1", "", NULL, 0), 400);
    CHECK_INT(put_picture("00000000000002A1", "", p29, l29 - 20), 400);
    CHECK_INT(put_picture("00000000000002A1", "?dither=maybe", p29, l29), 400);
    CHECK_INT(nvs.img_writes, 0);

    /* the picture becomes the content, and the tag gets it */
    int writes = nvs.writes;
    CHECK_INT(put_picture("00000000000002A1", "", p29, l29), 200);
    CHECK(HAS(resp, "\"changed\":true,\"pending\":true"));
    CHECK_INT(nvs.img_writes, 1);
    CHECK_INT(nvs.writes, writes + 1);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000000000002A1", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"layout\":\"image\",\"fields\":[],"));
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 61000), ESL_MSG_IMG);
    CHECK_INT(a.img_shown, 2);
    CHECK(a.shown == image_id_of("00000000000002A1"));
    CHECK_INT(buf_of(0x2A1)->fmt, ESL_IMG_FMT_RLE); /* as stored: 2D is no shorter for this photo */
    CHECK_INT((int)rle_decode(buf_of(0x2A1)->data, buf_of(0x2A1)->size, back, sizeof(back)), (int)raw29);
    CHECK(memcmp(back, want, raw29) == 0);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 121000), ESL_MSG_IDLE);

    /* GET .../image gives the same screen */
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000000000002A1/image", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"width\":296,\"height\":128"));

    /* the same picture again: nothing changes, nothing is written */
    CHECK_INT(put_picture("00000000000002A1", "?dither=false", p29, l29), 200);
    CHECK(HAS(resp, "\"changed\":false,\"pending\":false"));
    CHECK_INT(nvs.img_writes, 1);

    /* after a restart the picture is still there and the tag is not sent it again */
    ap_boot();
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 1000), ESL_MSG_IDLE);
    CHECK_INT(a.img_shown, 2);
    CHECK_INT(esl_faketag_round(&b, &st, &layouts, PAN, -60, 1000), ESL_MSG_IDLE);

    /* a tag that takes only raw images: decoded in place from the stored form; the
       raw bytes as a body give the same */
    CHECK_INT(put_picture("00000000000002A2", "", p29, l29), 200);
    CHECK_INT(esl_faketag_round(&b, &st, &layouts, PAN, -60, 2000), ESL_MSG_IMG);
    CHECK_INT(buf_of(0x2A2)->fmt, ESL_IMG_FMT_RAW);
    CHECK_INT(buf_of(0x2A2)->size, raw29);
    CHECK(memcmp(buf_of(0x2A2)->data, want, raw29) == 0);
    CHECK_INT(put_picture("00000000000002A2", "", want, raw29), 200);
    CHECK(HAS(resp, "\"changed\":false"));

    /* other content drops the picture */
    unsigned slot_a = 0;
    for (unsigned s = 0; s < CAP; s++)
        if (nvs.img_len[s] && nvs.img[s][0] == 0xA1)
            slot_a = s;
    CHECK(nvs.img_len[slot_a] > 0);
    CHECK_INT(put("00000000000002A1", "{\"layout\":\"text\",\"fields\":[\"back to text\"]}"), 200);
    CHECK_INT(nvs.img_len[slot_a], 0);
    CHECK_INT(esl_api_handle(&api, "DELETE", "/api/tags/00000000000002A2", AUTH, NULL, 0, 0, &out), 200);
    for (unsigned s = 0; s < CAP; s++)
        CHECK_INT(nvs.img_len[s], 0);

    /* 1.54": grey 128 without dither is all white, with dither about half black */
    esl_faketag_t c;
    esl_faketag_init(&c, 0x2A3, 1, 3000, ESL_MODEL_154);
    c.images = c.img_rle = true;
    CHECK_INT(esl_faketag_round(&c, &st, &layouts, PAN, -60, 3000), ESL_MSG_IDLE);
    size_t n = make_png(png, 200, 200, grey_half);
    CHECK_INT(put_picture("00000000000002A3", "", png, n), 200);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000000000002A3/image", AUTH, NULL, 0, 0, &out), 200);
    CHECK_INT(esl_faketag_round(&c, &st, &layouts, PAN, -60, 4000), ESL_MSG_IMG);
    CHECK_INT(buf_of(0x2A3)->fmt, ESL_IMG_FMT_2D);   /* all white: a few bytes */
    CHECK(buf_of(0x2A3)->size < 50);
    CHECK_INT((int)screen_of(buf_of(0x2A3), back, sizeof(back)), (int)raw154);
    CHECK_INT(count_ink(back, raw154), 0);
    CHECK_INT(put_picture("00000000000002A3", "?dither=true", png, n), 200);
    CHECK(HAS(resp, "\"changed\":true"));
    CHECK_INT(esl_faketag_round(&c, &st, &layouts, PAN, -60, 64000), ESL_MSG_IMG);
    CHECK(buf_of(0x2A3)->fmt != ESL_IMG_FMT_2D);       /* dithered: 2D is no shorter */
    CHECK_INT(screen_of(buf_of(0x2A3), back, sizeof(back)), raw154);
    size_t ink = count_ink(back, raw154);
    CHECK(ink > 200 * 200 * 45 / 100 && ink < 200 * 200 * 55 / 100);
    /* a box: exactly its pixels */
    n = make_png(png, 200, 200, grey_box);
    CHECK_INT(put_picture("00000000000002A3", "?dither=1", png, n), 200);
    CHECK_INT(esl_faketag_round(&c, &st, &layouts, PAN, -60, 124000), ESL_MSG_IMG);
    CHECK_INT(buf_of(0x2A3)->fmt, ESL_IMG_FMT_2D);
    CHECK_INT((int)screen_of(buf_of(0x2A3), back, sizeof(back)), (int)raw154);
    CHECK_INT(count_ink(back, raw154), 40 * 10);
    CHECK(back[10 * 25 + 20 / 8] & (0x80 >> (20 % 8)));
    CHECK(!(back[10 * 25 + 19 / 8] & (0x80 >> (19 % 8))));

    /* streamed: the same as with the body in RAM */
    src_p = png;
    src_left = n;
    CHECK(esl_api_image_put_uri("PUT", "/api/tags/00000000000002A3/image?dither=1"));
    CHECK(esl_api_image_put_uri("PUT", "/api/tags/00000000000002A3/image/"));
    CHECK(!esl_api_image_put_uri("GET", "/api/tags/00000000000002A3/image"));
    CHECK(!esl_api_image_put_uri("PUT", "/api/tags/00000000000002A3"));
    CHECK(!esl_api_image_put_uri("PUT", "/api/tags/2A3/image"));
    CHECK_INT(esl_api_image_put(&api, "/api/tags/00000000000002A3/image", "Bearer nope", n, src_read, NULL, NULL, 0, &out), 401);
    CHECK_INT(esl_api_image_put(&api, "/api/tags/00000000000002A3/image", AUTH, n, src_read, NULL, NULL, 0, &out), 200);
    CHECK(HAS(resp, "\"changed\":false"));
    CHECK_INT(src_left, 0);

    /* the lock is let go while the body comes in; a tag deleted meanwhile is not
       given the picture */
    src_p = png;
    src_left = n;
    hold_calls = 0;
    hold_delete = 0x2A3;
    CHECK_INT(esl_api_image_put(&api, "/api/tags/00000000000002A3/image", AUTH, n, src_read, NULL, test_hold, 0, &out),
              409);
    CHECK_INT(hold_calls, 2);
    esl_tag_t gone;
    CHECK(!esl_store_get(&st, 0x2A3, &gone));
    for (unsigned s = 0; s < CAP; s++)
        CHECK_INT(nvs.img_len[s], 0);
    esl_radio_set_image(NULL);
}

/* The shorter code is sent: 2D on text, RLE on a half dithered picture; and a tag that
   goes back to firmware without 2D gets its image drawn again, not the 2D one it can
   no longer take */
static void test_image_choice(void)
{
    static uint8_t img[4736], back[5000];
    const size_t raw29 = esl_render_size(ESL_MODEL_29);
    memset(&nvs, 0, sizeof(nvs));
    esl_layouts_init(&layouts, NULL);
    ap_boot();
    esl_faketag_t a;
    esl_faketag_init(&a, 0x2B1, 1, 3000, ESL_MODEL_29);
    CHECK_INT(put("00000000000002B1", "{\"layout\":\"text\",\"fields\":[\"x\"]}"), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 1000), ESL_MSG_IMG);
    CHECK_INT(buf_of(0x2B1)->fmt, ESL_IMG_FMT_2D);

    /* 100 dithered columns, the rest white */
    uint32_t seed = 3;
    memset(img, 0, sizeof(img));
    for (size_t i = 0; i < 100 * 16; i++) {
        seed = seed * 1103515245u + 12345u;
        img[i] = (uint8_t)(seed >> 16);
    }
    size_t lr = esl_img_rle_encode(img, raw29, NULL, raw29 - 1), l2 = esl_img_2d_encode(img, raw29, 128, NULL, raw29 - 1);
    CHECK(lr > 0 && l2 > lr);
    CHECK_INT(put_picture("00000000000002B1", "", img, raw29), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 61000), ESL_MSG_IMG);
    CHECK_INT(a.img_shown, 2);
    CHECK_INT(buf_of(0x2B1)->fmt, ESL_IMG_FMT_RLE);
    CHECK_INT(buf_of(0x2B1)->size, lr);
    CHECK_INT((int)screen_of(buf_of(0x2B1), back, sizeof(back)), (int)raw29);
    CHECK(memcmp(back, img, raw29) == 0);

    /* 12 dithered columns at the left, bars in the rest: 2D is the shorter code, but
       coded in place its first columns would overtake the image, so RLE is sent */
    memset(img, 0, sizeof(img));
    for (size_t i = 0; i < 12 * 16; i++) {
        seed = seed * 1103515245u + 12345u;
        img[i] = (uint8_t)(seed >> 16);
    }
    for (size_t x = 12; x < raw29 / 16; x++)
        for (size_t y = 20 + x % 40; y < 70 + x % 40; y++)
            img[x * 16 + y / 8] |= (uint8_t)(0x80 >> (y % 8));
    size_t mg = ESL_IMG_RLE_MARGIN(raw29);
    lr = esl_img_rle_encode(img, raw29, NULL, raw29 - 1);
    l2 = esl_img_2d_encode(img, raw29, 128, NULL, raw29 - 1);
    CHECK(lr > 0 && l2 > 0 && l2 < lr);
    CHECK_INT((int)esl_img_2d_count(img, raw29, 128, mg, raw29 - 1), 0);
    CHECK_INT(put_picture("00000000000002B1", "", img, raw29), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 91000), ESL_MSG_IMG);
    CHECK_INT(buf_of(0x2B1)->fmt, ESL_IMG_FMT_RLE);
    CHECK_INT(buf_of(0x2B1)->size, lr);
    CHECK_INT((int)screen_of(buf_of(0x2B1), back, sizeof(back)), (int)raw29);
    CHECK(memcmp(back, img, raw29) == 0);

    /* only 2D shorter than raw (no RLE): 2D within raw - 1 */
    a.img_rle = false;
    CHECK_INT(put("00000000000002B1", "{\"layout\":\"text\",\"fields\":[\"y\"]}"), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 121000), ESL_MSG_IMG);
    CHECK_INT(buf_of(0x2B1)->fmt, ESL_IMG_FMT_2D);
    uint32_t shown = a.shown;

    /* back to older firmware (RLE, no 2D) and a fresh start: the cached 2D image is not
       sent, it is drawn again as RLE and shown */
    a.img_rle = true;
    a.img_2d = false;
    a.shown = 0;
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 122000), ESL_MSG_IMG);
    CHECK_INT(buf_of(0x2B1)->fmt, ESL_IMG_FMT_RLE);
    CHECK(a.shown != 0 && a.shown != shown);
    CHECK_INT(a.img_failed, 0);
    esl_radio_set_image(NULL);
}

/* ESL_IMG_FMT_2D: a decoder written from the description in shared/proto.h, apart
   from the tag's (tag/src/image.c, tested in tag/test/test_image.c) */
typedef struct {
    const uint8_t *in;
    size_t n, bit;
} rd2d_t;

static int rd2d_bit(rd2d_t *r)
{
    if (r->bit >= r->n * 8)
        return -1;
    int b = (r->in[r->bit >> 3] >> (7 - (r->bit & 7))) & 1;
    r->bit++;
    return b;
}

static int rd2d_run(rd2d_t *r)
{
    int z = 0, b;
    while ((b = rd2d_bit(r)) == 0)
        z++;
    if (b < 0 || z > 16)
        return -1;
    int v = 1;
    for (int i = 0; i < z + 2; i++) {
        if ((b = rd2d_bit(r)) < 0)
            return -1;
        v = v * 2 + b;
    }
    return v - 4;
}

static int px2d(const uint8_t *c, int y)
{
    return y < 0 ? 0 : (c[y / 8] >> (7 - y % 8)) & 1;
}

/* the change after a to colour col (a pixel col whose row above is not), h if none */
static int next2d(const uint8_t *c, int a, int col, int h)
{
    for (int y = a + 1; y < h; y++)
        if (px2d(c, y) == col && px2d(c, y - 1) != col)
            return y;
    return h;
}

static size_t decode_2d(const uint8_t *in, size_t n, int h, uint8_t *out, size_t cols)
{
    static const uint8_t empty[ESL_IMG_2D_MAX_H / 8];
    size_t cb = ((size_t)h + 7) / 8;
    rd2d_t r = {in, n, 0};
    memset(out, 0, cols * cb);
    for (size_t x = 0; x < cols; x++) {
        const uint8_t *ref = x ? out + (x - 1) * cb : empty;
        uint8_t *cur = out + x * cb;
        int a0 = -1, col = 0;
        while (a0 < h) {
            int s = a0 < 0 ? 0 : a0, b1 = next2d(ref, a0, !col, h), zeros = 0, b, a1, end;
            while ((b = rd2d_bit(&r)) == 0 && zeros < 6)
                zeros++;
            if (b != 1)
                return 0;
            if (zeros == 2) {
                int r1 = rd2d_run(&r), r2 = rd2d_run(&r);
                if (r1 < 0 || r2 < 0 || s + r1 + r2 > h || s + r1 + r2 <= a0)
                    return 0;
                a1 = s + r1;
                end = a1 + r2;
            } else if (zeros == 3) {
                a1 = next2d(ref, b1, col, h);
                if (a1 >= h)
                    return 0;
                end = -1;
            } else {
                int d = zeros == 0 ? 0 : zeros == 1 ? 1 : zeros == 4 ? 2 : zeros == 5 ? 3 : -1;
                if (d < 0)
                    return 0;
                if (d && (b = rd2d_bit(&r)) < 0)
                    return 0;
                a1 = d && !b ? b1 - d : b1 + d;
                if (a1 <= a0 || a1 > h)
                    return 0;
                end = -2;
            }
            for (int y = s; y < (end >= 0 ? end : a1); y++)
                if ((y < a1 ? col : !col))
                    cur[y / 8] |= (uint8_t)(0x80 >> (y % 8));
            if (end == -2)
                col = !col;
            a0 = end >= 0 ? end : a1;
        }
    }
    /* only the zeros that fill the last byte after the code */
    if ((r.bit + 7) / 8 != n)
        return 0;
    while (r.bit < n * 8)
        if (rd2d_bit(&r) != 0)
            return 0;
    return cols * cb;
}

/* ESL_IMG_FMT_2D: decodes back to the image, in place too; a tag that takes it gets it */
static void test_image_2d(void)
{
    static uint8_t raw[8192], buf[16384 + 128], back[8192];
    uint32_t seed = 7;
    /* edge cases at several heights, one not a multiple of 8 */
    static const int hs[] = {128, 200, 13, 1, 8};
    for (unsigned hi = 0; hi < sizeof(hs) / sizeof(hs[0]); hi++) {
        int h = hs[hi];
        size_t cb = ((size_t)h + 7) / 8, cols = 296, n = cols * cb;
        for (int pass = 0; pass < 9; pass++) {
            memset(raw, 0, n);
            for (size_t x = 0; x < cols; x++)
                for (int y = 0; y < h; y++) {
                    int v;
                    seed = seed * 1103515245u + 12345u;
                    switch (pass) {
                    case 0: v = 0; break;                                 /* white */
                    case 1: v = 1; break;                                 /* all ink */
                    case 2: v = y & 1; break;                             /* stripes across */
                    case 3: v = x & 1; break;                             /* columns flip */
                    case 4: v = y == 0 || y == h - 1; break;              /* first and last row */
                    case 5: v = (x / 9 + y / 5) & 1; break;               /* checkers */
                    case 6: v = y > (int)(x % (size_t)h); break;          /* a slope: V codes */
                    case 7: v = (seed >> 16) % 23 == 0; break;            /* specks */
                    default: v = (seed >> 16) & 1; break;                 /* noise */
                    }
                    if (v)
                        raw[x * cb + (size_t)y / 8] |= (uint8_t)(0x80 >> (y % 8));
                }
            /* into a buffer of its own, with room: decodes to the image */
            static uint8_t sep[16384];
            size_t c = esl_img_2d_encode(raw, n, (uint16_t)h, sep, sizeof(sep));
            CHECK(c > 0);
            CHECK_INT((int)esl_img_2d_encode(raw, n, (uint16_t)h, NULL, sizeof(sep)), (int)c);   /* counted */
            CHECK_INT((int)esl_img_2d_encode(raw, n, (uint16_t)h, NULL, c - 1), 0);
            CHECK_INT((int)decode_2d(sep, c, h, back, cols), (int)n);
            CHECK(memcmp(back, raw, n) == 0);
            if (c > 1)
                CHECK_INT((int)esl_img_2d_encode(raw, n, (uint16_t)h, buf, c - 1), 0);   /* does not fit */
            /* in place, shorter than the image as the AP codes it: the same code, or 0 */
            size_t m = ESL_IMG_RLE_MARGIN(n);
            memcpy(buf + m, raw, n);
            size_t ci = esl_img_2d_encode(buf + m, n, (uint16_t)h, buf, n - 1);
            CHECK(ci == 0 || (ci == c && memcmp(buf, sep, c) == 0));
            if (h >= 128 && (pass < 2 || pass == 4 || pass == 6))
                CHECK(ci > 0 && ci < n / 4 + 8);   /* few changes: a fraction */
            if (pass == 8 && h >= 8)
                CHECK_INT((int)ci, 0);   /* noise: no shorter, so not coded */
        }
    }
    CHECK_INT((int)esl_img_2d_encode(raw, 100, 128, buf, sizeof(buf)), 0);           /* not whole columns */
    CHECK_INT((int)esl_img_2d_encode(raw, 1000, ESL_IMG_2D_MAX_H + 8, buf, sizeof(buf)), 0);

    size_t l29;
    char *f29 = read_example_layout("2.9.json", &l29);
    CHECK(f29 != NULL);
    if (!f29)
        return;
    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_layouts_init(&layouts, NULL);
    images_boot();
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    esl_faketag_t a;
    esl_faketag_init(&a, 0x29F, 1, 3000, ESL_MODEL_29);
    a.ota = true;
    CHECK_INT(put("000000000000029F", SHELF), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 1000), ESL_MSG_IMG);
    CHECK_INT(a.img_shown, 1);
    CHECK_INT(images.buf[0].fmt, ESL_IMG_FMT_2D);
    size_t raw29 = esl_render_size(ESL_MODEL_29);
    /* a fraction of RLE's: 4736 -> about 530 bytes, 6 blocks instead of 35 */
    CHECK(images.buf[0].size < raw29 / 6);
    CHECK(a.img_blocks <= 8);
    CHECK(a.shown == image_id_of("000000000000029F"));
    CHECK_INT((int)decode_2d(images.buf[0].data, images.buf[0].size, 128, back, 296), (int)raw29);
    esl_content_t c;
    CHECK(esl_store_get_content(&st, 0x29F, &c));
    CHECK_INT((int)esl_layouts_render(&layouts, ESL_MODEL_29, &c, raw, sizeof(raw)), (int)raw29);
    CHECK(memcmp(back, raw, raw29) == 0);
    printf("  2D: 2.9\" shelf screen %zu -> %u bytes, %u blocks\n", raw29, (unsigned)images.buf[0].size,
           (unsigned)a.img_blocks);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 61000), ESL_MSG_IDLE);
    esl_radio_set_image(NULL);
    free(f29);
}

/* esl_link: what radio.c does with every frame, over a link that records what it sends */
typedef struct {
    int starts, sends, fail;
    uint8_t last[ESL_PSDU_MAX];
    size_t last_len;
    int64_t last_rx_us;
    uint32_t last_ref;
    int64_t hand_at_us; /* calls esl_link_sending() at this time, 0 = not */
} fake_link_t;

static int64_t fake_now_us;

static int fake_send(esl_link_t *l, const uint8_t *psdu, size_t len, const esl_link_rx_t *req)
{
    fake_link_t *f = l->ctx;
    f->sends++;
    memcpy(f->last, psdu, len);
    f->last_len = len;
    f->last_rx_us = req->rx_us;
    f->last_ref = req->ref;
    if (f->hand_at_us)
        esl_link_sending(l, req, f->hand_at_us);
    fake_now_us += 4000; /* the time on the air */
    return f->fail;
}

static int64_t fake_clock(void)
{
    return fake_now_us;
}

/* With esl_radio_set_draw_later() (the AP's own radio): nothing is drawn while the tag
 * waits for its reply. A tag whose image is not in a buffer is told to come back in
 * ESL_IMAGE_BUSY_S, the image is drawn once the reply is out (esl_radio_after), and
 * the next POLL gets block 0 at once. Many tags waiting at once (a new AP, a new
 * layout file) all get theirs, two buffers or not. */
static void test_draw_later(void)
{
    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_layouts_init(&layouts, NULL);
    images_boot();
    esl_radio_set_draw_later(true);

    enum { N = 6 };
    esl_faketag_t t[N];
    for (unsigned i = 0; i < N; i++) {
        char mac[17], body[96];
        esl_faketag_init(&t[i], 0x2A0 + i, 1, 3000, ESL_MODEL_29);
        snprintf(mac, sizeof(mac), "%016X", 0x2A0 + i);
        snprintf(body, sizeof(body), "{\"layout\":\"price\",\"fields\":[\"Vare %u\",\"%u,90\"]}", i, 10 + i);
        CHECK_INT(put(mac, body), 200);
    }

    /* the first POLL of each: answered without drawing, told to come back soon; only
       the first one polling at a time is drawn after its reply */
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    esl_radio_event_t ev;
    size_t n = esl_faketag_poll(&t[0], PAN, rx);
    size_t m = esl_radio_handle(&st, &layouts, PAN, rx, n, -60, 1000, tx, &ev);
    CHECK(m != 0);
    CHECK_INT(images.drawn, 0);
    CHECK_INT(esl_faketag_receive(&t[0], tx, m), ESL_MSG_IDLE);
    CHECK_INT(t[0].next_poll_s, ESL_IMAGE_BUSY_S);
    esl_radio_after(&st, &layouts, &ev, 1001);
    CHECK_INT(images.drawn, 1);
    /* back 2 s later: block 0 straight away, the whole image in that wake-up */
    CHECK_INT(esl_faketag_round(&t[0], &st, &layouts, PAN, -60, 3000), ESL_MSG_IMG);
    CHECK_INT(images.drawn, 1);
    CHECK(t[0].shown != 0 && t[0].shown == image_id_of("00000000000002A0"));

    /* the rest poll in turn, each every ESL_IMAGE_BUSY_S while it waits */
    uint64_t now = 10000;
    for (unsigned round = 0; round < 3 * N; round++) {
        for (unsigned i = 1; i < N; i++) {
            if (t[i].shown)
                continue;
            esl_faketag_round(&t[i], &st, &layouts, PAN, -60, now);
            now += 100;
        }
        now += ESL_IMAGE_BUSY_S * 1000;
    }
    for (unsigned i = 0; i < N; i++) {
        char mac[17];
        snprintf(mac, sizeof(mac), "%016X", 0x2A0 + i);
        CHECK(t[i].shown != 0 && t[i].shown == image_id_of(mac));
        CHECK_INT(t[i].interval_s, ESL_TAG_BUILTIN_POLL_S);
    }
    CHECK_INT(images.drawn, N);

    /* a tag that shows its image: plain IDLE, nothing noted or drawn */
    CHECK_INT(esl_faketag_round(&t[0], &st, &layouts, PAN, -60, now), ESL_MSG_IDLE);
    CHECK_INT(images.drawn, N);
    CHECK(images.later == 0);

    /* new content while the tag is part way through its image: the IMG_REQ is not
       answered with a newly drawn image either, but with IDLE in ESL_IMAGE_BUSY_S; the
       image is drawn after that reply and the next POLL starts it over */
    CHECK_INT(put("00000000000002A0", "{\"layout\":\"price\",\"fields\":[\"Ost\",\"89,00\"]}"), 200);
    n = esl_faketag_poll(&t[0], PAN, rx);
    m = esl_radio_handle(&st, &layouts, PAN, rx, n, -60, now, tx, &ev);
    CHECK_INT(esl_faketag_receive(&t[0], tx, m), ESL_MSG_IDLE);
    esl_radio_after(&st, &layouts, &ev, now);
    CHECK_INT(images.drawn, N + 1);
    now += ESL_IMAGE_BUSY_S * 1000;
    n = esl_faketag_poll(&t[0], PAN, rx);
    m = esl_radio_handle(&st, &layouts, PAN, rx, n, -60, now, tx, &ev);
    CHECK_INT(esl_faketag_receive(&t[0], tx, m), ESL_MSG_IMG);
    esl_radio_after(&st, &layouts, &ev, now);
    uint32_t first = t[0].img_dl_id;
    CHECK(first != 0 && t[0].img_dl_got == ESL_IMG_CHUNK_MAX);
    CHECK_INT(put("00000000000002A0", "{\"layout\":\"price\",\"fields\":[\"Kaffe\",\"59,00\"]}"), 200);
    n = esl_faketag_img_request(&t[0], PAN, rx);
    m = esl_radio_handle(&st, &layouts, PAN, rx, n, -60, now + 100, tx, &ev);
    CHECK_INT(images.drawn, N + 1);
    CHECK_INT(esl_faketag_receive(&t[0], tx, m), ESL_MSG_IDLE);
    CHECK_INT(t[0].next_poll_s, ESL_IMAGE_BUSY_S);
    esl_radio_after(&st, &layouts, &ev, now + 100);
    CHECK_INT(images.drawn, N + 2);
    CHECK(images.later == 0);
    now += ESL_IMAGE_BUSY_S * 1000;
    CHECK_INT(esl_faketag_round(&t[0], &st, &layouts, PAN, -60, now), ESL_MSG_IMG);
    CHECK_INT(images.drawn, N + 2);
    CHECK(t[0].shown != first && t[0].shown == image_id_of("00000000000002A0"));

    esl_radio_set_draw_later(false);
    esl_radio_set_image(NULL);
}

static void test_link(void)
{
    fake_link_t fl = {0};
    esl_link_t l = {.name = "fake", .send = fake_send, .ctx = &fl};
    esl_faketag_t t;
    esl_link_rx_t f = {0};
    uint8_t tx[ESL_PSDU_MAX];
    esl_radio_event_t ev;
    bool sent;

    ap_boot();
    esl_faketag_init(&t, 0x00000123456789A8ull, 1, 3000, ESL_MODEL_29);

    /* a POLL: answered through send, with the frame it answers; the driver says when
       the reply went to the radio */
    f.len = (uint8_t)esl_faketag_poll(&t, PAN, f.psdu);
    f.rssi = -60;
    f.rx_us = 1000;
    f.ref = 77;
    fake_now_us = 1500;
    fl.hand_at_us = 1800;
    size_t n = esl_link_handle(&l, &st, &layouts, PAN, &f, 5000, tx, &ev, fake_clock, &sent);
    CHECK(n > 0);
    CHECK(sent);
    CHECK_INT(fl.sends, 1);
    CHECK_INT(fl.last_len, n);
    CHECK_INT(fl.last_rx_us, 1000);
    CHECK_INT(fl.last_ref, 77);
    CHECK_INT(ev.msg_type, ESL_MSG_POLL);
    CHECK(esl_faketag_receive(&t, fl.last, fl.last_len) != 0);
    CHECK_INT(l.stats.rx, 1);
    CHECK_INT(l.stats.polls, 1);
    CHECK_INT(l.stats.replies, 1);
    CHECK_INT(l.stats.last_us, 800);
    CHECK_INT(l.stats.worst_us, 800);
    CHECK_INT(l.stats.slow, 0);

    /* a frame that needs no reply: counted as received, nothing sent */
    f.psdu[0] ^= 0xFF;
    fake_now_us = 9000;
    CHECK_INT(esl_link_handle(&l, &st, &layouts, PAN, &f, 5000, tx, NULL, fake_clock, &sent), 0);
    CHECK(!sent);
    CHECK_INT(fl.sends, 1);
    CHECK_INT(l.stats.rx, 2);
    CHECK_INT(l.stats.worst_us, 800);

    /* the link could not send: counted as failed, not as a reply; a driver that
       does not call esl_link_sending() is timed when send() returns */
    fl.fail = -1;
    fl.hand_at_us = 0;
    f.len = (uint8_t)esl_faketag_poll(&t, PAN, f.psdu);
    fake_now_us = 1300 - 4000;
    CHECK(esl_link_handle(&l, &st, &layouts, PAN, &f, 6000, tx, NULL, fake_clock, &sent) > 0);
    CHECK(!sent);
    CHECK_INT(l.stats.polls, 2);
    CHECK_INT(l.stats.replies, 1);
    CHECK_INT(l.stats.tx_failed, 1);
    CHECK_INT(l.stats.last_us, 300);
    CHECK_INT(l.stats.worst_us, 800);

    /* a reply on its way later than ESL_LINK_SLOW_US is counted as slow */
    fl.fail = 0;
    f.len = (uint8_t)esl_faketag_poll(&t, PAN, f.psdu);
    f.rx_us = 1000;
    fake_now_us = 1000;
    fl.hand_at_us = 1000 + ESL_LINK_SLOW_US + 1;
    CHECK(esl_link_handle(&l, &st, &layouts, PAN, &f, 7000, tx, NULL, fake_clock, &sent) > 0);
    CHECK(sent);
    CHECK_INT(l.stats.slow, 1);
    CHECK_INT(l.stats.worst_us, ESL_LINK_SLOW_US + 1);
    CHECK_INT(l.stats.replies, 2);

    /* a driver whose radio reports later loses a reply it had taken */
    esl_link_lost(&l);
    CHECK_INT(l.stats.replies, 1);
    CHECK_INT(l.stats.tx_failed, 2);
}

/* Date fields in a layout (#183): the AP fills them in when it draws, and draws a tag
   again when the day (or for {week} alone, the week) changes; a tag whose layout has
   none is not drawn again */
static void test_image_clock(void)
{
    static const char file[] =
        "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":["
        "{\"name\":\"day\",\"fields\":[\"name\"],\"elements\":["
        "{\"type\":\"text\",\"field\":\"name\",\"x\":0,\"y\":0,\"w\":296,\"h\":40,\"fonts\":[\"medium\"]},"
        "{\"type\":\"text\",\"text\":\"{weekday} {date}\",\"x\":0,\"y\":60,\"w\":296,\"h\":40,\"fonts\":[\"medium\"]}]},"
        "{\"name\":\"week\",\"fields\":[\"name\"],\"elements\":["
        "{\"type\":\"text\",\"field\":\"name\",\"x\":0,\"y\":0,\"w\":296,\"h\":40,\"fonts\":[\"medium\"]},"
        "{\"type\":\"text\",\"text\":\"Week {week}\",\"x\":0,\"y\":60,\"w\":296,\"h\":40,\"fonts\":[\"medium\"]}]},"
        "{\"name\":\"plain\",\"fields\":[\"name\"],\"elements\":["
        "{\"type\":\"text\",\"field\":\"name\",\"x\":0,\"y\":0,\"w\":296,\"h\":40,\"fonts\":[\"medium\"]}]}]}";

    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_layouts_init(&layouts, NULL);
    images_boot();
    esl_clock_set_today(-1);
    CHECK_INT(put_layout("2.9", file, strlen(file)), 200);
    esl_faketag_t d, w, p;
    esl_faketag_init(&d, 0xDA1, 1, 3000, ESL_MODEL_29);
    d.img_rle = d.img_2d = false; /* raw: GET .../image gives the same id */
    esl_faketag_init(&w, 0xDA2, 1, 3000, ESL_MODEL_29);
    esl_faketag_init(&p, 0xDA3, 1, 3000, ESL_MODEL_29);
    CHECK_INT(put("0000000000000DA1", "{\"layout\":\"day\",\"fields\":[\"Menu\"]}"), 200);
    CHECK_INT(put("0000000000000DA2", "{\"layout\":\"week\",\"fields\":[\"Menu\"]}"), 200);
    CHECK_INT(put("0000000000000DA3", "{\"layout\":\"plain\",\"fields\":[\"Menu\"]}"), 200);

    /* the time not known yet (after a restart, before NTP): the date layouts are not drawn,
       and their tags keep what they show; the plain one is */
    uint64_t t = 1000;
    CHECK_INT(esl_faketag_round(&d, &st, &layouts, PAN, -60, t), ESL_MSG_IDLE);
    CHECK_INT(esl_faketag_round(&w, &st, &layouts, PAN, -60, t), ESL_MSG_IDLE);
    CHECK_INT(esl_faketag_round(&p, &st, &layouts, PAN, -60, t), ESL_MSG_IMG);
    CHECK(d.shown == 0 && w.shown == 0);
    CHECK_INT(images.drawn, 1);
    uint32_t p0 = p.shown;
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/0000000000000DA1", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"pending\":true"));

    /* then known (Thursday 29 October 2026, week 44): both date layouts get their image */
    esl_clock_set_today(20755);
    t += 10000;
    CHECK_INT(esl_faketag_round(&d, &st, &layouts, PAN, -60, t), ESL_MSG_IMG);
    CHECK_INT(esl_faketag_round(&w, &st, &layouts, PAN, -60, t), ESL_MSG_IMG);
    CHECK_INT(esl_faketag_round(&p, &st, &layouts, PAN, -60, t), ESL_MSG_IDLE);
    CHECK(d.shown != 0 && w.shown != 0 && d.shown != w.shown && p.shown == p0);
    uint32_t d1 = d.shown, w1 = w.shown;
    t += 10000;
    CHECK_INT(esl_faketag_round(&d, &st, &layouts, PAN, -60, t), ESL_MSG_IDLE);
    CHECK_INT(esl_faketag_round(&w, &st, &layouts, PAN, -60, t), ESL_MSG_IDLE);
    unsigned drawn = images.drawn;

    /* Sunday 1 November: a new day, the same week; the plain one is not even drawn */
    esl_clock_set_today(20758);
    t += 10000;
    CHECK_INT(esl_faketag_round(&d, &st, &layouts, PAN, -60, t), ESL_MSG_IMG);
    CHECK_INT(esl_faketag_round(&w, &st, &layouts, PAN, -60, t), ESL_MSG_IDLE);
    CHECK_INT(esl_faketag_round(&p, &st, &layouts, PAN, -60, t), ESL_MSG_IDLE);
    CHECK(d.shown != d1 && w.shown == w1);
    CHECK_INT(images.drawn, drawn + 1);

    /* Monday 2 November: week 45 */
    esl_clock_set_today(20759);
    t += 10000;
    CHECK_INT(esl_faketag_round(&d, &st, &layouts, PAN, -60, t), ESL_MSG_IMG);
    CHECK_INT(esl_faketag_round(&w, &st, &layouts, PAN, -60, t), ESL_MSG_IMG);
    CHECK_INT(esl_faketag_round(&p, &st, &layouts, PAN, -60, t), ESL_MSG_IDLE);
    CHECK(w.shown != w1 && p.shown == p0);

    /* GET .../image draws what the tag is sent */
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/0000000000000DA1/image", AUTH, NULL, 0, 0, &out), 200);
    CHECK_INT((int)json_u32(resp, "\"image_id\":"), (int)d.shown);
    esl_clock_set_today(-1);
}

/* ---- Radio security (docs/protokoll.md "Security") ---- */

static int sec_saves;
static void sec_save(void *ctx)
{
    (void)ctx;
    sec_saves++;
}

/* A POLL from mac, plain (key NULL) or with key and counter ctr; the reply's payload (MIC
   checked and taken off for a secure one) into r, its length, 0 for none, -1 for a bad MIC */
static int sec_poll(uint64_t mac, const uint8_t *key, uint32_t ctr, uint8_t *r)
{
    uint8_t pl[ESL_MAX_PAYLOAD], rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX], mic[ESL_SEC_MIC_LEN];
    esl_poll_t p = {1, 3000, 0, 0, ESL_MODEL_29, true, 0, false, 0, 0, 0, 0};
    size_t n = esl_poll_build(pl, &p);
    if (key) {
        pl[ESL_OFF_VER] = ESL_PROTO_VER_SEC;
        pl[n] = (uint8_t)ctr;
        pl[n + 1] = (uint8_t)(ctr >> 8);
        pl[n + 2] = (uint8_t)(ctr >> 16);
        pl[n + 3] = (uint8_t)(ctr >> 24);
        sec_mic(key, ESL_SEC_DIR_TAG, ctr, pl, (uint8_t)n, pl + n + ESL_SEC_CTR_LEN);
        n += ESL_SEC_TRAILER_TAG;
    }
    n = esl_frame_build_from_tag(rx, 7, PAN, ESL_AP_SHORT_ADDR, mac, pl, n);
    size_t m = esl_radio_handle(&st, &layouts, PAN, rx, n, -50, 1000, tx, NULL);
    if (!m)
        return 0;
    esl_ap_frame_t f;
    CHECK(esl_frame_parse_to_tag(tx, m, &f));
    memcpy(r, f.payload, f.payload_len);
    if (!key) {
        CHECK_INT(r[ESL_OFF_VER], ESL_PROTO_VER);
        return (int)f.payload_len;
    }
    CHECK_INT(r[ESL_OFF_VER], ESL_PROTO_VER_SEC);
    size_t len = f.payload_len - ESL_SEC_TRAILER_AP;
    sec_mic(key, ESL_SEC_DIR_AP, ctr, r, (uint8_t)len, mic);
    if (memcmp(mic, r + len, ESL_SEC_MIC_LEN) != 0)
        return -1;
    r[ESL_OFF_VER] = ESL_PROTO_VER;
    return (int)len;
}

static void test_security(void)
{
    static esl_radio_sec_t sec = {{0}, false, sec_save, NULL};
    const uint64_t mac = 0x00000123456789A8ull, other = 0x00000123456789A9ull;
    uint8_t r[ESL_MAX_PAYLOAD], key[ESL_SEC_KEY_LEN], bad[ESL_SEC_KEY_LEN];
    char uri[64];

    ap_boot();
    for (int i = 0; i < ESL_SEC_KEY_LEN; i++)
        sec.key[i] = (uint8_t)(0xA0 + i);
    sec.require = false;
    esl_radio_set_sec(&sec);
    api.sec = &sec;

    /* without pairing a tag talks plainly, as before */
    CHECK_INT(sec_poll(mac, NULL, 0, r), ESL_IDLE_LEN);
    CHECK_INT(r[ESL_OFF_TYPE], ESL_MSG_IDLE);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/config", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"radio_secure\":false"));

    /* pairing: an unknown tag 404, then the key in a plain PAIR at its next POLL */
    snprintf(uri, sizeof(uri), "/api/tags/%016llX/pair", (unsigned long long)other);
    CHECK_INT(esl_api_handle(&api, "PUT", uri, AUTH, NULL, 0, 0, &out), 404);
    snprintf(uri, sizeof(uri), "/api/tags/%016llX/pair", (unsigned long long)mac);
    CHECK_INT(esl_api_handle(&api, "PUT", uri, AUTH, NULL, 0, 0, &out), 200);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A8", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"secure\":false,\"pairing\":\"pair\""));
    CHECK_INT(sec_poll(mac, NULL, 0, r), ESL_PAIR_LEN);
    CHECK_INT(r[ESL_OFF_TYPE], ESL_MSG_PAIR);
    CHECK_INT(r[ESL_OFF_NEXT_POLL], ESL_BLOCKS_POLL_S);
    esl_radio_tag_key(&sec, mac, key);
    CHECK(memcmp(r + ESL_PAIR_OFF_KEY, key, ESL_SEC_KEY_LEN) == 0);
    /* lost: sent again; then the tag's first message with the key ends the pairing */
    CHECK_INT(sec_poll(mac, NULL, 0, r), ESL_PAIR_LEN);
    CHECK_INT(sec_poll(mac, key, 0x10000, r), ESL_IDLE_LEN);
    CHECK_INT(r[ESL_OFF_TYPE], ESL_MSG_IDLE);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A8", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"secure\":true,\"pairing\":null,\"sec_rejected\":0"));

    /* the same counter again (a retry) is answered; an older one, a wrong MIC or another
       key are not, and are counted */
    CHECK_INT(sec_poll(mac, key, 0x10000, r), ESL_IDLE_LEN);
    CHECK_INT(sec_poll(mac, key, 0x10005, r), ESL_IDLE_LEN);
    CHECK_INT(sec_poll(mac, key, 0x10004, r), 0);
    memcpy(bad, key, sizeof(bad));
    bad[3] ^= 1;
    CHECK_INT(sec_poll(mac, bad, 0x10006, r), 0);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A8", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"sec_rejected\":2"));
    CHECK_INT(sec_poll(mac, key, 0x10006, r), ESL_IDLE_LEN);

    /* require: a plain tag gets nothing, the paired one goes on */
    CHECK_INT(sec_poll(other, NULL, 0, r), ESL_IDLE_LEN);
    int saves = sec_saves;
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/config", AUTH, "{\"radio_secure\":true}", 21, 0, &out), 200);
    CHECK(HAS(resp, "\"radio_secure\":true"));
    CHECK_INT(sec_saves, saves + 1);
    CHECK(sec.require);
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/config", AUTH, "{\"radio_secure\":1}", 18, 0, &out), 422);
    CHECK_INT(sec_poll(other, NULL, 0, r), 0);
    CHECK_INT(sec_poll(mac, key, 0x10007, r), ESL_IDLE_LEN);
    /* a plain frame in its name is dropped and does not make it look unpaired */
    CHECK_INT(sec_poll(mac, NULL, 0, r), 0);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A8", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"secure\":true"));
    /* ... except to pair it */
    snprintf(uri, sizeof(uri), "/api/tags/%016llX/pair", (unsigned long long)other);
    CHECK_INT(esl_api_handle(&api, "PUT", uri, AUTH, NULL, 0, 0, &out), 200);
    CHECK_INT(sec_poll(other, NULL, 0, r), ESL_PAIR_LEN);

    /* forget: an authenticated PAIR of zeros, until the tag talks plainly */
    snprintf(uri, sizeof(uri), "/api/tags/%016llX/pair", (unsigned long long)mac);
    CHECK_INT(esl_api_handle(&api, "DELETE", uri, AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"pairing\":\"forget\""));
    CHECK_INT(sec_poll(mac, key, 0x10008, r), ESL_PAIR_LEN);
    CHECK_INT(r[ESL_OFF_TYPE], ESL_MSG_PAIR);
    memset(bad, 0, sizeof(bad));
    CHECK(memcmp(r + ESL_PAIR_OFF_KEY, bad, ESL_SEC_KEY_LEN) == 0);
    CHECK_INT(sec_poll(mac, NULL, 0, r), 0);  /* plain, with require: no answer */
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A8", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"secure\":false,\"pairing\":null"));

    /* the key for the backup, and back */
    CHECK_INT(esl_api_handle(&api, "GET", "/api/radio-key", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "{\"key\":\"A0A1A2A3A4A5A6A7A8A9AAABACADAEAF\"}"));
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/radio-key", AUTH, "{\"key\":\"00\"}", 12, 0, &out), 422);
    const char *zero = "{\"key\":\"00000000000000000000000000000000\"}";
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/radio-key", AUTH, zero, strlen(zero), 0, &out), 422);
    const char *nk = "{\"key\":\"00112233445566778899aabbccddeeff\"}";
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/radio-key", AUTH, nk, strlen(nk), 0, &out), 200);
    CHECK(HAS(resp, "00112233445566778899AABBCCDDEEFF"));
    CHECK_INT(sec.key[15], 0xFF);
    CHECK_INT(sec_poll(other, key, 0x20000, r), 0);  /* a key from before: no answer */

    /* a network radio answers the tags itself and has no keys */
    esl_radio_link_t link = {0};
    api.link = &link;
    CHECK_INT(esl_api_handle(&api, "PUT", uri, AUTH, NULL, 0, 0, &out), 409);
    api.link = NULL;

    esl_radio_set_sec(NULL);
    api.sec = NULL;
    CHECK_INT(esl_api_handle(&api, "GET", "/api/radio-key", AUTH, NULL, 0, 0, &out), 404);
}

void test_radio(void)
{
    esl_sb_init(&out, resp, sizeof(resp));
    test_handle_filters();
    test_fake_tag_filters();
    test_end_to_end();
    test_config();
    test_many_tags();
    test_image_send();
    test_image_rle();
    test_picture();
    test_image_choice();
    test_image_2d();
    test_image_clock();
    test_draw_later();
    test_link();
    test_security();
}
