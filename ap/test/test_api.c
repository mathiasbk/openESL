#include "tinytest.h"
#include "esl_api.h"
#include <stdlib.h>

#define CAP 8
#define AUTH "Bearer s3cret"
#define MAC1 "00000123456789A8"

static esl_tag_t tags[CAP];
static esl_store_t st;
static esl_layouts_t layouts;
static esl_api_t api = {&st, "s3cret", "0.1.0", 25, &layouts};
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

static void poll_as_tag(uint64_t mac, uint32_t shown, uint64_t now)
{
    uint8_t reply[ESL_MAX_PAYLOAD];
    esl_poll_t p = {2, 2950, shown, 0, ESL_MODEL_29};
    esl_store_on_poll(&st, mac, &p, -61, now, reply);
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
    const char *body = "{ \"layout\": \"price\", \"fields\": [\"Lettmelk 1 L\", \"24,90\", \"kr/l 24,90\"], \"price_red\": true }";

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
    char expect[512];
    snprintf(expect, sizeof(expect),
             "{\"mac\":\"" MAC1 "\",\"seen_s_ago\":12,\"battery_mv\":2950,\"rssi\":-61,\"fw\":2,"
             "\"model\":\"2.9\",\"draw_failed\":false,\"content_id\":%u,\"shown_content_id\":%u,\"pending\":false,"
             "\"layout\":\"price\",\"fields\":[\"Lettmelk 1 L\",\"24,90\",\"kr/l 24,90\"],"
             "\"price_red\":true,\"frame_red\":false}",
             (unsigned)id, (unsigned)id);
    CHECK_STR(resp, expect);

    /* price_red is part of the content */
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
    esl_poll_t p = {1, 2400, 0, ESL_POLL_FLAG_DRAW_FAILED, ESL_MODEL_UNKNOWN};
    esl_store_on_poll(&st, 2, &p, -70, 1000, reply);
    CHECK_INT(req("GET", "/api/tags/0000000000000002", AUTH, NULL, 1000), 200);
    CHECK(HAS(resp, "\"fw\":1,\"model\":null,\"draw_failed\":true,"));
    p.flags = 0;
    p.model = ESL_MODEL_154;
    esl_store_on_poll(&st, 2, &p, -70, 2000, reply);
    CHECK_INT(req("GET", "/api/tags/0000000000000002", AUTH, NULL, 2000), 200);
    CHECK(HAS(resp, "\"model\":\"1.54\",\"draw_failed\":false,"));
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
    CHECK_INT(req("PUT", u, AUTH, "{\"layout\":\"price\",\"fields\":[\"a\"],\"price_red\":\"yes\"}", 0), 422);

    /* 92 bytes of Latin-1 is the limit; each Latin-1 character (e.g. æ) is one byte */
    char body[512];
    char text[200];
    memset(text, 'x', 92);
    text[92] = '\0';
    snprintf(body, sizeof(body), "{\"layout\":\"text\",\"fields\":[\"%s\"]}", text);
    CHECK_INT(req("PUT", u, AUTH, body, 0), 200);
    text[92] = 'x';
    text[93] = '\0';
    snprintf(body, sizeof(body), "{\"layout\":\"text\",\"fields\":[\"%s\"]}", text);
    CHECK_INT(req("PUT", u, AUTH, body, 0), 422);
    CHECK(HAS(resp, "too long"));
    snprintf(body, sizeof(body), "{\"layout\":\"text\",\"fields\":[\"%.45s\",\"%.46s\",\"\xC3\xA6\"]}", text, text);
    CHECK_INT(req("PUT", u, AUTH, body, 0), 200);
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

static void test_batch_and_list(void)
{
    char body[8192];
    esl_sb_t sb;

    esl_store_init(&st, tags, CAP, NULL, 0);

    CHECK_INT(req("PUT", "/api/tags", AUTH,
                  "{\"tags\":[{\"mac\":\"0000000000000001\",\"layout\":\"price\",\"fields\":[\"A\",\"1,00\"]},"
                  "{\"mac\":\"0000000000000002\",\"layout\":\"text\",\"fields\":[\"B\"],\"frame_red\":true}]}", 0), 200);
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
        esl_sb_printf(&sb, "%s{\"mac\":\"0000000000000001\",\"layout\":\"price\",\"fields\":[\"Navn %d\",\"%d,90\",\"kr/kg\"],\"price_red\":false}",
                      i ? "," : "", i, i);
    esl_sb_puts(&sb, "]}");
    CHECK(!sb.overflow);
    CHECK_INT(req("PUT", "/api/tags", AUTH, body, 0), 200);

    /* list: a discovered tag shows up with content_id 0 */
    esl_store_delete(&st, 0x105);
    poll_as_tag(0xABCDEF, 0, 1000);
    CHECK_INT(req("GET", "/api/tags", AUTH, NULL, 86400000), 200);
    CHECK(HAS(resp, "{\"ap\":{\"fw\":\"0.1.0\",\"uptime_s\":86400,\"channel\":25,\"max_tags\":8},\"tags\":["));
    CHECK(HAS(resp, "{\"mac\":\"0000000000ABCDEF\",\"seen_s_ago\":86399,\"battery_mv\":2950,\"rssi\":-61,\"fw\":2,"
                    "\"model\":\"2.9\",\"draw_failed\":false,\"content_id\":0,\"shown_content_id\":0,\"pending\":false}"));
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

static void test_layouts(void)
{
    const char *file = "{\"name\":\"price-2.9\",\"fields\":[{\"x\":0,\"y\":0}]}";
    esl_layouts_init(&layouts, NULL);

    CHECK_INT(req("GET", "/api/layouts", NULL, NULL, 0), 401);
    CHECK_INT(req("GET", "/api/layouts", AUTH, NULL, 0), 200);
    CHECK_STR(resp, "{\"max_size\":8192,\"layouts\":[{\"model\":\"2.9\",\"version\":null,\"size\":0},"
                    "{\"model\":\"1.54\",\"version\":null,\"size\":0}]}");
    CHECK_INT(req("GET", "/api/layouts/", AUTH, NULL, 0), 200);
    CHECK_INT(req("PUT", "/api/layouts", AUTH, file, 0), 405);
    CHECK_INT(req("GET", "/api/layouts/2.9", AUTH, NULL, 0), 404);
    CHECK_INT(req("GET", "/api/layouts/4.2", AUTH, NULL, 0), 404);
    CHECK(HAS(resp, "unknown model"));
    CHECK_INT(req("GET", "/api/layoutsX", AUTH, NULL, 0), 404);

    /* upload, read back, same file again is not a change */
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, file, 0), 200);
    CHECK(HAS(resp, "{\"model\":\"2.9\",\"version\":"));
    CHECK(HAS(resp, "\"changed\":true"));
    uint32_t v = json_u32(resp, "\"version\":");
    CHECK(v != 0);
    CHECK_INT(json_u32(resp, "\"size\":"), strlen(file));
    CHECK_INT(req("PUT", "/api/layouts/2.9/", AUTH, file, 0), 200);
    CHECK(HAS(resp, "\"changed\":false"));
    CHECK_INT(json_u32(resp, "\"version\":"), v);
    CHECK_INT(req("GET", "/api/layouts/2.9", AUTH, NULL, 0), 200);
    CHECK_STR(resp, file);
    CHECK_INT(req("GET", "/api/layouts", AUTH, NULL, 0), 200);
    char expect[256];
    snprintf(expect, sizeof(expect), "{\"model\":\"2.9\",\"version\":%u,\"size\":%u}", (unsigned)v,
             (unsigned)strlen(file));
    CHECK(HAS(resp, expect));
    CHECK(HAS(resp, "{\"model\":\"1.54\",\"version\":null,\"size\":0}"));

    /* models are independent; a different file gets a different version */
    CHECK_INT(req("PUT", "/api/layouts/1.54", AUTH, "{\"name\":\"price-1.54\"}", 0), 200);
    CHECK(json_u32(resp, "\"version\":") != v);
    CHECK_INT(req("GET", "/api/layouts/2.9", AUTH, NULL, 0), 200);
    CHECK_STR(resp, file);

    /* validation */
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, NULL, 0), 400);
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, "{\"a\":", 0), 400);
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, "[1,2]", 0), 400);
    CHECK_INT(req("PUT", "/api/layouts/2.9", AUTH, "\"x\"", 0), 400);
    CHECK_INT(req("PUT", "/api/layouts/3.0", AUTH, file, 0), 404);
    CHECK_INT(req("POST", "/api/layouts/2.9", AUTH, file, 0), 405);

    /* the biggest file, with as many tokens as possible, is accepted */
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
    big[0] = '{';
    o = 1;
    while (o + 8 < ESL_LAYOUT_MAX_SIZE) {
        memcpy(&big[o], "\"a\":[1],", 8);
        o += 8;
    }
    memcpy(&big[o], "\"b\":1}", 6);
    big[ESL_LAYOUT_MAX_SIZE] = '\0';
    CHECK_INT(strlen(big), ESL_LAYOUT_MAX_SIZE);
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

    /* without a layout store the endpoints do not exist */
    esl_api_t no_layouts = api;
    no_layouts.layouts = NULL;
    CHECK_INT(esl_api_handle(&no_layouts, "GET", "/api/layouts", AUTH, NULL, 0, 0, &out), 404);
}

static void test_status_page(void)
{
    static char page[ESL_API_PAGE_SIZE(CAP)];
    esl_sb_t sb;
    esl_sb_init(&sb, page, sizeof(page));
    esl_store_init(&st, tags, CAP, NULL, 0);

    CHECK_INT(req("PUT", "/api/tags/" MAC1, AUTH,
                  "{\"layout\":\"price\",\"fields\":[\"Br\u00f8d <b>&amp;\",\"9,90\"]}", 0), 200);
    poll_as_tag(0xABCDEF, 0, 1000);
    CHECK_INT(esl_api_status_page(&api, 1000 + 3 * 3600 * 1000, &sb), 200);
    CHECK(HAS(page, "<!DOCTYPE html>"));
    CHECK(HAS(page, "Firmware 0.1.0, up 3 h, channel 25, 2 of 8 tags."));
    CHECK(HAS(page, "<td>" MAC1 "</td><td>-</td><td>-</td>"));
    CHECK(HAS(page, "price: Br\xC3\xB8" "d &lt;b&gt;&amp;amp; | 9,90</td><td class=\"p\">pending"));
    CHECK(HAS(page, "<td>0000000000ABCDEF</td><td>2.9</td><td>3 h ago</td><td>2950 mV</td><td>-61</td><td>2</td>"
                    "<td>-</td><td>no content</td>"));
    CHECK(!HAS(page, "<b>"));

    /* worst case: every slot full, every byte escaped */
    char body[512], text[ESL_MAX_TEXT + 1];
    memset(text, '&', ESL_MAX_TEXT / 4);
    text[ESL_MAX_TEXT / 4] = '\0';
    esl_store_init(&st, tags, CAP, NULL, 0);
    for (unsigned i = 0; i < CAP; i++) {
        char u[64];
        snprintf(u, sizeof(u), "/api/tags/%016X", 0x1000 + i);
        snprintf(body, sizeof(body), "{\"layout\":\"text\",\"fields\":[\"%s\",\"%s\",\"%s\",\"%s\"]}", text, text,
                 text, text);
        CHECK_INT(req("PUT", u, AUTH, body, 0), 200);
        esl_poll_t p = {65535, 65535, 1, ESL_POLL_FLAG_DRAW_FAILED, ESL_MODEL_154};
        uint8_t reply[ESL_MAX_PAYLOAD];
        esl_store_on_poll(&st, 0x1000 + i, &p, -128, 0, reply);
    }
    esl_sb_init(&sb, page, sizeof(page));
    CHECK_INT(esl_api_status_page(&api, UINT64_C(1) << 50, &sb), 200);
    CHECK(!sb.overflow);
    CHECK(HAS(page, "pending, draw failed"));

    /* too small a buffer gives 500 instead of a cut page */
    char small[100];
    esl_sb_init(&sb, small, sizeof(small));
    CHECK_INT(esl_api_status_page(&api, 0, &sb), 500);
}

void test_api(void)
{
    esl_store_init(&st, tags, CAP, NULL, 0);
    esl_layouts_init(&layouts, NULL);
    esl_sb_init(&out, resp, sizeof(resp));
    test_auth_and_routing();
    test_put_one();
    test_put_errors();
    test_batch_and_list();
    test_layouts();
    test_status_page();
}
