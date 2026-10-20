#include "tinytest.h"
#include "esl_api.h"
#include "esl_persist.h"
#include "esl_image.h"
#include "esl_link.h"
#include "esl_radio.h"
#include "esl_render.h"
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
} nvs_t;

static void nvs_save(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    nvs_t *n = ctx;
    n->len[slot] = esl_persist_encode(mac, c, n->blob[slot]);
    n->used[slot] = true;
    n->writes++;
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

static nvs_t nvs;
static esl_layouts_t layouts; /* empty unless a test uploads a file */
static esl_tag_t tags[CAP];
static esl_store_t st;
static esl_api_t api = {&st, "t", "0.1.0", 25, &layouts, NULL, NULL, NULL, NULL, NULL, NULL, false};
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
    static const esl_store_ops_t ops = {&nvs, NULL, NULL, nvs_save, nvs_erase, nvs_cfg, nvs_next_poll, NULL, NULL};
    esl_store_init(&st, tags, CAP, &ops, 0);
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
    esl_poll_t p = {1, 3000, 0, 0, ESL_MODEL_29, false, 0};
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
    CHECK(HAS(resp, "\"poll_s\":null,\"config\":{\"poll_s\":null,\"max_poll_s\":null,\"lost_ap_s\":null},"
                    "\"config_pending\":false"));

    /* per-tag settings: the tag gets CONFIG, uses the new interval and confirms */
    CHECK_INT(put_cfg("00000000C0FFEE01", "{\"poll_s\":120,\"lost_ap_s\":65535}"), 200);
    CHECK(HAS(resp, "\"poll_s\":120,\"config\":{\"poll_s\":120,\"max_poll_s\":null,\"lost_ap_s\":65535}"));
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
    a.img_rle = false; /* raw images: the block count below */
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
    CHECK_INT(esl_api_handle(&api, "PUT", "/api/tags/000000000000029A/image", AUTH, "{}", 2, 0, &out), 405);

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

    /* the file is deleted: the tag keeps the image it shows (it needs no file) */
    CHECK_INT(esl_api_handle(&api, "DELETE", "/api/layouts/2.9", AUTH, NULL, 0, 0, &out), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 301000), ESL_MSG_IDLE);
    CHECK_INT(a.img_shown, 4);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"pending\":false"));
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

    /* the file is back, with the answer to the POLL lost: the retry gets block 0 again */
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    uint32_t drawn = images.drawn;
    CHECK_INT(air(&a, 361000, true), 0);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 361000), ESL_MSG_IMG);
    CHECK_INT(images.drawn, drawn + 1);
    CHECK_INT(a.img_shown, 5);

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
        CHECK_INT(a.img_shown, 6);
        CHECK(a.shown == image_id_of("000000000000029A"));
    }
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 481000), ESL_MSG_IDLE);

    /* the panel fails: DRAWFAIL with the id, not offered again until the content changes */
    a.img_bad = true;
    CHECK_INT(put("000000000000029A", "{\"layout\":\"price\",\"fields\":[\"Smør\",\"49,90\"]}"), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 541000), ESL_MSG_IMG);
    CHECK_INT(a.img_shown, 6);
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
    CHECK_INT(a.img_shown, 7);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 781000), ESL_MSG_IDLE);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"draw_failed\":false"));

    /* the AP restarts: the tag shows its image, the AP draws it once to know the id and sends nothing */
    ap_boot();
    images_boot();
    drawn = images.drawn;
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 841000), ESL_MSG_IDLE);
    CHECK_INT(images.drawn, drawn + 1);
    CHECK_INT(a.img_shown, 7);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"pending\":false"));

    /* every buffer busy: the third tag is told to come back in ESL_IMAGE_BUSY_S */
    esl_faketag_t c, d;
    esl_faketag_init(&c, 0x29C, 1, 3000, ESL_MODEL_29);
    esl_faketag_init(&d, 0x29D, 1, 3000, ESL_MODEL_29);
    c.images = d.images = true;
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
    esl_tag_t t;
    CHECK(esl_store_get(&st, 0x29E, &t));
    CHECK_INT((int)esl_layouts_render(&layouts, ESL_MODEL_29, &t.content, raw, sizeof(raw)), (int)raw29);
    CHECK(memcmp(back, raw, raw29) == 0);
    printf("  RLE: 2.9\" shelf screen %zu -> %u bytes, %u blocks\n", raw29, (unsigned)images.buf[0].size,
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
} fake_link_t;

static int fake_send(esl_link_t *l, const uint8_t *psdu, size_t len, int64_t rx_us)
{
    fake_link_t *f = l->ctx;
    f->sends++;
    memcpy(f->last, psdu, len);
    f->last_len = len;
    f->last_rx_us = rx_us;
    return f->fail;
}

static int64_t fake_now_us;
static int64_t fake_clock(void)
{
    return fake_now_us;
}

static void test_link(void)
{
    fake_link_t fl = {0};
    esl_link_t l = {"fake", NULL, fake_send, NULL, &fl, {0}};
    esl_faketag_t t;
    esl_link_rx_t f = {0};
    uint8_t tx[ESL_PSDU_MAX];
    esl_radio_event_t ev;
    bool sent;

    ap_boot();
    esl_faketag_init(&t, 0x00000123456789A8ull, 1, 3000, ESL_MODEL_29);

    /* a POLL: answered through send, with the time the frame arrived */
    f.len = (uint8_t)esl_faketag_poll(&t, PAN, f.psdu);
    f.rssi = -60;
    f.rx_us = 1000;
    fake_now_us = 1800;
    size_t n = esl_link_handle(&l, &st, &layouts, PAN, &f, 5000, tx, &ev, fake_clock, &sent);
    CHECK(n > 0);
    CHECK(sent);
    CHECK_INT(fl.sends, 1);
    CHECK_INT(fl.last_len, n);
    CHECK_INT(fl.last_rx_us, 1000);
    CHECK_INT(ev.msg_type, ESL_MSG_POLL);
    CHECK(esl_faketag_receive(&t, fl.last, fl.last_len) != 0);
    CHECK_INT(l.stats.rx, 1);
    CHECK_INT(l.stats.polls, 1);
    CHECK_INT(l.stats.replies, 1);
    CHECK_INT(l.stats.last_us, 800);
    CHECK_INT(l.stats.worst_us, 800);

    /* a frame that needs no reply: counted as received, nothing sent */
    f.psdu[0] ^= 0xFF;
    fake_now_us = 9000;
    CHECK_INT(esl_link_handle(&l, &st, &layouts, PAN, &f, 5000, tx, NULL, fake_clock, &sent), 0);
    CHECK(!sent);
    CHECK_INT(fl.sends, 1);
    CHECK_INT(l.stats.rx, 2);
    CHECK_INT(l.stats.worst_us, 800);

    /* the link could not send: counted as failed, not as a reply */
    fl.fail = -1;
    f.len = (uint8_t)esl_faketag_poll(&t, PAN, f.psdu);
    fake_now_us = 1300;
    CHECK(esl_link_handle(&l, &st, &layouts, PAN, &f, 6000, tx, NULL, fake_clock, &sent) > 0);
    CHECK(!sent);
    CHECK_INT(l.stats.polls, 2);
    CHECK_INT(l.stats.replies, 1);
    CHECK_INT(l.stats.tx_failed, 1);
    CHECK_INT(l.stats.last_us, 300);
    CHECK_INT(l.stats.worst_us, 800);
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
    test_link();
}
