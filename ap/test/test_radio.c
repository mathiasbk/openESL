#include "tinytest.h"
#include "esl_api.h"
#include "esl_persist.h"
#include "esl_radio.h"
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
static esl_api_t api = {&st, "t", "0.1.0", 25, &layouts, NULL, NULL, NULL, NULL, NULL, NULL};
static char resp[ESL_API_RESP_SIZE(CAP)];
static esl_sb_t out;

/* AP boot: empty table, then everything from fake NVS. */
static void ap_boot(void)
{
    static const esl_store_ops_t ops = {&nvs, NULL, NULL, nvs_save, nvs_erase, nvs_cfg, nvs_next_poll};
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

/* One round on the air. Returns the reply type the tag accepted (0 = none). */
static uint8_t air(esl_faketag_t *t, uint64_t now_ms, bool lose_reply)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
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
    esl_poll_t p = {1, 3000, 0, 0, ESL_MODEL_29, false, 0, false, 0};
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

    /* reply parser: CONTENT with a field running past the end, or a bad layout */
    esl_reply_t r;
    CHECK(!esl_reply_parse((const uint8_t *)"\x03\x01\x00\x00\x01\x00\x00\x00\x01\x00\x05" "abc", 14, &r));
    CHECK(!esl_reply_parse((const uint8_t *)"\x03\x01\x00\x00\x01\x00\x00\x00\x07\x00", 10, &r));
    CHECK(!esl_reply_parse((const uint8_t *)"\x02\x01\x00\x00\x00", 5, &r));
    CHECK(!esl_reply_parse((const uint8_t *)"\x02\x02\x00\x00", 4, &r));
    CHECK(esl_reply_parse((const uint8_t *)"\x03\x01\x3C\x00\x01\x00\x00\x00\x02\x02\x02hi\x00", 14, &r));
    CHECK_INT(r.next_poll_s, 60);
    CHECK_INT(r.content.nfields, 2);
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

    /* client sets content: the next poll gets CONTENT, the one after IDLE */
    CHECK_INT(put("00000123456789A8", body), 200);
    CHECK(HAS(resp, "\"pending\":true"));
    CHECK_INT(air(&t, 61000, false), ESL_MSG_CONTENT);
    CHECK_INT(t.content.layout, ESL_LAYOUT_PRICE);
    CHECK_INT(t.content.flags, ESL_CF_PRICE_HIGHLIGHT);
    uint8_t len;
    const uint8_t *f = esl_content_field(&t.content, 0, &len);
    CHECK_INT(len, 10);
    CHECK_MEM(f, "Gr\xF8nnsaker", 10);
    CHECK_INT(air(&t, 121000, false), ESL_MSG_IDLE);
    CHECK_INT(put("00000123456789A8", body), 200);
    CHECK(HAS(resp, "\"changed\":false,\"pending\":false")); /* same content: no redraw */
    CHECK_INT(air(&t, 181000, false), ESL_MSG_IDLE);
    CHECK_INT(t.contents, 1);

    /* CONTENT lost on the air: the AP sends it again on the next poll */
    CHECK_INT(put("00000123456789A8", "{\"layout\":\"text\",\"fields\":[\"Stengt\"]}"), 200);
    CHECK_INT(air(&t, 241000, true), 0);
    CHECK_INT(air(&t, 301000, false), ESL_MSG_CONTENT);
    CHECK_INT(t.content.layout, ESL_LAYOUT_TEXT);
    CHECK_INT(t.contents, 2);

    /* AP loses power: content comes back from NVS, the tag gets no redraw */
    uint32_t shown = t.shown;
    ap_boot();
    CHECK_INT(esl_store_count(&st), 1);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A8", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"pending\":true")); /* not seen since boot */
    CHECK_INT(air(&t, 1000, false), ESL_MSG_IDLE);
    CHECK_INT(t.shown, shown);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000123456789A8", AUTH, NULL, 0, 2000, &out), 200);
    CHECK(HAS(resp, "\"pending\":false"));

    /* content set while the AP was off for the tag is still delivered after a reboot */
    CHECK_INT(put("00000123456789A8", "{\"layout\":\"text\",\"fields\":[\"\\u00c5pent\"]}"), 200);
    ap_boot();
    CHECK_INT(air(&t, 1000, false), ESL_MSG_CONTENT);
    CHECK_INT(t.content.text[0], 0xC5);

    /* tag restarts (shows nothing, flag booted): gets its content again */
    esl_faketag_t again;
    esl_faketag_init(&again, t.mac, 2, 2800, ESL_MODEL_29);
    CHECK_INT(air(&again, 2000, false), ESL_MSG_CONTENT);
    CHECK_INT(again.shown, t.shown);

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

    /* content goes first, the settings on the next poll; next_poll_s in CONTENT is the tag's own */
    CHECK_INT(put("00000000C0FFEE01", body), 200);
    CHECK_INT(put_cfg("00000000C0FFEE01", "{\"poll_s\":30}"), 200);
    CHECK_INT(air(&t, 331000, false), ESL_MSG_CONTENT);
    CHECK_INT(t.interval_s, 30);
    CHECK_INT(air(&t, 361000, false), ESL_MSG_CONFIG);
    CHECK_INT(t.cfg.lost_ap_s, 0);
    CHECK_INT(air(&t, 391000, false), ESL_MSG_IDLE);
    CHECK_INT(t.contents, 1);

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
    CHECK_INT(air(&again, 2000, false), ESL_MSG_CONTENT);
    CHECK_INT(air(&again, 3000, false), ESL_MSG_CONFIG);
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

    for (unsigned i = 0; i < CAP; i++)
        CHECK_INT(air(&t[i], 60000, false), ESL_MSG_CONTENT);
    for (unsigned i = 0; i < CAP; i++) {
        uint8_t len;
        const uint8_t *f = esl_content_field(&t[i].content, 0, &len);
        snprintf(body, sizeof(body), "Vare %u", i);
        CHECK_INT(len, strlen(body));
        CHECK_MEM(f, body, len);
        CHECK_INT(air(&t[i], 120000, false), ESL_MSG_IDLE);
    }
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags", AUTH, NULL, 0, 120000, &out), 200);
    CHECK(!HAS(resp, "\"pending\":true"));
    CHECK(HAS(resp, "\"model\":\"1.54\""));
}

/* ---- layout files over the air (docs/protokoll.md part 1, "Layout transfer") ---- */

char *read_example_layout(const char *name, size_t *len); /* test_layout.c */

static int put_layout(const char *model, const char *file, size_t len)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "/api/layouts/%s", model);
    return esl_api_handle(&api, "PUT", uri, AUTH, file, len, 0, &out);
}

static uint32_t version_of(uint8_t model)
{
    uint32_t v = 0;
    esl_layouts_info(&layouts, model, &v, NULL, NULL);
    return v;
}

/* One frame from the tag through the AP and back. Returns what the tag accepted. */
static uint8_t hop(esl_faketag_t *t, const uint8_t *rx, size_t n, bool lose)
{
    uint8_t tx[ESL_PSDU_MAX];
    size_t m = esl_radio_handle(&st, &layouts, PAN, rx, n, -60, 0, tx, NULL);
    return m && !lose ? esl_faketag_receive(t, tx, m) : 0;
}

static const char *const SHELF =
    "{\"layout\":\"shelf\",\"fields\":[\"Kaffe 500 g\",\"49,90\",\"kr 99,80/kg\",\"Tilbud\"],\"price_highlight\":true}";

static void test_layout_download(void)
{
    size_t l29, l154;
    char *f29 = read_example_layout("2.9.json", &l29);
    char *f154 = read_example_layout("1.54.json", &l154);
    CHECK(f29 && f154);
    if (!f29 || !f154)
        return;

    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_layouts_init(&layouts, NULL);
    esl_faketag_t a, b;
    esl_faketag_init(&a, 0x29A, 1, 3000, ESL_MODEL_29);
    esl_faketag_init(&b, 0x154B, 1, 3000, ESL_MODEL_154);

    /* no files: nothing to download, the tags work as before */
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 0), ESL_MSG_IDLE);
    CHECK_INT(a.layout_blocks, 0);

    /* a file for each model, and named content for both tags (krav A12, A14) */
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    CHECK_INT(put_layout("1.54", f154, l154), 200);
    CHECK_INT(put("000000000000029A", SHELF), 200);
    CHECK_INT(put("000000000000154B", SHELF), 200);
    uint32_t id = json_u32(resp, "\"content_id\":");

    /* one round: POLL -> LAYOUT@0, LAYOUT_REQ(94) -> LAYOUT@94, then POLL -> CONTENT */
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 0), ESL_MSG_CONTENT);
    CHECK_INT(a.layout_ver, version_of(ESL_MODEL_29));
    CHECK_INT(a.layout_size, 174);
    CHECK_INT(a.layout_blocks, 2);
    CHECK_INT(a.layout_installs, 1);
    CHECK_INT(a.shown, id);
    CHECK_INT(a.content.layout, ESL_LAYOUT_FILE + 0); /* "shelf" is layout 0 of the file */
    CHECK_INT(a.drawn_elements, 5);
    CHECK_INT(a.missing_layouts, 0);
    CHECK_INT(esl_faketag_round(&b, &st, &layouts, PAN, -60, 0), ESL_MSG_CONTENT);
    CHECK_INT(b.layout_ver, version_of(ESL_MODEL_154));
    CHECK(b.layout_ver != a.layout_ver);
    CHECK_INT(b.shown, id);
    CHECK_INT(b.drawn_elements, 5);

    /* the downloaded file is exactly the AP's binary for the model */
    esl_layout_bin_t bin;
    char err[ESL_LAYOUT_ERR_MAX];
    CHECK(esl_layout_compile(f29, l29, ESL_MODEL_29, &bin, err, sizeof(err)));
    CHECK_MEM(a.layout, bin.bin, bin.size);

    /* the same version is not sent again (krav T17) */
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 0), ESL_MSG_IDLE);
    CHECK_INT(a.layout_blocks, 2);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"layout_pending\":false,\"layout_failed\":false,\"content_error\":null"));

    /* another layout from the file */
    CHECK_INT(put("000000000000029A", "{\"layout\":\"sign\",\"fields\":[\"Velkommen\",\"\\u00c5pent 9-21\"],\"frame\":true}"),
              200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 0), ESL_MSG_CONTENT);
    CHECK_INT(a.content.layout, ESL_LAYOUT_FILE + 1);
    CHECK_INT(a.drawn_elements, 7);
    CHECK_INT(a.content.flags, ESL_CF_FRAME);

    /* a changed file: downloaded, and the content drawn with the old one is sent again in the same round */
    uint32_t old = a.layout_ver;
    uint32_t contents = a.contents;
    char *f2 = malloc(l29 + 1);
    memcpy(f2, f29, l29);
    char *band = strstr(f2, "\"h\": 30, \"color\": \"black\"");
    CHECK(band != NULL);
    if (band)
        band[6] = '2'; /* the black band 20 px instead of 30 */
    CHECK_INT(put_layout("2.9", f2, l29), 200);
    CHECK(version_of(ESL_MODEL_29) != old);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 0), ESL_MSG_CONTENT);
    CHECK_INT(a.layout_ver, version_of(ESL_MODEL_29));
    CHECK_INT(a.contents, contents + 1);
    CHECK_INT(a.content.layout, ESL_LAYOUT_FILE + 1);

    /* built-in content is not sent again when the file changes */
    CHECK_INT(put("000000000000029A", "{\"layout\":\"price\",\"fields\":[\"Melk\",\"24,90\"]}"), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 0), ESL_MSG_CONTENT);
    contents = a.contents;
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 0), ESL_MSG_IDLE);
    CHECK_INT(a.layout_ver, old);
    CHECK_INT(a.contents, contents);

    /* the tag restarts: the file is not in its flash yet, so it fetches it again, then the content */
    esl_faketag_init(&a, 0x29A, 1, 3000, ESL_MODEL_29);
    CHECK_INT(put("000000000000029A", SHELF), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 0), ESL_MSG_CONTENT);
    CHECK_INT(a.layout_ver, old);
    CHECK_INT(a.drawn_elements, 5);

    /* the file is deleted: the tag drops its copy, named content cannot be sent */
    CHECK_INT(esl_api_handle(&api, "DELETE", "/api/layouts/2.9", AUTH, NULL, 0, 0, &out), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 0), ESL_MSG_IDLE);
    CHECK_INT(a.layout_ver, 0);
    CHECK_INT(a.shown, 0); /* it asks for the content again, resolved against no file */
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000029A", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"content_error\":\"the layout is not in the layout file for this tag's model\""));
    CHECK(HAS(resp, "\"pending\":true"));
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    CHECK_INT(esl_faketag_round(&a, &st, &layouts, PAN, -60, 0), ESL_MSG_CONTENT);
    CHECK_INT(a.shown, id);

    free(f2);
    free(f29);
    free(f154);
}

static void test_layout_lost_and_changed(void)
{
    size_t l29;
    char *f29 = read_example_layout("2.9.json", &l29);
    CHECK(f29 != NULL);
    if (!f29)
        return;
    uint8_t rx[ESL_PSDU_MAX];
    esl_faketag_t t;

    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_layouts_init(&layouts, NULL);
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    uint32_t v1 = version_of(ESL_MODEL_29);
    esl_faketag_init(&t, 0x77, 1, 3000, ESL_MODEL_29);

    /* block 0 arrives, the answer to LAYOUT_REQ is lost: the round ends */
    CHECK_INT(hop(&t, rx, esl_faketag_poll(&t, PAN, rx), false), ESL_MSG_LAYOUT);
    CHECK_INT(t.dl_got, ESL_LAYOUT_CHUNK_MAX);
    CHECK_INT(hop(&t, rx, esl_faketag_request(&t, PAN, rx), true), 0);
    CHECK_INT(t.layout_ver, 0);

    /* next round: the AP offers block 0 again, the tag carries on at 94 */
    CHECK_INT(hop(&t, rx, esl_faketag_poll(&t, PAN, rx), false), ESL_MSG_LAYOUT);
    CHECK_INT(t.dl_got, ESL_LAYOUT_CHUNK_MAX);
    size_t n = esl_faketag_request(&t, PAN, rx);
    esl_rx_frame_t f;
    esl_layout_req_t req;
    CHECK(esl_frame_parse_from_tag(rx, n, &f) && esl_layout_req_parse(f.payload, f.payload_len, &req));
    CHECK_INT(req.pos, ESL_LAYOUT_CHUNK_MAX);
    CHECK_INT(req.layout_ver, v1);
    CHECK_INT(req.model, ESL_MODEL_29);
    CHECK_INT(req.max, ESL_LAYOUT_CHUNK_MAX);

    /* the file changes before the tag asks: the AP answers with block 0 of the new file */
    char *f2 = malloc(l29);
    memcpy(f2, f29, l29);
    char *x = strstr(f2, "\"x\": 120");
    CHECK(x != NULL);
    if (x)
        x[7] = '5'; /* price box at x 125 */
    CHECK_INT(put_layout("2.9", f2, l29), 200);
    uint32_t v2 = version_of(ESL_MODEL_29);
    CHECK(v2 != v1);
    CHECK_INT(hop(&t, rx, n, false), ESL_MSG_LAYOUT);
    CHECK_INT(t.dl_ver, v2);
    CHECK_INT(t.dl_got, ESL_LAYOUT_CHUNK_MAX);
    CHECK_INT(hop(&t, rx, esl_faketag_request(&t, PAN, rx), false), ESL_MSG_LAYOUT);
    CHECK_INT(t.layout_ver, v2);
    CHECK_INT(t.layout_installs, 1);
    CHECK_INT(esl_faketag_request(&t, PAN, rx), 0);

    /* a LAYOUT_REQ that is not for this AP's PAN is ignored */
    esl_layout_req_t r2 = {ESL_MODEL_29, 94, v2, 0};
    uint8_t pl[ESL_LREQ_LEN], tx[ESL_PSDU_MAX];
    esl_layout_req_build(pl, &r2);
    n = esl_frame_build_from_tag(rx, 1, 0x1234, ESL_AP_SHORT_ADDR, 0x77, pl, sizeof(pl));
    CHECK_INT(esl_radio_handle(&st, &layouts, PAN, rx, n, 0, 0, tx, NULL), 0);
    esl_radio_event_t ev;
    n = esl_frame_build_from_tag(rx, 9, PAN, ESL_AP_SHORT_ADDR, 0x77, pl, sizeof(pl));
    CHECK(esl_radio_handle(&st, &layouts, PAN, rx, n, 0, 0, tx, &ev) > 0);
    CHECK_INT(ev.msg_type, ESL_MSG_LAYOUT_REQ);
    CHECK_INT(ev.reply_type, ESL_MSG_LAYOUT);
    CHECK_INT(ev.req.layout_ver, v2);
    CHECK(ev.mac == 0x77);

    free(f2);
    free(f29);
}

static void test_layout_rejected_and_old_firmware(void)
{
    size_t l29;
    char *f29 = read_example_layout("2.9.json", &l29);
    CHECK(f29 != NULL);
    if (!f29)
        return;
    esl_faketag_t t, old;

    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_layouts_init(&layouts, NULL);
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    uint32_t v1 = version_of(ESL_MODEL_29);

    /* firmware without layout files (POLL without the layout version): never a LAYOUT,
     * built-in content as before */
    esl_faketag_init(&old, 0x01D, 1, 3000, ESL_MODEL_29);
    old.layouts = false;
    CHECK_INT(put("000000000000001D", SHELF), 200); /* model unknown yet: accepted */
    CHECK_INT(esl_faketag_round(&old, &st, &layouts, PAN, -60, 0), ESL_MSG_IDLE);
    CHECK_INT(old.layout_blocks, 0);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/000000000000001D", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"layout_version\":null"));
    CHECK(HAS(resp, "\"content_error\":\"the tag firmware cannot download layout files\""));
    CHECK_INT(put("000000000000001D", SHELF), 422); /* now the model is known */
    CHECK_INT(put("000000000000001D", "{\"layout\":\"price\",\"fields\":[\"Melk\",\"24,90\"]}"), 200);
    CHECK_INT(esl_faketag_round(&old, &st, &layouts, PAN, -60, 0), ESL_MSG_CONTENT);
    CHECK_INT(old.content.layout, ESL_LAYOUT_PRICE);

    /* a tag that cannot use the file says so (POLL flag); the AP stops offering that version */
    esl_faketag_init(&t, 0x8AD, 1, 3000, ESL_MODEL_29);
    CHECK_INT(put("00000000000008AD", SHELF), 200);
    uint8_t rx[ESL_PSDU_MAX];
    CHECK_INT(hop(&t, rx, esl_faketag_poll(&t, PAN, rx), false), ESL_MSG_LAYOUT); /* offered v1 */
    t.dl_ver = 0;
    t.layout_rejected = v1; /* as if the CRC or the structure check had failed */
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 0), ESL_MSG_IDLE);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 0), ESL_MSG_IDLE);
    CHECK_INT(t.layout_blocks, 1);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000000000008AD", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"layout_failed\":true,\"content_error\":\"the tag rejected the layout file\""));
    /* built-in content still gets through */
    CHECK_INT(put("00000000000008AD", "{\"layout\":\"text\",\"fields\":[\"Hei\"]}"), 200);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 0), ESL_MSG_CONTENT);
    /* a new version is offered again, and this time it works */
    char *f2 = malloc(l29);
    memcpy(f2, f29, l29);
    char *x = strstr(f2, "\"x\": 120");
    if (x)
        x[7] = '5';
    CHECK_INT(put_layout("2.9", f2, l29), 200);
    CHECK_INT(put("00000000000008AD", SHELF), 200);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 0), ESL_MSG_CONTENT);
    CHECK_INT(t.layout_ver, version_of(ESL_MODEL_29));
    CHECK_INT(t.layout_rejected, 0);
    CHECK_INT(t.drawn_elements, 5);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/00000000000008AD", AUTH, NULL, 0, 0, &out), 200);
    CHECK(HAS(resp, "\"layout_failed\":false"));

    free(f2);
    free(f29);
}

static void test_layout_many_tags(void)
{
    static esl_faketag_t t[CAP];
    size_t l29, l154;
    char *f29 = read_example_layout("2.9.json", &l29);
    char *f154 = read_example_layout("1.54.json", &l154);
    CHECK(f29 && f154);
    if (!f29 || !f154)
        return;

    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_layouts_init(&layouts, NULL);
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    CHECK_INT(put_layout("1.54", f154, l154), 200);
    for (unsigned i = 0; i < CAP; i++) {
        char mac[17];
        esl_faketag_init(&t[i], 0xE000 + i, 1, 3000, i % 2 ? ESL_MODEL_154 : ESL_MODEL_29);
        esl_mac_format(t[i].mac, mac);
        CHECK_INT(put(mac, i % 3 ? SHELF : "{\"layout\":\"sign\",\"fields\":[\"A\",\"B\"]}"), 200);
    }
    for (unsigned i = 0; i < CAP; i++) {
        CHECK_INT(esl_faketag_round(&t[i], &st, &layouts, PAN, -60, 0), ESL_MSG_CONTENT);
        CHECK_INT(t[i].layout_ver, version_of(t[i].model));
        CHECK_INT(t[i].drawn_elements, i % 3 ? 5 : 7);
        CHECK_INT(esl_faketag_round(&t[i], &st, &layouts, PAN, -60, 60000), ESL_MSG_IDLE);
    }
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags", AUTH, NULL, 0, 60000, &out), 200);
    CHECK(!HAS(resp, "\"pending\":true"));
    CHECK(!HAS(resp, "\"layout_pending\":true"));
    free(f29);
    free(f154);
}

/* Layout, content and CONFIG for the same tag (docs/protokoll.md part 1,
 * "Priority"): the AP sends the layout file first (the content may use it),
 * then the content, then the settings, one message per POLL. After a layout
 * download the tag polls again at once, so the next item follows in the same
 * wake-up. */
static void test_layout_content_config(void)
{
    size_t l29;
    char *f29 = read_example_layout("2.9.json", &l29);
    CHECK(f29 != NULL);
    if (!f29)
        return;
    esl_faketag_t t, cfgonly;

    memset(&nvs, 0, sizeof(nvs));
    ap_boot();
    esl_layouts_init(&layouts, NULL);
    esl_faketag_init(&t, 0xA11, 1, 3000, ESL_MODEL_29);

    /* everything waiting before the tag is ever seen */
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    CHECK_INT(put("0000000000000A11", SHELF), 200);
    uint32_t id = json_u32(resp, "\"content_id\":");
    CHECK_INT(put_cfg("0000000000000A11", "{\"poll_s\":120,\"lost_ap_s\":600}"), 200);

    /* round 1: LAYOUT blocks, then the extra POLL gets CONTENT; every reply,
     * the LAYOUT blocks too, already carries the tag's own interval */
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 1000), ESL_MSG_CONTENT);
    CHECK_INT(t.layout_ver, version_of(ESL_MODEL_29));
    CHECK_INT(t.layout_blocks, 2);
    CHECK_INT(t.shown, id);
    CHECK_INT(t.drawn_elements, 5);
    CHECK_INT(t.configs, 0);
    CHECK_INT(t.interval_s, 120);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/0000000000000A11", AUTH, NULL, 0, 1000, &out), 200);
    CHECK(HAS(resp, "\"pending\":true,")); /* confirmed by the next POLL */
    CHECK(HAS(resp, "\"config_pending\":true,\"layout_version\":"));
    CHECK(HAS(resp, "\"layout_pending\":false,"));

    /* round 2: CONFIG; round 3: nothing left */
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 121000), ESL_MSG_CONFIG);
    CHECK_INT(t.cfg.poll_s, 120);
    CHECK_INT(t.cfg.lost_ap_s, 600);
    CHECK(t.cfg_ver != 0);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 241000), ESL_MSG_IDLE);
    CHECK_INT(t.configs, 1);
    CHECK_INT(t.contents, 1);
    CHECK_INT(t.layout_installs, 1);
    CHECK_INT(t.rejected, 0);

    /* all three change at once: layout + content in one wake-up, CONFIG in the next */
    char *f2 = malloc(l29);
    memcpy(f2, f29, l29);
    char *band = strstr(f2, "\"h\": 30, \"color\": \"black\"");
    CHECK(band != NULL);
    if (band)
        band[6] = '2';
    CHECK_INT(put_layout("2.9", f2, l29), 200);
    CHECK_INT(put("0000000000000A11", "{\"layout\":\"sign\",\"fields\":[\"Velkommen\",\"Hei\"]}"), 200);
    CHECK_INT(put_cfg("0000000000000A11", "{\"poll_s\":45}"), 200);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 300000), ESL_MSG_CONTENT);
    CHECK_INT(t.layout_ver, version_of(ESL_MODEL_29));
    CHECK_INT(t.layout_installs, 2);
    CHECK_INT(t.content.layout, ESL_LAYOUT_FILE + 1);
    CHECK_INT(t.interval_s, 45);
    CHECK_INT(t.configs, 1);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 345000), ESL_MSG_CONFIG);
    CHECK_INT(t.cfg.poll_s, 45);
    CHECK_INT(t.cfg.lost_ap_s, 0);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 390000), ESL_MSG_IDLE);

    /* layout and settings, content unchanged (built-in layout): the extra POLL
     * after the download gets CONFIG in the same wake-up */
    CHECK_INT(put("0000000000000A11", "{\"layout\":\"price\",\"fields\":[\"Melk\",\"24,90\"]}"), 200);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 400000), ESL_MSG_CONTENT);
    uint32_t contents = t.contents;
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    CHECK_INT(put_cfg("0000000000000A11", "{\"poll_s\":300}"), 200);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 450000), ESL_MSG_CONFIG);
    CHECK_INT(t.layout_ver, version_of(ESL_MODEL_29));
    CHECK_INT(t.contents, contents);
    CHECK_INT(t.interval_s, 300);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 750000), ESL_MSG_IDLE);

    /* content and settings, no layout change: CONTENT first, CONFIG next */
    CHECK_INT(put("0000000000000A11", SHELF), 200);
    CHECK_INT(put_cfg("0000000000000A11", "{\"poll_s\":60}"), 200);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 800000), ESL_MSG_CONTENT);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 860000), ESL_MSG_CONFIG);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 920000), ESL_MSG_IDLE);
    CHECK_INT(t.rejected, 0);

    /* firmware with CONFIG but without layout files (14-byte POLL): CONFIG, never LAYOUT */
    esl_faketag_init(&cfgonly, 0xC0F, 1, 3000, ESL_MODEL_29);
    cfgonly.layouts = false;
    CHECK_INT(put_cfg("0000000000000C0F", "{\"poll_s\":90}"), 200);
    CHECK_INT(esl_faketag_round(&cfgonly, &st, &layouts, PAN, -60, 1000), ESL_MSG_CONFIG);
    CHECK_INT(esl_faketag_round(&cfgonly, &st, &layouts, PAN, -60, 91000), ESL_MSG_IDLE);
    CHECK_INT(cfgonly.layout_blocks, 0);
    CHECK_INT(cfgonly.interval_s, 90);
    CHECK_INT(cfgonly.rejected, 0);

    /* the AP restarts: content and settings come back from NVS, the layout file
     * is uploaded again; a tag that has everything gets IDLE */
    ap_boot();
    esl_layouts_init(&layouts, NULL);
    CHECK_INT(put_layout("2.9", f29, l29), 200);
    CHECK_INT(esl_faketag_round(&t, &st, &layouts, PAN, -60, 1000), ESL_MSG_IDLE);
    CHECK_INT(t.interval_s, 60);

    free(f2);
    free(f29);
}

void test_radio(void)
{
    esl_sb_init(&out, resp, sizeof(resp));
    test_handle_filters();
    test_fake_tag_filters();
    test_end_to_end();
    test_config();
    test_many_tags();
    test_layout_download();
    test_layout_lost_and_changed();
    test_layout_rejected_and_old_firmware();
    test_layout_many_tags();
    test_layout_content_config();
}
