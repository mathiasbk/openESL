#include "tinytest.h"
#include "esl_api.h"
#include <stdlib.h>

#define CAP 8
#define AUTH "Bearer s3cret"
#define MAC1 "00000123456789A8"

static esl_store_t st;
static esl_layouts_t layouts;
static char persisted_url[ESL_WEBHOOK_URL_MAX + 1];
static int persisted;

static void persist_url(void *ctx, const char *url)
{
    (void)ctx;
    strcpy(persisted_url, url);
    persisted++;
}

static esl_notify_t notify = {"", NULL, persist_url};
static esl_api_t api = {&st, "s3cret", "0.1.0", 25, &layouts, NULL, NULL, NULL, &notify, NULL, NULL, false, NULL, NULL, NULL, NULL};
static char resp[ESL_API_RESP_SIZE(CAP)];
static esl_sb_t out;

static int req(const char *method, const char *uri, const char *auth, const char *body, uint64_t now_ms)
{
    return esl_api_handle(&api, method, uri, auth, body, body ? strlen(body) : 0, now_ms, &out);
}

static uint32_t json_u32(const char *s, const char *key)
{
    const char *p = strstr(s, key);
    return p ? (uint32_t)strtoul(p + strlen(key), NULL, 10) : 0;
}

/* The tag shows the image of content id shown: here the image id is the content id,
   as if the AP had drawn it (esl_image does that on the radio path) */
static void drawn(uint64_t mac, uint32_t shown)
{
    esl_tag_t t;
    if (shown && esl_store_get(&st, mac, &t) && t.has_content && t.content_id == shown)
        esl_store_image_known(&st, mac, shown, 0, shown);
}

static void poll_as_tag(uint64_t mac, uint32_t shown, uint64_t now)
{
    uint8_t reply[ESL_MAX_PAYLOAD];
    esl_poll_t p = {2, 2950, shown, ESL_POLL_F_IMAGE | ESL_POLL_F_IMG_RLE, ESL_MODEL_29, false, 0};
    drawn(mac, shown);
    esl_store_on_poll(&st, mac, &p, -61, now, NULL, reply);
}

static void test_auth_and_routing(void)
{
    CHECK_INT(req("GET", "/api/tags", NULL, NULL, 0), 401);
    CHECK_STR(resp, "{\"error\":\"unauthorized\"}");
    CHECK_INT(req("GET", "/api/tags", "Bearer wrong", NULL, 0), 401);
    CHECK_INT(req("GET", "/api/tags", "Bearer s3cre", NULL, 0), 401);
    CHECK_INT(req("GET", "/api/tags", "Bearer s3cret2", NULL, 0), 401);
    CHECK_INT(req("GET", "/api/tags", "Basic s3cret", NULL, 0), 401);
    CHECK_INT(req("GET", "/api/tags", "bearer s3cret", NULL, 0), 200);
    CHECK_INT(req("GET", "/api/tags?x=1", AUTH, NULL, 0), 200);
    CHECK_INT(req("GET", "/api/tags/", AUTH, NULL, 0), 200);

    /* an empty token locks the API instead of opening it */
    esl_api_t open = api;
    open.token = "";
    CHECK_INT(esl_api_handle(&open, "GET", "/api/tags", "Bearer ", NULL, 0, 0, &out), 401);

    CHECK_INT(req("GET", "/", AUTH, NULL, 0), 404);
    CHECK_INT(req("GET", "/api/other", AUTH, NULL, 0), 404);
    CHECK_INT(req("GET", "/api/tagsX", AUTH, NULL, 0), 404);
    CHECK_INT(req("GET", "/api/tags/123", AUTH, NULL, 0), 400);
    CHECK_INT(req("GET", "/api/tags/00000123456789AZ", AUTH, NULL, 0), 400);
    CHECK_INT(req("DELETE", "/api/tags", AUTH, NULL, 0), 405);
    CHECK_INT(req("POST", "/api/tags/" MAC1, AUTH, "{}", 0), 405);
    CHECK_INT(req("GET", "/api/tags/" MAC1, AUTH, NULL, 0), 404);
    CHECK_STR(resp, "{\"error\":\"unknown tag\"}");
}

static void test_put_one(void)
{
    const char *body = "{ \"layout\": \"price\", \"fields\": [\"Lettmelk 1 L\", \"24,90\", \"kr/l 24,90\"], \"price_highlight\": true }";

    CHECK_INT(req("PUT", "/api/tags/" MAC1, AUTH, body, 1000), 200);
    CHECK(HAS(resp, "\"mac\":\"" MAC1 "\""));
    CHECK(HAS(resp, "\"changed\":true"));
    CHECK(HAS(resp, "\"pending\":true"));
    uint32_t id = json_u32(resp, "\"content_id\":");
    CHECK(id != 0);

    /* same content again (lower-case MAC in the URL is the same tag) */
    CHECK_INT(req("PUT", "/api/tags/00000123456789a8", AUTH, body, 2000), 200);
    CHECK(HAS(resp, "\"changed\":false"));
    CHECK_INT(json_u32(resp, "\"content_id\":"), id);

    /* tag fetches it, then status shows it as shown */
    poll_as_tag(0x00000123456789A8ull, 0, 3000);
    poll_as_tag(0x00000123456789A8ull, id, 5000);
    CHECK_INT(req("PUT", "/api/tags/" MAC1, AUTH, body, 6000), 200);
    CHECK(HAS(resp, "\"changed\":false,\"pending\":false"));

    CHECK_INT(req("GET", "/api/tags/" MAC1 "/", AUTH, NULL, 17000), 200);
    char expect[768];
    snprintf(expect, sizeof(expect),
             "{\"mac\":\"" MAC1 "\",\"seen_s_ago\":12,\"battery_mv\":2950,\"rssi\":-61,\"fw\":2,"
             "\"model\":\"2.9\",\"draw_failed\":false,\"content_id\":%u,\"shown_content_id\":%u,\"pending\":false,"
             "\"poll_s\":null,\"config\":{\"poll_s\":null,\"max_poll_s\":null,\"lost_ap_s\":null},\"config_pending\":false,"
             "\"content_error\":null,\"ota\":false,"
             "\"image\":true,\"image_id\":%u,\"shown_image_id\":%u,\"fw_update\":null,\"layout\":\"price\",\"fields\":[\"Lettmelk 1 L\",\"24,90\",\"kr/l 24,90\"],"
             "\"price_highlight\":true,\"frame\":false}",
             (unsigned)id, (unsigned)id, (unsigned)id, (unsigned)id);
    CHECK_STR(resp, expect);

    /* price_highlight is part of the content */
    CHECK_INT(req("PUT", "/api/tags/" MAC1, AUTH,
                  "{\"layout\":\"price\",\"fields\":[\"Lettmelk 1 L\",\"24,90\",\"kr/l 24,90\"]}", 18000), 200);
    CHECK(HAS(resp, "\"changed\":true"));
    CHECK(json_u32(resp, "\"content_id\":") != id);

    /* text layout with UTF-8 and \u escapes; trailing empty fields trimmed */
    CHECK_INT(req("PUT", "/api/tags/0000000000000002", AUTH,
                  "{\"layout\":\"text\",\"fields\":[\"Velkommen\",\"\xC3\x85pent 9\xE2\x80\x93" "21\",\"\\u00e6\\u00f8\\u00e5\",\"\"]}", 0), 200);
    uint32_t id2 = json_u32(resp, "\"content_id\":");
    CHECK_INT(req("PUT", "/api/tags/0000000000000002", AUTH,
                  "{\"layout\":\"text\",\"fields\":[\"Velkommen\",\"\\u00c5pent 9-21\",\"\xC3\xA6\xC3\xB8\xC3\xA5\"]}", 0), 200);
    CHECK(HAS(resp, "\"changed\":false"));
    CHECK_INT(json_u32(resp, "\"content_id\":"), id2);
    CHECK_INT(req("GET", "/api/tags/0000000000000002", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"fields\":[\"Velkommen\",\"\xC3\x85pent 9-21\",\"\xC3\xA6\xC3\xB8\xC3\xA5\"]"));
    CHECK(HAS(resp, "\"seen_s_ago\":null"));
    CHECK(HAS(resp, "\"model\":null,\"draw_failed\":false,"));
    CHECK(HAS(resp, "\"shown_content_id\":null"));

    /* model unknown (old firmware sends 0) and a failed drawing */
    uint8_t reply[ESL_MAX_PAYLOAD];
    esl_poll_t p = {1, 2400, 0, ESL_POLL_FLAG_DRAW_FAILED, ESL_MODEL_UNKNOWN, false, 0};
    esl_store_on_poll(&st, 2, &p, -70, 1000, NULL, reply);
    CHECK_INT(req("GET", "/api/tags/0000000000000002", AUTH, NULL, 1000), 200);
    CHECK(HAS(resp, "\"fw\":1,\"model\":null,\"draw_failed\":true,"));
    p.flags = 0;
    p.model = ESL_MODEL_154;
    esl_store_on_poll(&st, 2, &p, -70, 2000, NULL, reply);
    CHECK_INT(req("GET", "/api/tags/0000000000000002", AUTH, NULL, 2000), 200);
    CHECK(HAS(resp, "\"model\":\"1.54\",\"draw_failed\":false,"));

    /* battery 0 = the tag could not measure it; the API passes it on as 0 */
    p.battery_mv = 0;
    esl_store_on_poll(&st, 2, &p, -70, 3000, NULL, reply);
    CHECK_INT(req("GET", "/api/tags/0000000000000002", AUTH, NULL, 3000), 200);
    CHECK(HAS(resp, "\"seen_s_ago\":0,\"battery_mv\":0,\"rssi\":-70,\"fw\":1,"));

    /* a POLL handled after the request read the clock: just now, not 2^64 ms ago */
    esl_store_on_poll(&st, 2, &p, -70, 4000, NULL, reply);
    CHECK_INT(req("GET", "/api/tags/0000000000000002", AUTH, NULL, 3999), 200);
    CHECK(HAS(resp, "\"seen_s_ago\":0,"));
    CHECK_INT(req("GET", "/api/health", AUTH, NULL, 3999), 200);
    CHECK(!HAS(resp, "\"silent\"]"));
}

static void test_put_errors(void)
{
    const char *u = "/api/tags/0000000000000003";
    CHECK_INT(req("PUT", u, AUTH, NULL, 0), 400);
    CHECK_INT(req("PUT", u, AUTH, "{\"layout\":", 0), 400);
    CHECK_STR(resp, "{\"error\":\"invalid JSON\"}");
    CHECK_INT(req("PUT", u, AUTH, "[]", 0), 400);
    CHECK_INT(req("PUT", u, AUTH, "{\"layout\":\"image\",\"fields\":[]}", 0), 422);
    CHECK_INT(req("PUT", u, AUTH, "{\"layout\":\"price\"}", 0), 422);
    CHECK_INT(req("PUT", u, AUTH, "{\"layout\":\"price\",\"fields\":\"x\"}", 0), 422);
    CHECK_INT(req("PUT", u, AUTH, "{\"layout\":\"price\",\"fields\":[1]}", 0), 422);
    CHECK_INT(req("PUT", u, AUTH, "{\"layout\":\"price\",\"fields\":[\"a\",\"b\",\"c\",\"d\"]}", 0), 422);
    CHECK_INT(req("PUT", u, AUTH, "{\"layout\":\"text\",\"fields\":[\"a\",\"b\",\"c\",\"d\",\"e\"]}", 0), 422);
    CHECK_INT(req("PUT", u, AUTH, "{\"layout\":\"price\",\"fields\":[\"a\"],\"price_highlight\":\"yes\"}", 0), 422);

    /* text and one byte per field share 96 bytes: 95 bytes of Latin-1 in one field;
       each Latin-1 character (e.g. æ) is one byte */
    char body[512];
    char text[200];
    memset(text, 'x', 95);
    text[95] = '\0';
    snprintf(body, sizeof(body), "{\"layout\":\"text\",\"fields\":[\"%s\"]}", text);
    CHECK_INT(req("PUT", u, AUTH, body, 0), 200);
    /* empty fields at the end cost nothing */
    snprintf(body, sizeof(body), "{\"layout\":\"text\",\"fields\":[\"%s\",\"\",null,\"\"]}", text);
    CHECK_INT(req("PUT", u, AUTH, body, 0), 200);
    text[95] = 'x';
    text[96] = '\0';
    snprintf(body, sizeof(body), "{\"layout\":\"text\",\"fields\":[\"%s\"]}", text);
    CHECK_INT(req("PUT", u, AUTH, body, 0), 422);
    CHECK(HAS(resp, "too long"));
    /* three fields: 93 bytes */
    snprintf(body, sizeof(body), "{\"layout\":\"text\",\"fields\":[\"%.45s\",\"%.47s\",\"\xC3\xA6\"]}", text, text);
    CHECK_INT(req("PUT", u, AUTH, body, 0), 200);
    snprintf(body, sizeof(body), "{\"layout\":\"text\",\"fields\":[\"%.46s\",\"%.47s\",\"\xC3\xA6\"]}", text, text);
    CHECK_INT(req("PUT", u, AUTH, body, 0), 422);
    /* very long UTF-8 input does not overflow anything */
    char huge[2048];
    memset(huge, 'y', sizeof(huge));
    memcpy(huge, "{\"layout\":\"text\",\"fields\":[\"", 28);
    memcpy(&huge[sizeof(huge) - 5], "\"]}", 4);
    CHECK_INT(req("PUT", u, AUTH, huge, 0), 422);

    char *big = malloc(ESL_API_MAX_BODY + 2);
    memset(big, ' ', ESL_API_MAX_BODY + 1);
    big[0] = '{';
    big[ESL_API_MAX_BODY] = '}';
    big[ESL_API_MAX_BODY + 1] = '\0';
    CHECK_INT(req("PUT", u, AUTH, big, 0), 413);
    free(big);
}

static void test_auth_lockout(void)
{
    /* The static api has no lock (lock NULL): wrong tokens are only 401 */
    for (int i = 0; i < 2 * ESL_API_AUTH_TRIES; i++)
        CHECK_INT(req("GET", "/api/tags", "Bearer wrong", NULL, 1000), 401);
    CHECK_INT(req("GET", "/api/tags", AUTH, NULL, 1000), 200);

    esl_api_lock_t lock = {0, 0};
    esl_api_t locked = api;
    locked.lock = &lock;
    uint64_t t = 10 * 1000;
    for (int i = 0; i < ESL_API_AUTH_TRIES; i++) {
        CHECK_INT(esl_api_handle(&locked, "GET", "/api/tags", "Bearer wrong", NULL, 0, t, &out), 401);
        CHECK_STR(resp, "{\"error\":\"unauthorized\"}");
    }
    /* the fifth wrong token locks: even the right one is refused for 60 s */
    CHECK_INT(esl_api_handle(&locked, "GET", "/api/tags", AUTH, NULL, 0, t, &out), 429);
    CHECK_STR(resp, "{\"error\":\"too many wrong tokens: locked for 60 s\"}");
    CHECK_INT(esl_api_handle(&locked, "GET", "/api/tags", "Bearer wrong", NULL, 0, t + 30 * 1000, &out), 429);
    CHECK_STR(resp, "{\"error\":\"too many wrong tokens: locked for 30 s\"}");
    CHECK_INT(esl_api_handle(&locked, "GET", "/api/tags", AUTH, NULL, 0, t + 59 * 1000 + 1, &out), 429);
    CHECK_STR(resp, "{\"error\":\"too many wrong tokens: locked for 1 s\"}");
    /* wrong tokens while locked do not extend it */
    CHECK_INT(esl_api_handle(&locked, "GET", "/api/tags", AUTH, NULL, 0, t + 60 * 1000, &out), 200);
    CHECK(lock.fails == 0);

    /* a right token resets the count */
    t += 120 * 1000;
    for (int i = 0; i < ESL_API_AUTH_TRIES - 1; i++)
        CHECK_INT(esl_api_handle(&locked, "GET", "/api/tags", "Bearer wrong", NULL, 0, t, &out), 401);
    CHECK_INT(esl_api_handle(&locked, "GET", "/api/tags", AUTH, NULL, 0, t, &out), 200);
    for (int i = 0; i < ESL_API_AUTH_TRIES - 1; i++)
        CHECK_INT(esl_api_handle(&locked, "GET", "/api/tags", "Bearer wrong", NULL, 0, t, &out), 401);
    CHECK_INT(esl_api_handle(&locked, "GET", "/api/tags", AUTH, NULL, 0, t, &out), 200);
    /* a missing header counts like a wrong token; a request outside /api does not */
    for (int i = 0; i < ESL_API_AUTH_TRIES; i++)
        CHECK_INT(esl_api_handle(&locked, "GET", "/", NULL, NULL, 0, t, &out), 404);
    for (int i = 0; i < ESL_API_AUTH_TRIES; i++)
        CHECK_INT(esl_api_handle(&locked, "GET", "/api/health", NULL, NULL, 0, t, &out), 401);
    CHECK_INT(esl_api_handle(&locked, "DELETE", "/api/tags/0000000000002000", AUTH, NULL, 0, t, &out), 429);

    /* esl_api_auth_check() for transports that check the token themselves (OTA) */
    unsigned wait_s = 7;
    CHECK_INT(esl_api_auth_check(&locked, AUTH, t + 60 * 1000, &wait_s), 0);
    CHECK_INT(esl_api_auth_check(&locked, "Bearer wrong", t + 60 * 1000, &wait_s), 401);
    CHECK(wait_s == 0);
    CHECK_INT(esl_api_auth_check(&api, "Bearer wrong", t, &wait_s), 401);
}

static void test_batch_and_list(void)
{
    char body[8192];
    esl_sb_t sb;

    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);

    CHECK_INT(req("PUT", "/api/tags", AUTH,
                  "{\"tags\":[{\"mac\":\"0000000000000001\",\"layout\":\"price\",\"fields\":[\"A\",\"1,00\"]},"
                  "{\"mac\":\"0000000000000002\",\"layout\":\"text\",\"fields\":[\"B\"],\"frame\":true}]}", 0), 200);
    CHECK(HAS(resp, "{\"tags\":[{\"mac\":\"0000000000000001\",\"content_id\":"));
    CHECK(HAS(resp, "},{\"mac\":\"0000000000000002\",\"content_id\":"));
    CHECK_INT(esl_store_count(&st), 2);

    /* a bad entry rejects the whole batch and names it */
    CHECK_INT(req("PUT", "/api/tags", AUTH,
                  "{\"tags\":[{\"mac\":\"0000000000000003\",\"layout\":\"price\",\"fields\":[\"C\"]},"
                  "{\"mac\":\"0000000000000004\",\"layout\":\"nope\",\"fields\":[]}]}", 0), 422);
    CHECK(HAS(resp, "\"index\":1"));
    CHECK_INT(esl_store_count(&st), 2);
    CHECK_INT(req("PUT", "/api/tags", AUTH, "{\"tags\":[{\"mac\":\"xyz\",\"layout\":\"price\",\"fields\":[]}]}", 0), 422);
    CHECK(HAS(resp, "\"index\":0"));
    CHECK_INT(req("PUT", "/api/tags", AUTH, "{\"tags\":[42]}", 0), 422);
    CHECK_INT(req("PUT", "/api/tags", AUTH, "{\"items\":[]}", 0), 422);
    CHECK_INT(req("PUT", "/api/tags", AUTH, "{\"tags\":[]}", 0), 200);
    CHECK_STR(resp, "{\"tags\":[]}");

    /* more new tags than free slots: 507 and nothing applied */
    esl_sb_init(&sb, body, sizeof(body));
    esl_sb_puts(&sb, "{\"tags\":[");
    for (int i = 0; i < CAP - 1; i++)
        esl_sb_printf(&sb, "%s{\"mac\":\"%016X\",\"layout\":\"text\",\"fields\":[\"t\"]}", i ? "," : "", 0x100 + i);
    esl_sb_puts(&sb, "]}");
    CHECK_INT(req("PUT", "/api/tags", AUTH, body, 0), 507);
    CHECK_INT(esl_store_count(&st), 2);

    /* exactly enough room (duplicates count once) */
    esl_sb_init(&sb, body, sizeof(body));
    esl_sb_puts(&sb, "{\"tags\":[");
    for (int i = 0; i < CAP - 2; i++)
        esl_sb_printf(&sb, "{\"mac\":\"%016X\",\"layout\":\"text\",\"fields\":[\"t\"]},", 0x100 + i);
    esl_sb_puts(&sb, "{\"mac\":\"0000000000000100\",\"layout\":\"text\",\"fields\":[\"u\"]}]}");
    CHECK_INT(req("PUT", "/api/tags", AUTH, body, 0), 200);
    CHECK_INT(esl_store_count(&st), CAP);

    /* more than 50 entries */
    esl_sb_init(&sb, body, sizeof(body));
    esl_sb_puts(&sb, "{\"tags\":[");
    for (int i = 0; i < ESL_API_MAX_BATCH + 1; i++)
        esl_sb_printf(&sb, "%s{\"mac\":\"0000000000000001\",\"layout\":\"text\",\"fields\":[]}", i ? "," : "");
    esl_sb_puts(&sb, "]}");
    CHECK(!sb.overflow);
    CHECK_INT(req("PUT", "/api/tags", AUTH, body, 0), 422);

    /* a full batch of 50 known tags fits the token pool */
    esl_sb_init(&sb, body, sizeof(body));
    esl_sb_puts(&sb, "{\"tags\":[");
    for (int i = 0; i < ESL_API_MAX_BATCH; i++)
        esl_sb_printf(&sb, "%s{\"mac\":\"0000000000000001\",\"layout\":\"price\",\"fields\":[\"Navn %d\",\"%d,90\",\"kr/kg\"],\"price_highlight\":false}",
                      i ? "," : "", i, i);
    esl_sb_puts(&sb, "]}");
    CHECK(!sb.overflow);
    CHECK_INT(req("PUT", "/api/tags", AUTH, body, 0), 200);

    /* list: a discovered tag shows up with content_id 0 */
    esl_store_delete(&st, 0x105);
    poll_as_tag(0xABCDEF, 0, 1000);
    CHECK_INT(req("GET", "/api/tags", AUTH, NULL, 86400000), 200);
    CHECK(HAS(resp, "{\"ap\":{\"fw\":\"0.1.0\",\"board\":\"host\",\"board_name\":\"host\",\"uptime_s\":86400,\"channel\":25,\"max_tags\":8,\"table_full\":false,\"poll_s\":0},\"tags\":["));
    /* the network radio: its address and when it was last heard; no "radio" without a link */
    esl_radio_link_t link = {.up = true, .ip = 0xC0A803F9, .seen_ms = 86395000, .ver = 3};
    api.link = &link;
    CHECK_INT(req("GET", "/api/tags", AUTH, NULL, 86400000), 200);
    CHECK(HAS(resp, "\"poll_s\":0,\"radio\":{\"connected\":true,\"ip\":\"192.168.3.249\",\"seen_s\":5,\"fw\":3}},\"tags\":["));
    link.up = false;
    CHECK_INT(req("GET", "/api/tags", AUTH, NULL, 86400000), 200);
    CHECK(HAS(resp, "\"poll_s\":0,\"radio\":{\"connected\":false}},\"tags\":["));
    /* a last-heard time ahead of now (a race with the radio's task) reads as just now */
    link.up = true;
    link.seen_ms = 86400001;
    CHECK_INT(req("GET", "/api/tags", AUTH, NULL, 86400000), 200);
    CHECK(HAS(resp, "\"radio\":{\"connected\":true,\"ip\":\"192.168.3.249\",\"seen_s\":0,\"fw\":3}},\"tags\":["));
    api.link = NULL;
    api.reset_reason = "task_wdt";
    CHECK_INT(req("GET", "/api/tags", AUTH, NULL, 86400000), 200);
    CHECK(HAS(resp, "\"poll_s\":0,\"reset\":\"task_wdt\"},\"tags\":["));
    api.reset_reason = NULL;
    CHECK(HAS(resp, "{\"mac\":\"0000000000ABCDEF\",\"seen_s_ago\":86399,\"battery_mv\":2950,\"rssi\":-61,\"fw\":2,"
                    "\"model\":\"2.9\",\"draw_failed\":false,\"content_id\":0,\"shown_content_id\":0,\"pending\":false,"
                    "\"poll_s\":null,\"config\":{\"poll_s\":null,\"max_poll_s\":null,\"lost_ap_s\":null},"
                    "\"config_pending\":false,"
                    "\"content_error\":null,\"ota\":false,"
                    "\"image\":true,\"image_id\":null,\"shown_image_id\":0,\"fw_update\":null}"));
    CHECK(!HAS(resp, "\"layout\"")); /* list has no content */

    /* delete */
    CHECK_INT(req("DELETE", "/api/tags/0000000000ABCDEF", AUTH, NULL, 0), 200);
    CHECK_STR(resp, "{\"mac\":\"0000000000ABCDEF\",\"deleted\":true}");
    CHECK_INT(req("DELETE", "/api/tags/0000000000ABCDEF", AUTH, NULL, 0), 404);

    /* response that does not fit the buffer -> 500, not truncated JSON */
    char small[64];
    esl_sb_t tiny;
    esl_sb_init(&tiny, small, sizeof(small));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags", AUTH, NULL, 0, 0, &tiny), 500);
    CHECK_STR(small, "{\"error\":\"response too large\"}");
}

char *read_example_layout(const char *name, size_t *len); /* test_layout.c */

/* A tag that polls with model, showing images or not (no SPI flash). */
static void poll_model(uint64_t mac, uint8_t model, bool images)
{
    uint8_t reply[ESL_MAX_PAYLOAD];
    esl_layout_env_t env;
    esl_poll_t p = {2, 2950, 0, images ? ESL_POLL_F_IMAGE : 0, model, true, 0};
    esl_layouts_env(&layouts, model, &env);
    esl_store_on_poll(&st, mac, &p, -61, 1000, &env, reply);
}

static void test_layouts(void)
{
    size_t len29, len154;
    char *f29 = read_example_layout("2.9.json", &len29);
    char *f154 = read_example_layout("1.54.json", &len154);
    CHECK(f29 && f154);
    if (!f29 || !f154)
        return;
    f29[len29] = f154[len154] = '\0';
    esl_layouts_init(&layouts, NULL);
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);

    CHECK_INT(req("GET", "/api/layouts", NULL, NULL, 0), 401);
    CHECK_INT(req("GET", "/api/layouts", AUTH, NULL, 0), 200);
    CHECK_STR(resp, "{\"max_size\":8192,\"max_binary_size\":4096,\"board\":\"host\",\"limits\":{\"runs\":32,"
                    "\"rects\":20,\"lines\":16,\"icons\":8,\"barcodes\":1,\"qrs\":1,\"fileBytes\":4096,"
                    "\"jsonBytes\":8192,\"fields\":6,\"fixedTextBytes\":64},\"layouts\":["
                    "{\"model\":\"2.9\",\"version\":null,\"size\":0,\"binary_size\":0,\"layouts\":[]},"
                    "{\"model\":\"1.54\",\"version\":null,\"size\":0,\"binary_size\":0,\"layouts\":[]}]}");
    CHECK_INT(req("GET", "/api/layouts/", AUTH, NULL, 0), 200);
    CHECK_INT(req("PUT", "/api/layouts", AUTH, f29, 0), 405);
    CHECK_INT(req("GET", "/api/layouts/2.9", AUTH, NULL, 0), 404);
    CHECK_INT(req("GET", "/api/layouts/4.2", AUTH, NULL, 0), 404);
    CHECK(HAS(resp, "unknown model"));
    CHECK_INT(req("GET", "/api/layoutsX", AUTH, NULL, 0), 404);

    /* without a file only price and text exist */
    CHECK_INT(req("PUT", "/api/tags/" MAC1, AUTH, "{\"layout\":\"shelf\",\"fields\":[\"a\"]}", 0), 422);
    CHECK_STR(resp, "{\"error\":\"layout \\\"shelf\\\" is not in any layout file\"}");

    /* upload, read back, same file again is not a change */
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, f29, 0), 200);
    CHECK(HAS(resp, "{\"model\":\"2.9\",\"version\":"));
    CHECK(HAS(resp, "\"layouts\":[\"shelf\",\"sign\"],\"changed\":true,\"content_errors\":[],\"content_errors_total\":0}"));
    uint32_t v = json_u32(resp, "\"version\":");
    CHECK(v != 0);
    CHECK_INT(json_u32(resp, "\"size\":"), len29);
    CHECK_INT(json_u32(resp, "\"binary_size\":"), 175);
    CHECK_INT(req("PUT", "/api/layouts/2.9/", AUTH, f29, 0), 200);
    CHECK(HAS(resp, "\"changed\":false"));
    CHECK_INT(json_u32(resp, "\"version\":"), v);
    CHECK_INT(req("GET", "/api/layouts/2.9", AUTH, NULL, 0), 200);
    CHECK_STR(resp, f29);
    CHECK_INT(req("GET", "/api/layouts", AUTH, NULL, 0), 200);
    char expect[256];
    snprintf(expect, sizeof(expect),
             "{\"model\":\"2.9\",\"version\":%u,\"size\":%u,\"binary_size\":175,\"layouts\":[\"shelf\",\"sign\"]}",
             (unsigned)v, (unsigned)len29);
    CHECK(HAS(resp, expect));
    CHECK(HAS(resp, "{\"model\":\"1.54\",\"version\":null,\"size\":0,\"binary_size\":0,\"layouts\":[]}"));

    /* the file is checked against the model in the URL (krav A13): 422 with where and why */
    CHECK_INT(req("PUT", "/api/layouts/1.54", AUTH, f29, 0), 422);
    CHECK_STR(resp, "{\"error\":\"model must be \\\"1.54\\\": the file is checked against that screen\"}");
    CHECK_INT(req("PUT", "/api/layouts/1.54", AUTH, f154, 0), 200);
    CHECK(json_u32(resp, "\"version\":") != v);
    CHECK_INT(req("GET", "/api/layouts/2.9", AUTH, NULL, 0), 200);
    CHECK_STR(resp, f29);
    const char *wide = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"1.54\",\"layouts\":[{\"name\":\"a\","
                       "\"fields\":[\"t\"],\"elements\":[{\"type\":\"text\",\"field\":\"t\",\"x\":0,\"y\":0,"
                       "\"w\":296,\"h\":30,\"fonts\":[\"small\"]}]}]}";
    CHECK_INT(req("PUT", "/api/layouts/1.54", AUTH, wide, 0), 422);
    CHECK_STR(resp, "{\"error\":\"layouts[0].elements[0]: x + w = 296 is past the right edge of the screen (200)\"}");

    /* content naming a layout (krav A14). Model not known yet: any model's file will do */
    CHECK_INT(req("PUT", "/api/tags/" MAC1, AUTH,
                  "{\"layout\":\"shelf\",\"fields\":[\"Kaffe\",\"49,90\",\"kr 99,80/kg\",\"Tilbud\"],"
                  "\"price_highlight\":true}", 0), 200);
    uint32_t id = json_u32(resp, "\"content_id\":");
    CHECK_INT(req("GET", "/api/tags/" MAC1, AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"layout\":\"shelf\",\"fields\":[\"Kaffe\",\"49,90\",\"kr 99,80/kg\",\"Tilbud\"]"));
    CHECK_INT(req("PUT", "/api/tags/" MAC1, AUTH, "{\"layout\":\"sign\",\"fields\":[\"a\",\"b\",\"c\"]}", 0), 422);
    CHECK_STR(resp, "{\"error\":\"layout sign takes at most 2 fields\"}");
    CHECK_INT(req("PUT", "/api/tags/" MAC1, AUTH, "{\"layout\":\"Shelf\",\"fields\":[]}", 0), 422);
    CHECK(HAS(resp, "the name of a layout in a layout file"));
    /* the same name on both models is the same content_id */
    CHECK_INT(req("PUT", "/api/tags/0000000000000154", AUTH,
                  "{\"layout\":\"shelf\",\"fields\":[\"Kaffe\",\"49,90\",\"kr 99,80/kg\",\"Tilbud\"],"
                  "\"price_highlight\":true}", 0), 200);
    CHECK_INT(json_u32(resp, "\"content_id\":"), id);

    /* model known: only that model's file counts */
    const char *only29 = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"only-29\","
                         "\"fields\":[\"t\"],\"elements\":[{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":2,\"h\":2}]}]}";
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, only29, 0), 200);
    poll_model(0x154, ESL_MODEL_154, true);
    CHECK_INT(req("PUT", "/api/tags/0000000000000154", AUTH, "{\"layout\":\"only-29\",\"fields\":[]}", 0), 422);
    CHECK_STR(resp, "{\"error\":\"layout \\\"only-29\\\" is not in the layout file for model 1.54\"}");
    CHECK_INT(req("PUT", "/api/tags/0000000000000154", AUTH, "{\"layout\":\"sign\",\"fields\":[\"x\"]}", 0), 200);
    /* a tag without images takes the content, and says why it does not show it */
    poll_model(0x29, ESL_MODEL_29, false);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"only-29\",\"fields\":[]}", 0), 200);
    poll_model(0x29, ESL_MODEL_29, false);
    CHECK_INT(req("GET", "/api/tags/0000000000000029", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"content_error\":\"the tag cannot show images drawn by the AP (old firmware, or no SPI flash)\""));
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"price\",\"fields\":[]}", 0), 200);
    /* batch: the row is named */
    CHECK_INT(req("PUT", "/api/tags", AUTH,
                  "{\"tags\":[{\"mac\":\"0000000000000154\",\"layout\":\"sign\",\"fields\":[\"a\"]},"
                  "{\"mac\":\"0000000000000154\",\"layout\":\"only-29\",\"fields\":[]}]}", 0), 422);
    CHECK(HAS(resp, "\"index\":1"));

    /* six fields, e.g. the price with and without VAT next to brand, name and info */
    const char *six = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"six\","
                      "\"fields\":[\"brand\",\"name\",\"info\",\"price\",\"price_ex\",\"unit\"],\"elements\":["
                      "{\"type\":\"text\",\"field\":\"price_ex\",\"x\":0,\"y\":0,\"w\":100,\"h\":20,\"fonts\":[\"small\"]},"
                      "{\"type\":\"text\",\"field\":5,\"x\":0,\"y\":20,\"w\":100,\"h\":20,\"fonts\":[\"small\"]}]}]}";
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, six, 0), 200);
    poll_model(0x290, ESL_MODEL_29, true);
    CHECK_INT(req("PUT", "/api/tags/0000000000000290", AUTH,
                  "{\"layout\":\"six\",\"fields\":[\"Kramp\",\"Spann\",\"1626\",\"192,25\",\"153,80\",\"stk\"]}", 0), 200);
    CHECK_INT(req("GET", "/api/tags/0000000000000290", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"153,80\",\"stk\"]"));
    CHECK_INT(req("PUT", "/api/tags/0000000000000290", AUTH,
                  "{\"layout\":\"six\",\"fields\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\"]}", 0), 422);
    CHECK(HAS(resp, "at most 6 fields"));

    /* a barcode field must be one the tag can draw */
    const char *bc29 = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"bc\","
                       "\"fields\":[\"name\",\"ean\"],\"elements\":[{\"type\":\"barcode\",\"field\":\"ean\","
                       "\"symbology\":\"ean13\",\"x\":0,\"y\":0,\"w\":150,\"h\":40}]},{\"name\":\"c128\","
                       "\"fields\":[\"code\"],\"elements\":[{\"type\":\"barcode\",\"field\":\"code\","
                       "\"symbology\":\"code128\",\"x\":0,\"y\":0,\"w\":100,\"h\":40}]}]}";
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, bc29, 0), 200);
    poll_model(0x29, ESL_MODEL_29, true);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"bc\",\"fields\":[\"x\",\"590123412345\"]}", 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"bc\",\"fields\":[\"x\",\"5901234123457\"]}", 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"bc\",\"fields\":[\"x\"]}", 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"bc\",\"fields\":[\"x\",\"5901234123458\"]}", 0), 422);
    CHECK_STR(resp, "{\"error\":\"field 1 of layout bc is a ean13 barcode: it takes 12 digits, or 13 with a correct check digit\"}");
    /* 100 px with 1-pixel modules: Code 128 of 5 letters is 11 * 7 + 13 + 20 = 110 modules */
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"c128\",\"fields\":[\"ABCD\"]}", 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"c128\",\"fields\":[\"ABCDE\"]}", 0), 422);
    CHECK(HAS(resp, "too long for its barcode on model 2.9 (110 of 100 modules)"));
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"c128\",\"fields\":[\"12345678\"]}", 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"c128\",\"fields\":[\"bl\u00e5\"]}", 0), 422);
    CHECK(HAS(resp, "ASCII characters only"));
    /* the model not known yet: checked against every file with the layout */
    CHECK_INT(req("PUT", "/api/tags/00000000000000AA", AUTH, "{\"layout\":\"c128\",\"fields\":[\"ABCDE\"]}", 0), 422);

    /* a new file is checked against the content the tags have (#186): accepted, but the tags
       it no longer fits are listed and their content is held back until it fits again */
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"c128\",\"fields\":[\"ABCD\"]}", 0), 200);
    char small29[600];
    snprintf(small29, sizeof(small29), "%.*s60%s", (int)(strstr(bc29, "\"w\":100") + 4 - bc29), bc29,
             strstr(bc29, "\"w\":100") + 7);
    CHECK(HAS(small29, "\"symbology\":\"code128\",\"x\":0,\"y\":0,\"w\":60,"));
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, small29, 0), 200);
    CHECK(HAS(resp, "\"changed\":true,\"content_errors\":[{\"mac\":\"0000000000000029\",\"error\":"
                    "\"field 0 of layout c128 is too long for its barcode on model 2.9 (99 of 60 modules)\"}],"
                    "\"content_errors_total\":1}"));
    CHECK_INT(req("GET", "/api/tags/0000000000000029", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"content_error\":\"a barcode, QR code or number does not fit its box in the layout file for "
                    "this tag's model\""));
    poll_model(0x29, ESL_MODEL_29, true); /* a POLL keeps it */
    CHECK_INT(req("GET", "/api/tags/0000000000000029", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "does not fit its box"));
    {
        esl_tag_t t;
        CHECK(esl_store_get(&st, 0x29, &t) && t.content_err == ESL_CERR_UNFIT);
    }
    /* the same file again lists it again; the old box fits it, and the mark is gone */
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, small29, 0), 200);
    CHECK(HAS(resp, "\"changed\":false,\"content_errors\":[{\"mac\":\"0000000000000029\""));
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, bc29, 0), 200);
    CHECK(HAS(resp, "\"content_errors\":[],\"content_errors_total\":0}"));
    CHECK_INT(req("GET", "/api/tags/0000000000000029", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"content_error\":null"));
    /* new content clears it too (PUT /api/tags checks it against the file) */
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, small29, 0), 200);
    CHECK_INT(json_u32(resp, "\"content_errors_total\":"), 1);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"bc\",\"fields\":[\"x\"]}", 0), 200);
    CHECK_INT(req("GET", "/api/tags/0000000000000029", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"content_error\":null"));
    /* content set without PUT /api/tags (as the schedule does) or before the tag's model was
       known is checked when the tag polls (esl_store_check_tag, from esl_radio_handle) */
    {
        esl_content_t c;
        esl_set_result_t r;
        CHECK(esl_content_init_named(&c, "c128", 4, 0) == ESL_CONTENT_OK);
        CHECK(esl_content_add_field(&c, (const uint8_t *)"WXYZ", 4) == ESL_CONTENT_OK);
        CHECK(esl_store_set(&st, 0x29, &c, &r) == ESL_STORE_OK);
        CHECK(!esl_store_check_tag(&st, &layouts, 0x29));
        CHECK_INT(req("GET", "/api/tags/0000000000000029", AUTH, NULL, 0), 200);
        CHECK(HAS(resp, "does not fit its box"));
        CHECK(esl_store_check_tag(&st, &layouts, 0x29)); /* checked once: nothing to do */
        /* a tag whose model is not known yet when the file comes */
        CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, bc29, 0), 200);
        CHECK_INT(req("PUT", "/api/tags/00000000000000BB", AUTH, "{\"layout\":\"c128\",\"fields\":[\"ABCD\"]}", 0), 200);
        CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, small29, 0), 200);
        CHECK(!HAS(resp, "00000000000000BB"));
        poll_model(0xBB, ESL_MODEL_29, true);
        CHECK(!esl_store_check_tag(&st, &layouts, 0xBB));
        CHECK_INT(req("GET", "/api/tags/00000000000000BB", AUTH, NULL, 0), 200);
        CHECK(HAS(resp, "does not fit its box"));
        /* another reason it has is not hidden by this one */
        poll_model(0xBB, ESL_MODEL_29, false);
        CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, small29, 0), 200);
        CHECK_INT(req("GET", "/api/tags/00000000000000BB", AUTH, NULL, 0), 200);
        CHECK(HAS(resp, "\"content_error\":\"the tag cannot show images"));
        CHECK_INT(req("DELETE", "/api/tags/00000000000000BB", AUTH, NULL, 0), 200);
    }
    /* deleting the file: not in the file any more, and not unfit; back to a file that fits */
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, bc29, 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"c128\",\"fields\":[\"ABCD\"]}", 0), 200);
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, small29, 0), 200);
    CHECK_INT(json_u32(resp, "\"content_errors_total\":"), 1);
    CHECK_INT(req("DELETE", "/api/layouts/2.9", AUTH, NULL, 0), 200);
    CHECK_INT(req("GET", "/api/tags/0000000000000029", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"content_error\":null")); /* the file is gone: nothing is unfit */
    poll_model(0x29, ESL_MODEL_29, true); /* and at the POLL: not in the file */
    CHECK_INT(req("GET", "/api/tags/0000000000000029", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"content_error\":\"the layout is not in the layout file for this tag's model\""));
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, bc29, 0), 200);
    CHECK_INT(json_u32(resp, "\"content_errors_total\":"), 0);
    poll_model(0x29, ESL_MODEL_29, true);
    CHECK_INT(req("GET", "/api/tags/0000000000000029", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"content_error\":null"));

    /* a QR field must fit its box: 34 px at H holds version 3 (29 modules and 2 x 2 of quiet zone), 24 bytes */
    const char *qr29 = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"qr\","
                       "\"fields\":[\"url\"],\"elements\":[{\"type\":\"qr\",\"field\":\"url\",\"ecc\":\"H\","
                       "\"x\":0,\"y\":0,\"w\":34,\"h\":34}]}]}";
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, qr29, 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"qr\",\"fields\":[\"https://a.example/12345\"]}", 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"qr\",\"fields\":[\"https://a.example/123456\"]}", 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"qr\",\"fields\":[\"https://a.example/1234567\"]}", 0), 422);
    CHECK_STR(resp, "{\"error\":\"field 0 of layout qr is too long for its QR code on model 2.9 (25 of 24 bytes)\"}");

    /* a formatted number must fit in one of its fonts: a cut price would be a wrong one */
    const char *num29 = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"pr\","
                        "\"fields\":[\"price\"],\"elements\":[{\"type\":\"text\",\"field\":\"price\",\"x\":0,\"y\":0,"
                        "\"w\":110,\"h\":60,\"fonts\":[\"large\",\"mid\"],\"decimals\":2,\"thousands\":true}]}]}";
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, num29, 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"pr\",\"fields\":[\"9.9\"]}", 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"pr\",\"fields\":[\"1299.9\"]}", 0), 422);
    CHECK_STR(resp, "{\"error\":\"a number in layout pr does not fit in its box on model 2.9, not even in its smallest font\"}");

    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, f29, 0), 200);

    /* named content the tag cannot get yet says why */
    CHECK_INT(req("PUT", "/api/tags/0000000000000029", AUTH, "{\"layout\":\"sign\",\"fields\":[\"x\"]}", 0), 200);
    CHECK_INT(req("DELETE", "/api/layouts/2.9", AUTH, NULL, 0), 200);
    poll_model(0x29, ESL_MODEL_29, true);
    CHECK_INT(req("GET", "/api/tags/0000000000000029", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"content_error\":\"the layout is not in the layout file for this tag's model\""));
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, f29, 0), 200);

    /* validation */
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, NULL, 0), 400);
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, "{\"a\":", 0), 400);
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, "[1,2]", 0), 400);
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, "\"x\"", 0), 400);
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, "{\"name\":\"price-2.9\"}", 0), 422);
    CHECK_STR(resp, "{\"error\":\"unknown key \\\"name\\\" (keys starting with _ are ignored)\"}");
    CHECK_INT(req("PUT", "/api/layouts/3.0", AUTH, f29, 0), 404);
    CHECK_INT(req("POST", "/api/layouts/2.9", AUTH, f29, 0), 405);

    /* the biggest file, with as many tokens as possible, is parsed (and then checked) */
    static char big[ESL_LAYOUT_MAX_SIZE + 2];
    size_t o = 0;
    big[o++] = '[';
    while (o < ESL_LAYOUT_MAX_SIZE - 3) {
        big[o++] = '1';
        big[o++] = ',';
    }
    big[o++] = '1';
    big[o++] = ']';
    big[o] = '\0';
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, big, 0), 400); /* an array, not an object */
    memset(big, ' ', ESL_LAYOUT_MAX_SIZE);
    memcpy(big, f154, len154);
    big[ESL_LAYOUT_MAX_SIZE] = '\0';
    CHECK_INT(req("PUT", "/api/layouts/1.54", AUTH, big, 0), 200);
    CHECK_INT(req("GET", "/api/layouts/1.54", AUTH, NULL, 0), 200);
    CHECK_STR(resp, big);
    big[ESL_LAYOUT_MAX_SIZE] = ' ';
    big[ESL_LAYOUT_MAX_SIZE + 1] = '\0';
    CHECK_INT(req("PUT", "/api/layouts/1.54", AUTH, big, 0), 413);

    /* delete */
    CHECK_INT(req("DELETE", "/api/layouts/2.9", AUTH, NULL, 0), 200);
    CHECK_STR(resp, "{\"model\":\"2.9\",\"deleted\":true}");
    CHECK_INT(req("DELETE", "/api/layouts/2.9", AUTH, NULL, 0), 404);
    CHECK_INT(req("GET", "/api/layouts/2.9", AUTH, NULL, 0), 404);

    /* without a layout store the endpoints do not exist, and only price/text are layouts */
    esl_api_t no_layouts = api;
    no_layouts.layouts = NULL;
    CHECK_INT(esl_api_handle(&no_layouts, "GET", "/api/layouts", AUTH, NULL, 0, 0, &out), 404);
    const char *named = "{\"layout\":\"sign\",\"fields\":[]}";
    CHECK_INT(esl_api_handle(&no_layouts, "PUT", "/api/tags/" MAC1, AUTH, named, strlen(named), 0, &out), 422);
    free(f29);
    free(f154);
}

static void test_status_page(void)
{
    static char page[ESL_API_PAGE_SIZE(CAP)];
    esl_sb_t sb;
    esl_sb_init(&sb, page, sizeof(page));
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);

    CHECK_INT(req("PUT", "/api/tags/" MAC1, AUTH,
                  "{\"layout\":\"price\",\"fields\":[\"Br\u00f8d <b>&amp;\",\"9,90\"]}", 0), 200);
    poll_as_tag(0xABCDEF, 0, 1000);
    CHECK_INT(esl_api_status_page(&api, 1000 + 3 * 3600 * 1000, &sb), 200);
    CHECK(HAS(page, "<!DOCTYPE html>"));
    CHECK(HAS(page, "Firmware 0.1.0, up 3 h, channel 25, 2 of 8 tags, <span class=\"p\">2 need attention</span>."));
    CHECK(HAS(page, "<td>" MAC1 "</td><td>-</td><td>-</td>"));
    CHECK(HAS(page, "price: Br\xC3\xB8" "d &lt;b&gt;&amp;amp; | 9,90</td><td class=\"p\">pending</td>"
                    "<td class=\"p\">unseen</td></tr>"));
    CHECK(HAS(page, "<td>-</td><td>no content</td><td class=\"p\">silent</td></tr>")); /* seen 3 h ago */
    CHECK(HAS(page, "<p>Layout files: 2.9 none (built-in price and text only); 1.54 version "));
    CHECK(HAS(page, "<td>0000000000ABCDEF</td><td>2.9</td><td>3 h ago</td><td>2950 mV</td><td>-61</td><td>2</td>"
                    "<td>-</td><td>no content</td>"));
    CHECK(!HAS(page, "<b>"));
    CHECK(!HAS(page, "/dashboard"));
    api.dashboard = true;
    esl_sb_init(&sb, page, sizeof(page));
    CHECK_INT(esl_api_status_page(&api, 1000, &sb), 200);
    CHECK(HAS(page, "<a href=\"/dashboard\">Dashboard</a>"));
    api.dashboard = false;

    /* battery 0 = not measured */
    uint8_t reply[ESL_MAX_PAYLOAD];
    esl_poll_t p = {1, 0, 0, 0, ESL_MODEL_154, false, 0};
    esl_store_on_poll(&st, 2, &p, -70, 1000, NULL, reply);
    esl_sb_init(&sb, page, sizeof(page));
    CHECK_INT(esl_api_status_page(&api, 2000, &sb), 200);
    CHECK(HAS(page, "<td>0000000000000002</td><td>1.54</td><td>1 s ago</td><td>-</td><td>-70</td>"));

    /* worst case: every slot full, every byte escaped, both models with 16 layouts with
       long names, and half the tags with named content they cannot get */
    static char lf[4096];
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
        esl_sb_t l;
        esl_sb_init(&l, lf, sizeof(lf));
        esl_sb_printf(&l, "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"%s\",\"layouts\":[",
                      esl_model_name(m));
        for (int i = 0; i < ESL_LAYOUT_FILE_MAX; i++)
            esl_sb_printf(&l, "%s{\"name\":\"a-long-layout-name-with-31-ch%02d\",\"fields\":[\"a\",\"b\",\"c\",\"d\"],"
                          "\"elements\":[{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":1,\"h\":1}]}", i ? "," : "", i);
        esl_sb_puts(&l, "]}");
        char u[64];
        snprintf(u, sizeof(u), "/api/layouts/%s", esl_model_name(m));
        CHECK_INT(req("PUT", u, AUTH, lf, 0), 200);
    }
    char body[512], text[ESL_MAX_TEXT + 1];
    memset(text, '&', ESL_MAX_TEXT / 4);
    text[ESL_MAX_TEXT / 4] = '\0';
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    for (unsigned i = 0; i < CAP; i++) {
        char u[64];
        snprintf(u, sizeof(u), "/api/tags/%016X", 0x1000 + i);
        snprintf(body, sizeof(body), "{\"layout\":\"%s\",\"fields\":[\"%s\",\"%s\",\"%s\",\"%s\"]}",
                 i % 2 ? "a-long-layout-name-with-31-ch15" : "text", text, text, text, text);
        CHECK_INT(req("PUT", u, AUTH, body, 0), 200);
        esl_poll_t p = {65535, 65535, 1, ESL_POLL_FLAG_DRAW_FAILED, ESL_MODEL_154, false, 0};
        uint8_t reply[ESL_MAX_PAYLOAD];
        esl_store_on_poll(&st, 0x1000 + i, &p, -128, 0, NULL, reply);
    }
    esl_sb_init(&sb, page, sizeof(page));
    CHECK_INT(esl_api_status_page(&api, UINT64_C(1) << 50, &sb), 200);
    CHECK(!sb.overflow);
    CHECK(HAS(page, "pending, draw failed"));
    CHECK(HAS(page, "pending, draw failed (the tag cannot show images drawn by the AP (old firmware, or no SPI flash))"));
    CHECK(HAS(page, "a-long-layout-name-with-31-ch15: "));

    /* too small a buffer gives 500 instead of a cut page */
    char small[100];
    esl_sb_init(&sb, small, sizeof(small));
    CHECK_INT(esl_api_status_page(&api, 0, &sb), 500);
}

static void test_config_api(void)
{
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);

    /* AP-wide default */
    CHECK_INT(req("GET", "/api/config", NULL, NULL, 0), 401);
    CHECK_INT(req("GET", "/api/config", AUTH, NULL, 0), 200);
    CHECK_STR(resp, "{\"poll_s\":0,\"builtin_poll_s\":0,\"webhook\":null}");
    CHECK_INT(req("PUT", "/api/config/", AUTH, "{\"poll_s\":3600}", 0), 200);
    CHECK_STR(resp, "{\"poll_s\":3600,\"builtin_poll_s\":0,\"webhook\":null}");
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"poll_s\":0}", 0), 200);
    CHECK_INT(esl_store_get_next_poll(&st), 0);
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"poll_s\":9}", 0), 422);
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"poll_s\":3601}", 0), 422);
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"poll_s\":\"60\"}", 0), 422);
    CHECK_INT(req("PUT", "/api/config", AUTH, "{}", 0), 422);
    CHECK_INT(req("PUT", "/api/config", AUTH, "[]", 0), 400);
    CHECK_INT(req("DELETE", "/api/config", AUTH, NULL, 0), 405);
    CHECK_INT(req("GET", "/api/configX", AUTH, NULL, 0), 404);
    CHECK_INT(esl_store_get_next_poll(&st), 0);

    /* per tag: validation */
    static const char *const bad[] = {
        "{\"poll_s\":9}", "{\"poll_s\":3601}", "{\"poll_s\":-60}", "{\"poll_s\":60.5}", "{\"poll_s\":6e1}",
        "{\"poll_s\":true}", "{\"poll_s\":99999999999999999999}", "{\"max_poll_s\":0}",
        "{\"lost_ap_s\":59}", "{\"lost_ap_s\":65536}",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK_INT(req("PUT", "/api/tags/" MAC1 "/config", AUTH, bad[i], 0), 422);
    CHECK_INT(esl_store_count(&st), 0); /* nothing created by a bad request */
    CHECK_INT(req("PUT", "/api/tags/" MAC1 "/config", AUTH, "nope", 0), 400);
    CHECK_INT(req("GET", "/api/tags/" MAC1 "/config", AUTH, NULL, 0), 404);
    CHECK_INT(req("DELETE", "/api/tags/" MAC1 "/config", AUTH, NULL, 0), 405);
    CHECK_INT(req("GET", "/api/tags/" MAC1 "/configs", AUTH, NULL, 0), 400);
    CHECK_INT(req("GET", "/api/tags/" MAC1 "/x", AUTH, NULL, 0), 400);

    /* per tag: set, read back, unknown MAC accepted */
    CHECK_INT(req("PUT", "/api/tags/" MAC1 "/config/", AUTH,
                  "{\"poll_s\":10,\"max_poll_s\":3600,\"lost_ap_s\":60,\"other\":1}", 0), 200);
    CHECK_STR(resp, "{\"mac\":\"" MAC1 "\",\"poll_s\":10,\"config\":{\"poll_s\":10,\"max_poll_s\":3600,"
                    "\"lost_ap_s\":60},\"config_pending\":false,\"changed\":true}");
    CHECK_INT(req("GET", "/api/tags/" MAC1 "/config", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"lost_ap_s\":60}"));
    CHECK_INT(req("GET", "/api/tags/" MAC1, AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"content_id\":0,"));
    CHECK(HAS(resp, "\"poll_s\":10,"));

    /* the tag polls with CONFIG support: gets CONFIG with next_poll_s = its interval */
    uint8_t reply[ESL_MAX_PAYLOAD];
    esl_poll_t p = {2, 2950, 0, 0, ESL_MODEL_29, true, 0};
    CHECK_INT(esl_store_on_poll(&st, 0x00000123456789A8ull, &p, -61, 0, NULL, reply), ESL_CONFIG_LEN);
    CHECK_INT(reply[0], ESL_MSG_CONFIG);
    CHECK_INT(ESL_GET_U16(&reply[ESL_OFF_NEXT_POLL]), 10);
    CHECK_INT(req("GET", "/api/tags/" MAC1 "/config", AUTH, NULL, 0), 200);
    CHECK(HAS(resp, "\"config_pending\":true"));

    /* a tag with settings is kept like a tag with content when the table fills up */
    CHECK_INT(esl_store_available(&st), CAP - 1);

    /* worst case GET /api/tags: every slot with the longest numbers fits the buffer */
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 3600);
    for (unsigned i = 0; i < CAP; i++) {
        char u[64];
        snprintf(u, sizeof(u), "/api/tags/%016X/config", 0xFFFFFFF0u + i);
        CHECK_INT(req("PUT", u, AUTH, "{\"poll_s\":3600,\"max_poll_s\":3600,\"lost_ap_s\":65535}", 0), 200);
        esl_poll_t q = {65535, 65535, 4294967295u, 0xFF, ESL_MODEL_154, true, 1};
        esl_store_on_poll(&st, 0xFFFFFFF0u + i, &q, -128, 0, NULL, reply);
        esl_content_t c;
        esl_set_result_t r;
        esl_content_init(&c, ESL_LAYOUT_TEXT, 0);
        esl_store_set(&st, 0xFFFFFFF0u + i, &c, &r);
    }
    CHECK_INT(req("GET", "/api/tags", AUTH, NULL, UINT64_C(1) << 60), 200);
    CHECK(!out.overflow);
    CHECK(HAS(resp, "\"draw_failed\":true"));
    CHECK(HAS(resp, "\"config_pending\":true,\"content_error\":null,\"ota\":true,"));
}

/* Streaming (esl_sb_set_sink): a glue with a small buffer gets GET /api/tags
   and the status page in pieces, byte for byte what a big buffer would hold.
   Everything else must fit the buffer, and errors never start a stream. */
static char streamed[ESL_API_PAGE_SIZE(CAP)];
static size_t streamed_len, pieces;

static bool collect(esl_sb_t *sb)
{
    CHECK(sb->len > 0 && sb->len < sb->cap);
    CHECK_INT(sb->status, 200);
    memcpy(streamed + streamed_len, sb->buf, sb->len);
    streamed_len += sb->len;
    pieces++;
    return true;
}

static void test_streaming(void)
{
    static char page[ESL_API_PAGE_SIZE(CAP)];
    char small[200]; /* smaller than one tag: every tag needs a flush */
    esl_sb_t sb;
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    for (unsigned i = 0; i < CAP; i++) {
        char u[64];
        snprintf(u, sizeof(u), "/api/tags/%016X", 0x2000 + i);
        CHECK_INT(req("PUT", u, AUTH, "{\"layout\":\"price\",\"fields\":[\"Br\u00f8d & <b>\",\"9,90\"]}", 0), 200);
        poll_as_tag(0x2000 + i, 0, 1000 + i);
    }

    /* GET /api/tags, whole */
    CHECK_INT(req("GET", "/api/tags", AUTH, NULL, 5000), 200);
    CHECK(!out.overflow && out.sent == 0);
    size_t whole = out.len;
    CHECK(whole > sizeof(small));
    /* in pieces */
    streamed_len = pieces = 0;
    esl_sb_init(&sb, small, sizeof(small));
    esl_sb_set_sink(&sb, collect, NULL);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags", AUTH, NULL, 0, 5000, &sb), 200);
    CHECK(!sb.overflow && sb.streaming && sb.sent == streamed_len && pieces >= CAP);
    esl_sb_flush(&sb);
    CHECK_INT((int)sb.len, 0);
    CHECK_INT((int)streamed_len, (int)whole);
    CHECK_MEM(streamed, resp, whole);

    /* the same without a sink is the old 500 */
    esl_sb_init(&sb, small, sizeof(small));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags", AUTH, NULL, 0, 5000, &sb), 500);
    CHECK(HAS(small, "response too large"));

    /* a reply that is not streamed does not use the sink, even when it is there */
    streamed_len = pieces = 0;
    esl_sb_init(&sb, small, sizeof(small));
    esl_sb_set_sink(&sb, collect, NULL);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/config", AUTH, NULL, 0, 5000, &sb), 200);
    CHECK(!sb.streaming && sb.sent == 0 && pieces == 0);
    CHECK_STR(small, "{\"poll_s\":0,\"builtin_poll_s\":0,\"webhook\":null}");
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags/0000000000002000", AUTH, NULL, 0, 5000, &sb), 500);
    CHECK(!sb.streaming && pieces == 0); /* one tag is bigger than this buffer: an error, not a stream */
    CHECK_INT(esl_api_handle(&api, "GET", "/api/tags", "Bearer wrong", NULL, 0, 5000, &sb), 401);
    CHECK(!sb.streaming && sb.sent == 0 && pieces == 0);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/layouts/2.9", AUTH, NULL, 0, 5000, &sb), 500); /* too big */
    CHECK(!sb.streaming && pieces == 0);

    /* the status page */
    esl_sb_init(&sb, page, sizeof(page));
    CHECK_INT(esl_api_status_page(&api, 9000, &sb), 200);
    whole = sb.len;
    CHECK(whole > sizeof(small) && sb.sent == 0);
    streamed_len = pieces = 0;
    esl_sb_init(&sb, small, sizeof(small));
    esl_sb_set_sink(&sb, collect, NULL);
    CHECK_INT(esl_api_status_page(&api, 9000, &sb), 200);
    esl_sb_flush(&sb);
    CHECK(!sb.overflow && pieces >= CAP);
    CHECK_INT((int)streamed_len, (int)whole);
    CHECK_MEM(streamed, page, whole);
}

static void test_webhook_config(void)
{
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    notify.url[0] = '\0';
    persisted = 0;

    CHECK_INT(req("PUT", "/api/config", AUTH, "{}", 0), 422);
    CHECK(HAS(resp, "poll_s, webhook, tx_power, ntp or tz"));
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"webhook\":\"http://192.168.1.10:8080/esl\"}", 0), 200);
    CHECK_STR(resp, "{\"poll_s\":0,\"builtin_poll_s\":0,\"webhook\":\"http://192.168.1.10:8080/esl\"}");
    CHECK_STR(notify.url, "http://192.168.1.10:8080/esl");
    CHECK_STR(persisted_url, "http://192.168.1.10:8080/esl");
    CHECK_INT(persisted, 1);
    /* both at once; poll_s unchanged when left out */
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"poll_s\":120,\"webhook\":\"http://hook.local/\"}", 0), 200);
    CHECK_STR(resp, "{\"poll_s\":120,\"builtin_poll_s\":0,\"webhook\":\"http://hook.local/\"}");
    CHECK_INT(req("GET", "/api/config", AUTH, NULL, 0), 200);
    CHECK_STR(resp, "{\"poll_s\":120,\"builtin_poll_s\":0,\"webhook\":\"http://hook.local/\"}");
    /* bad URLs change nothing */
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"webhook\":\"https://hook.local/\"}", 0), 422);
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"webhook\":\"http:///x\"}", 0), 422);
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"webhook\":\"http://a b/\"}", 0), 422);
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"webhook\":7}", 0), 422);
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"poll_s\":5,\"webhook\":null}", 0), 422);
    char longurl[200];
    memset(longurl, 'a', sizeof(longurl));
    memcpy(longurl, "{\"webhook\":\"http://", 19);
    memcpy(longurl + 196, "\"}", 3);
    CHECK_INT(req("PUT", "/api/config", AUTH, longurl, 0), 422);
    CHECK_STR(notify.url, "http://hook.local/");
    CHECK_INT(persisted, 2);
    /* off again, with null or "" */
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"webhook\":\"\"}", 0), 200);
    CHECK_STR(resp, "{\"poll_s\":120,\"builtin_poll_s\":0,\"webhook\":null}");
    CHECK_STR(notify.url, "");
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"webhook\":null,\"poll_s\":null}", 0), 200);
    CHECK_STR(resp, "{\"poll_s\":0,\"builtin_poll_s\":0,\"webhook\":null}");
    CHECK_INT(persisted, 4);
    /* an AP without a webhook */
    api.notify = NULL;
    CHECK_INT(req("GET", "/api/config", AUTH, NULL, 0), 200);
    CHECK_STR(resp, "{\"poll_s\":0,\"builtin_poll_s\":0,\"webhook\":null}");
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"webhook\":\"http://hook.local/\"}", 0), 422);
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"poll_s\":60}", 0), 200);
    api.notify = &notify;
}

static int8_t tx_set_dbm;
static int tx_sets, tx_resets;
static void tx_set(void *ctx, int8_t dbm, bool builtin)
{
    (void)ctx;
    tx_set_dbm = dbm;
    tx_sets++;
    tx_resets += builtin;
}

static void test_tx_power_config(void)
{
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    esl_tx_power_t tp = {10, 20, tx_set, NULL};
    api.notify = NULL;
    api.tx_power = &tp;
    tx_sets = tx_resets = 0;

    CHECK_INT(req("GET", "/api/config", AUTH, NULL, 0), 200);
    CHECK_STR(resp, "{\"poll_s\":0,\"builtin_poll_s\":0,\"webhook\":null,\"tx_power\":10,\"builtin_tx_power\":20}");
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"tx_power\":15}", 0), 200);
    CHECK_STR(resp, "{\"poll_s\":0,\"builtin_poll_s\":0,\"webhook\":null,\"tx_power\":15,\"builtin_tx_power\":20}");
    CHECK_INT(tx_set_dbm, 15);
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"tx_power\":-24}", 0), 200);
    CHECK_INT(tp.dbm, -24);
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"tx_power\":-0}", 0), 200);
    CHECK_INT(tp.dbm, 0);
    /* out of range or not a whole number: nothing changes */
    static const char *bad[] = {"21", "-25", "100", "1.5", "1e1", "\"10\"", "-", "true", "--1"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char body[40];
        snprintf(body, sizeof(body), "{\"tx_power\":%s}", bad[i]);
        CHECK_INT(req("PUT", "/api/config", AUTH, body, 0), 422);
    }
    /* a bad value with a good poll_s changes neither */
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"poll_s\":60,\"tx_power\":30}", 0), 422);
    CHECK_INT(esl_store_get_next_poll(&st), 0);
    CHECK_INT(tx_sets, 3);
    CHECK_INT(tp.dbm, 0);
    /* null: back to the built-in value */
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"tx_power\":null,\"poll_s\":60}", 0), 200);
    CHECK_STR(resp, "{\"poll_s\":60,\"builtin_poll_s\":0,\"webhook\":null,\"tx_power\":20,\"builtin_tx_power\":20}");
    CHECK_INT(tx_sets, 4);
    CHECK_INT(tx_resets, 1);
    CHECK_INT(tx_set_dbm, 20);
    /* an AP that cannot set it */
    api.tx_power = NULL;
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"tx_power\":10}", 0), 422);
    CHECK(HAS(resp, "transmit power"));
    CHECK_INT(req("GET", "/api/config", AUTH, NULL, 0), 200);
    CHECK(!HAS(resp, "tx_power"));
    api.notify = &notify;
}

static int clock_sets;
static char clock_last_ntp[8], clock_last_tz[8]; /* "-" for NULL (unchanged) */

static void clock_set(void *ctx, const char *ntp, const char *tz)
{
    esl_clock_cfg_t *c = ctx;
    clock_sets++;
    snprintf(clock_last_ntp, sizeof(clock_last_ntp), "%s", ntp ? ntp : "-");
    snprintf(clock_last_tz, sizeof(clock_last_tz), "%s", tz ? tz : "-");
    if (ntp)
        snprintf(c->ntp, sizeof(c->ntp), "%s", ntp[0] ? ntp : c->builtin_ntp);
    if (tz)
        snprintf(c->tz, sizeof(c->tz), "%s", tz);
}

static void test_clock_config(void)
{
    static esl_clock_cfg_t clock = {"pool.ntp.org", "", "pool.ntp.org", &clock, clock_set};
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    notify.url[0] = '\0';

    /* without clock settings: left out, and PUT says so */
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"tz\":\"UTC0\"}", 0), 422);
    CHECK(HAS(resp, "no clock settings"));
    api.clock = &clock;
    CHECK_INT(req("GET", "/api/config", AUTH, NULL, 0), 200);
    CHECK_STR(resp, "{\"poll_s\":0,\"builtin_poll_s\":0,\"webhook\":null,\"ntp\":\"pool.ntp.org\","
                    "\"builtin_ntp\":\"pool.ntp.org\",\"tz\":null}");
    CHECK_INT(req("PUT", "/api/config", AUTH, "{}", 0), 422);
    CHECK(HAS(resp, "poll_s, webhook, tx_power, ntp or tz"));

    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"ntp\":\"no.pool.ntp.org\",\"tz\":\"CET-1CEST,M3.5.0,M10.5.0/3\"}", 0), 200);
    CHECK_STR(resp, "{\"poll_s\":0,\"builtin_poll_s\":0,\"webhook\":null,\"ntp\":\"no.pool.ntp.org\","
                    "\"builtin_ntp\":\"pool.ntp.org\",\"tz\":\"CET-1CEST,M3.5.0,M10.5.0/3\"}");
    CHECK_INT(clock_sets, 1);
    /* one key leaves the other alone */
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"tz\":\"<+0530>-5:30\"}", 0), 200);
    CHECK_STR(clock_last_ntp, "-");
    CHECK_STR(clock.ntp, "no.pool.ntp.org");
    CHECK(HAS(resp, "\"tz\":\"<+0530>-5:30\"}"));
    /* poll_s alone does not touch the clock */
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"poll_s\":60}", 0), 200);
    CHECK_INT(clock_sets, 2);

    static const char *const bad[] = {
        "{\"ntp\":\"-pool\"}", "{\"ntp\":\".pool\"}", "{\"ntp\":\"pool ntp\"}", "{\"ntp\":\"pool_ntp\"}",
        "{\"ntp\":3}", "{\"ntp\":true}", "{\"tz\":\"1CET\"}", "{\"tz\":\"CET -1\"}", "{\"tz\":\"CET\\\"\"}",
        "{\"tz\":[]}", "{\"tz\":\"UTC0\",\"ntp\":\"a b\"}",
        "{\"ntp\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}", /* 64 */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_INT(req("PUT", "/api/config", AUTH, bad[i], 0), 422);
    }
    CHECK_INT(clock_sets, 2); /* nothing saved by a bad request */
    CHECK_STR(clock.tz, "<+0530>-5:30");

    /* null and "" go back: ntp to the built-in server, tz to UTC */
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"ntp\":null,\"tz\":\"\"}", 0), 200);
    CHECK_STR(clock_last_ntp, "");
    CHECK_STR(clock_last_tz, "");
    CHECK_STR(resp, "{\"poll_s\":60,\"builtin_poll_s\":0,\"webhook\":null,\"ntp\":\"pool.ntp.org\","
                    "\"builtin_ntp\":\"pool.ntp.org\",\"tz\":null}");
    CHECK_INT(req("PUT", "/api/config", AUTH, "{\"ntp\":\"\",\"tz\":null}", 0), 200);
    CHECK_INT(clock_sets, 4);

    CHECK(esl_ntp_name_ok("192.168.1.1", 11));
    CHECK(!esl_ntp_name_ok("", 0));
    CHECK(esl_tz_ok("UTC0", 4));
    CHECK(!esl_tz_ok("", 0));
    api.clock = NULL;
}

static void poll_flags(uint64_t mac, uint32_t shown, uint16_t battery, uint8_t flags, bool images, uint64_t now)
{
    uint8_t reply[ESL_MAX_PAYLOAD];
    esl_poll_t p = {2, battery, shown, flags | (images ? ESL_POLL_F_IMAGE : 0), ESL_MODEL_29, false, 0};
    drawn(mac, shown);
    esl_layout_env_t env;
    esl_layouts_env(&layouts, ESL_MODEL_29, &env);
    esl_store_on_poll(&st, mac, &p, -61, now, &env, reply);
}

static void test_health(void)
{
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    esl_layouts_init(&layouts, NULL);

    /* empty table */
    CHECK_INT(req("GET", "/api/health", NULL, NULL, 0), 401);
    CHECK_INT(req("PUT", "/api/health", AUTH, "{}", 0), 405);
    CHECK_INT(req("GET", "/api/health", AUTH, NULL, 0), 200);
    CHECK_STR(resp, "{\"problems\":[],\"ok\":true,\"tags\":0,\"with_problems\":0,\"silent\":0,\"unseen\":0,"
                    "\"draw_failed\":0,\"low_battery\":0,\"content_error\":0}");

    /* four tags: 1 fine, 2 silent, 3 draw failed + low battery, 4 never seen (from NVS) */
    CHECK_INT(req("PUT", "/api/tags/0000000000000001", AUTH, "{\"layout\":\"text\",\"fields\":[\"a\"]}", 0), 200);
    uint32_t id = json_u32(resp, "\"content_id\":");
    CHECK_INT(req("PUT", "/api/tags/0000000000000002", AUTH, "{\"layout\":\"text\",\"fields\":[\"a\"]}", 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000003", AUTH, "{\"layout\":\"text\",\"fields\":[\"a\"]}", 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000004", AUTH, "{\"layout\":\"text\",\"fields\":[\"a\"]}", 0), 200);
    uint64_t now = UINT64_C(1000) * 1000;
    poll_flags(1, id, 2950, 0, true, now - 30 * 1000);
    poll_flags(2, id, 2950, 0, true, now - 400 * 1000);
    poll_flags(3, id, 2400, ESL_POLL_FLAG_DRAW_FAILED, true, now - 10 * 1000);
    CHECK_INT(req("GET", "/api/health", AUTH, NULL, now), 200);
    CHECK_STR(resp, "{\"problems\":["
                    "{\"mac\":\"0000000000000002\",\"problems\":[\"silent\"],\"seen_s_ago\":400,\"battery_mv\":2950},"
                    "{\"mac\":\"0000000000000003\",\"problems\":[\"draw_failed\",\"low_battery\"],\"seen_s_ago\":10,\"battery_mv\":2400},"
                    "{\"mac\":\"0000000000000004\",\"problems\":[\"unseen\"],\"seen_s_ago\":null,\"battery_mv\":null}"
                    "],\"ok\":false,\"tags\":4,\"with_problems\":3,\"silent\":1,\"unseen\":1,"
                    "\"draw_failed\":1,\"low_battery\":1,\"content_error\":0}");
    /* thresholds from the query string: 2 is fine with 600 s, 3's battery is fine below 2400 */
    CHECK_INT(req("GET", "/api/health?silent_s=600&battery_mv=2300", AUTH, NULL, now), 200);
    CHECK(HAS(resp, "\"with_problems\":2,\"silent\":0,\"unseen\":1,\"draw_failed\":1,\"low_battery\":0,"));
    /* a tag with its own interval is silent after 3 x that */
    CHECK_INT(req("PUT", "/api/tags/0000000000000002/config", AUTH, "{\"poll_s\":300}", 0), 200);
    CHECK_INT(req("GET", "/api/health", AUTH, NULL, now), 200);
    CHECK(HAS(resp, "\"silent\":0,"));
    CHECK_INT(req("GET", "/api/health", AUTH, NULL, now + 500 * 1000), 200);
    CHECK(HAS(resp, "{\"mac\":\"0000000000000002\",\"problems\":[\"silent\"],\"seen_s_ago\":900,"));
    CHECK(HAS(resp, "\"silent\":3,")); /* the others too by now */
    /* early after boot nothing is unseen yet */
    CHECK_INT(req("GET", "/api/health", AUTH, NULL, 100 * 1000), 200);
    CHECK(HAS(resp, "\"unseen\":0,"));

    /* content that cannot be shown: a layout no longer in the file, a tag without images */
    static char lf[512];
    snprintf(lf, sizeof(lf), "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":["
                             "{\"name\":\"sign\",\"fields\":[\"a\"],\"elements\":[{\"type\":\"rect\",\"x\":0,\"y\":0,\"w\":1,\"h\":1}]}]}");
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, lf, 0), 200);
    CHECK_INT(req("PUT", "/api/tags/0000000000000001", AUTH, "{\"layout\":\"sign\",\"fields\":[\"a\"]}", 0), 200);
    CHECK_INT(req("DELETE", "/api/layouts/2.9", AUTH, NULL, 0), 200);
    poll_flags(1, id, 2950, 0, true, now);
    /* tag 3 gets content, but it does not take images (no SPI flash) */
    CHECK_INT(req("PUT", "/api/tags/0000000000000003", AUTH, "{\"layout\":\"text\",\"fields\":[\"b\"]}", 0), 200);
    poll_flags(3, id, 2950, 0, false, now);
    CHECK_INT(req("GET", "/api/health", AUTH, NULL, now), 200);
    CHECK(HAS(resp, "{\"mac\":\"0000000000000001\",\"problems\":[\"content_error\"]"));
    CHECK(HAS(resp, "{\"mac\":\"0000000000000003\",\"problems\":[\"content_error\"]"));
    CHECK(HAS(resp, "\"low_battery\":0,\"content_error\":2}"));

    /* the report for the webhook: what changed since the last one */
    uint8_t prev[CAP];
    memset(prev, 0, sizeof(prev));
    esl_sb_t sb;
    static char page[ESL_API_RESP_SIZE(CAP)];
    esl_sb_init(&sb, page, sizeof(page));
    CHECK_INT(esl_api_health(&api, now, NULL, prev, &sb), 200);
    CHECK(HAS(page, "\"changed\":3}")); /* 1 and 3 content, 4 unseen (2 polls every 300 s, 400 s is fine) */
    CHECK_INT(prev[0], ESL_HEALTH_CONTENT_ERROR);
    CHECK_INT(prev[3], ESL_HEALTH_UNSEEN);
    esl_sb_init(&sb, page, sizeof(page));
    CHECK_INT(esl_api_health(&api, now, NULL, prev, &sb), 200);
    CHECK(HAS(page, "\"changed\":0}"));
    poll_flags(2, id, 2400, 0, true, now); /* 2 runs low */
    esl_sb_init(&sb, page, sizeof(page));
    CHECK_INT(esl_api_health(&api, now, NULL, prev, &sb), 200);
    CHECK(HAS(page, "\"changed\":1}"));
    CHECK_INT(prev[1], ESL_HEALTH_LOW_BATTERY);
    /* a deleted tag clears its slot */
    CHECK_INT(req("DELETE", "/api/tags/0000000000000004", AUTH, NULL, 0), 200);
    esl_sb_init(&sb, page, sizeof(page));
    CHECK_INT(esl_api_health(&api, now, NULL, prev, &sb), 200);
    CHECK_INT(prev[3], 0);
    CHECK(HAS(page, "\"tags\":3,"));

    /* streamed like GET /api/tags */
    char small[160];
    streamed_len = pieces = 0;
    esl_sb_init(&sb, small, sizeof(small));
    esl_sb_set_sink(&sb, collect, NULL);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/health", AUTH, NULL, 0, now, &sb), 200);
    esl_sb_flush(&sb);
    CHECK(!sb.overflow && pieces >= 1);
    CHECK_INT(req("GET", "/api/health", AUTH, NULL, now), 200);
    CHECK_INT((int)streamed_len, (int)out.len);
    CHECK_MEM(streamed, resp, out.len);
}

/* What the dashboard and the status screen show for a tag */
static void test_tag_level(void)
{
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    esl_layouts_init(&layouts, NULL);
    const uint64_t poll_ms = (uint64_t)ESL_TAG_BUILTIN_POLL_S * 1000, now = UINT64_C(1000) * 1000;
    for (int m = 1; m <= 4; m++) {
        char uri[40];
        snprintf(uri, sizeof(uri), "/api/tags/%016X", m);
        CHECK_INT(req("PUT", uri, AUTH, "{\"layout\":\"text\",\"fields\":[\"a\"]}", 0), 200);
    }
    uint32_t id = json_u32(resp, "\"content_id\":");
    poll_flags(1, id, 2950, 0, true, now - poll_ms / 2);                          /* shows it */
    poll_flags(2, id, 2950, 0, true, now - poll_ms / 2);                          /* gets new content below */
    poll_flags(3, id, 2950, 0, true, now - poll_ms - (ESL_TAG_CHECKIN_GRACE_S + 5) * 1000); /* late */
    poll_flags(4, id, 2950, 0, true, now - 4 * poll_ms);                          /* silent */
    CHECK_INT(req("PUT", "/api/tags/0000000000000002", AUTH, "{\"layout\":\"text\",\"fields\":[\"b\"]}", 0), 200);
    esl_tag_level_t want[] = {ESL_TAG_OK, ESL_TAG_BUSY, ESL_TAG_BUSY, ESL_TAG_BAD};
    for (int m = 1; m <= 4; m++) {
        esl_tag_t t;
        CHECK(esl_store_get(&st, (uint64_t)m, &t));
        CHECK_INT(esl_api_tag_level(&api, &t, now), want[m - 1]);
    }
    /* within the grace it is not late yet */
    esl_tag_t t;
    poll_flags(3, id, 2950, 0, true, now - poll_ms - (ESL_TAG_CHECKIN_GRACE_S - 5) * 1000);
    CHECK(esl_store_get(&st, 3, &t));
    CHECK_INT(esl_api_tag_level(&api, &t, now), ESL_TAG_OK);
}

void test_api(void)
{
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    esl_layouts_init(&layouts, NULL);
    esl_sb_init(&out, resp, sizeof(resp));
    test_auth_and_routing();
    test_auth_lockout();
    test_put_one();
    test_put_errors();
    test_batch_and_list();
    test_layouts();
    test_status_page();
    test_config_api();
    test_streaming();
    test_webhook_config();
    test_tx_power_config();
    test_clock_config();
    test_health();
    test_tag_level();
}
