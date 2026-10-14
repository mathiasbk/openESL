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

static nvs_t nvs;
static esl_tag_t tags[CAP];
static esl_store_t st;
static esl_api_t api = {&st, "t", "0.1.0", 25, NULL};
static char resp[ESL_API_RESP_SIZE(CAP)];
static esl_sb_t out;

/* AP boot: empty table, then everything from fake NVS. */
static void ap_boot(void)
{
    static const esl_store_ops_t ops = {&nvs, NULL, NULL, nvs_save, nvs_erase};
    esl_store_init(&st, tags, CAP, &ops, 0);
    for (unsigned s = 0; s < CAP; s++) {
        uint64_t mac;
        esl_content_t c;
        if (nvs.used[s] && esl_persist_decode(nvs.blob[s], nvs.len[s], &mac, &c))
            esl_store_restore(&st, s, mac, &c);
    }
}

/* One round on the air. Returns the reply type the tag accepted (0 = none). */
static uint8_t air(esl_faketag_t *t, uint64_t now_ms, bool lose_reply)
{
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];
    size_t n = esl_faketag_poll(t, PAN, rx);
    size_t m = esl_radio_handle(&st, PAN, rx, n, -60, now_ms, tx, NULL);
    if (!m || lose_reply)
        return 0;
    return esl_faketag_receive(t, tx, m);
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
    esl_poll_t p = {1, 3000, 0, 0, ESL_MODEL_29};
    esl_radio_event_t ev;
    size_t n;

    ap_boot();
    esl_poll_build(payload, &p);

    /* a POLL for us: IDLE echoing the sequence number, tag discovered */
    n = esl_frame_build_from_tag(rx, 42, PAN, ESL_AP_SHORT_ADDR, 0x1122, payload, sizeof(payload));
    size_t m = esl_radio_handle(&st, PAN, rx, n, -70, 5000, tx, &ev);
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
    CHECK_INT(esl_radio_handle(&st, PAN, rx, n, 0, 0, tx, NULL), 0);
    n = esl_frame_build_from_tag(rx, 1, PAN, 0x0001, 0x3344, payload, sizeof(payload));
    CHECK_INT(esl_radio_handle(&st, PAN, rx, n, 0, 0, tx, NULL), 0);
    uint8_t idle[ESL_IDLE_LEN];
    esl_idle_build(idle, 0);
    n = esl_frame_build_from_tag(rx, 1, PAN, ESL_AP_SHORT_ADDR, 0x3344, idle, sizeof(idle));
    CHECK_INT(esl_radio_handle(&st, PAN, rx, n, 0, 0, tx, NULL), 0);
    n = esl_frame_build_from_tag(rx, 1, PAN, ESL_AP_SHORT_ADDR, 0x3344, payload, ESL_POLL_LEN - 1);
    CHECK_INT(esl_radio_handle(&st, PAN, rx, n, 0, 0, tx, NULL), 0);
    n = esl_frame_build_to_tag(rx, 1, PAN, ESL_AP_SHORT_ADDR, 0x3344, payload, sizeof(payload));
    CHECK_INT(esl_radio_handle(&st, PAN, rx, n, 0, 0, tx, NULL), 0);
    CHECK_INT(esl_radio_handle(&st, PAN, rx, 5, 0, 0, tx, NULL), 0);
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
    const char *body = "{\"layout\":\"price\",\"fields\":[\"Gr\\u00f8nnsaker\",\"19,90\",\"kr/kg\"],\"price_red\":true}";

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
    CHECK_INT(t.content.flags, ESL_CF_PRICE_RED);
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

    /* next_poll_s reaches the tag in both IDLE and CONTENT */
    esl_store_set_next_poll(&st, 300);
    CHECK_INT(air(&again, 3000, false), ESL_MSG_IDLE);
    CHECK_INT(again.next_poll_s, 300);
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

void test_radio(void)
{
    esl_sb_init(&out, resp, sizeof(resp));
    test_handle_filters();
    test_fake_tag_filters();
    test_end_to_end();
    test_many_tags();
}
