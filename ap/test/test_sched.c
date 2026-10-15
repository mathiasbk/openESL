#include "tinytest.h"
#include "esl_api.h"
#include <stdlib.h>

#define CAP 4
#define AUTH "Bearer s3cret"
#define MAC1 "0000000000000001"
#define MAC2 "0000000000000002"
#define T0 1792065600u /* 2026-10-15 12:00 UTC */

static esl_tag_t tags[CAP];
static esl_store_t st;
static esl_sched_entry_t entries[CAP];
static esl_sched_t sched;
static esl_api_t api = {&st, "s3cret", "0.1.0", 25, NULL, NULL, NULL, &sched, NULL, NULL};
static char resp[ESL_API_RESP_SIZE(CAP)];
static esl_sb_t out;

/* the blobs, as NVS would keep them */
static uint8_t blobs[CAP][ESL_SCHED_BLOB_MAX];
static size_t blob_len[CAP];
static int saves, erases;

static void blob_save(void *ctx, unsigned slot, const uint8_t *b, size_t len)
{
    (void)ctx;
    memcpy(blobs[slot], b, len);
    blob_len[slot] = len;
    saves++;
}

static bool blob_load(void *ctx, unsigned slot, uint8_t *b, size_t *len)
{
    (void)ctx;
    if (!blob_len[slot] || blob_len[slot] > *len)
        return false;
    memcpy(b, blobs[slot], blob_len[slot]);
    *len = blob_len[slot];
    return true;
}

static void blob_erase(void *ctx, unsigned slot)
{
    (void)ctx;
    blob_len[slot] = 0;
    erases++;
}

static const esl_sched_ops_t ops = {NULL, NULL, NULL, blob_save, blob_load, blob_erase};

static void reset(void)
{
    esl_store_init(&st, tags, CAP, NULL, 0);
    esl_sched_init(&sched, entries, CAP, &ops);
    memset(blob_len, 0, sizeof(blob_len));
    saves = erases = 0;
}

static int req(const char *method, const char *uri, const char *body, uint64_t now_ms)
{
    return esl_api_handle(&api, method, uri, AUTH, body, body ? strlen(body) : 0, now_ms, &out);
}

static void price(const char *name, esl_content_t *c)
{
    esl_content_init(c, ESL_LAYOUT_PRICE, 0);
    esl_content_add_field(c, (const uint8_t *)name, strlen(name));
}

static bool shows(uint64_t mac, const char *name)
{
    esl_tag_t t;
    uint8_t len;
    if (!esl_store_get(&st, mac, &t) || !t.has_content)
        return false;
    const uint8_t *f = esl_content_field(&t.content, 0, &len);
    return len == strlen(name) && memcmp(f, name, len) == 0;
}

static void test_clock(void)
{
    reset();
    uint8_t src = 99;
    CHECK_INT(esl_sched_now(&sched, 5000, &src), 0);
    CHECK_INT(src, ESL_TIME_UNKNOWN);
    esl_sched_set_time(&sched, T0, 5000, ESL_TIME_API);
    CHECK_INT(esl_sched_now(&sched, 5000, &src), T0);
    CHECK_INT(src, ESL_TIME_API);
    CHECK_INT(esl_sched_now(&sched, 65999, NULL), T0 + 60);
    CHECK_STR(esl_time_src_name(ESL_TIME_NTP), "ntp");
    CHECK(esl_time_src_name(ESL_TIME_UNKNOWN) == NULL);
}

static void test_core(void)
{
    esl_content_t a, b, c;
    price("A", &a);
    price("B", &b);
    price("C", &c);
    reset();

    /* nothing is applied while the time is unknown */
    CHECK_INT(esl_sched_add(&sched, 1, T0 + 10, &a), ESL_SCHED_OK);
    CHECK_INT(esl_sched_tick(&sched, &st, 1000), 0);
    CHECK_INT(esl_sched_count(&sched), 1);

    esl_sched_set_time(&sched, T0, 1000, ESL_TIME_NTP);
    CHECK_INT(esl_sched_tick(&sched, &st, 10999), 0); /* T0 + 9 */
    CHECK_INT(esl_sched_tick(&sched, &st, 11000), 1); /* T0 + 10 */
    CHECK(shows(1, "A"));
    CHECK_INT(esl_sched_count(&sched), 0);
    CHECK_INT(blob_len[0], 0);

    /* same tag and time replaces; the latest due entry wins, whatever slot it is in */
    CHECK_INT(esl_sched_add(&sched, 2, T0 + 30, &c), ESL_SCHED_OK);
    CHECK_INT(esl_sched_add(&sched, 2, T0 + 20, &a), ESL_SCHED_OK);
    CHECK_INT(esl_sched_add(&sched, 2, T0 + 20, &b), ESL_SCHED_OK);
    CHECK_INT(esl_sched_count(&sched), 2);
    CHECK_INT(esl_sched_add(&sched, 3, T0 + 40, &a), ESL_SCHED_OK);
    CHECK_INT(esl_sched_add(&sched, 4, T0 + 40, &a), ESL_SCHED_OK);
    CHECK_INT(esl_sched_free(&sched), 0);
    CHECK_INT(esl_sched_add(&sched, 5, T0 + 40, &a), ESL_SCHED_FULL);
    CHECK(esl_sched_has(&sched, 2, T0 + 20));
    CHECK_INT(esl_sched_count_mac(&sched, 2), 2);
    CHECK_INT(esl_sched_tick(&sched, &st, 1000 + 35000), 1); /* both for tag 2 are due */
    CHECK(shows(2, "C"));
    CHECK_INT(esl_sched_count(&sched), 2);

    uint64_t mac;
    uint32_t at;
    esl_content_t got;
    bool found = false;
    for (unsigned s = 0; s < CAP; s++)
        if (esl_sched_get_slot(&sched, s, &mac, &at, &got) && mac == 3) {
            found = at == T0 + 40 && esl_content_equal(&got, &a);
        }
    CHECK(found);

    /* restore at boot from the blobs */
    uint8_t copy[CAP][ESL_SCHED_BLOB_MAX];
    size_t copy_len[CAP];
    memcpy(copy, blobs, sizeof(copy));
    memcpy(copy_len, blob_len, sizeof(copy_len));
    esl_sched_init(&sched, entries, CAP, &ops);
    unsigned restored = 0;
    for (unsigned s = 0; s < CAP; s++)
        if (copy_len[s])
            restored += esl_sched_restore(&sched, s, copy[s], copy_len[s]);
    CHECK_INT(restored, 2);
    CHECK(!esl_sched_restore(&sched, 0, copy[0], 3));
    CHECK_INT(esl_sched_remove(&sched, 3, false), 1);
    CHECK_INT(esl_sched_remove(&sched, 0, true), 1);
    CHECK_INT(esl_sched_count(&sched), 0);
}

static void test_api_time(void)
{
    reset();
    CHECK_INT(req("GET", "/api/time", NULL, 1000), 200);
    CHECK_STR(resp, "{\"time\":null,\"source\":null}");
    CHECK_INT(req("PUT", "/api/time", "{\"time\":1792065600123}", 1000), 422); /* milliseconds */
    CHECK_INT(req("PUT", "/api/time", "{\"time\":-1}", 1000), 422);
    CHECK_INT(req("PUT", "/api/time", "{\"time\":1792065600}", 1000), 200);
    CHECK_STR(resp, "{\"time\":1792065600,\"source\":\"api\"}");
    CHECK_INT(req("GET", "/api/time", NULL, 3500), 200);
    CHECK_STR(resp, "{\"time\":1792065602,\"source\":\"api\"}");
    CHECK_INT(req("DELETE", "/api/time", NULL, 1000), 405);

    /* without a schedule there is no clock either */
    esl_api_t none = api;
    none.sched = NULL;
    CHECK_INT(esl_api_handle(&none, "GET", "/api/time", AUTH, NULL, 0, 0, &out), 404);
    const char *b = "{\"layout\":\"price\",\"fields\":[\"A\"],\"at\":1792065700}";
    CHECK_INT(esl_api_handle(&none, "PUT", "/api/tags/" MAC1, AUTH, b, strlen(b), 0, &out), 422);
}

static void test_api_schedule(void)
{
    reset();
    const char *later = "{\"layout\":\"price\",\"fields\":[\"Tilbud\",\"9,90\"],\"at\":1792069200}";

    /* "at" needs the time */
    CHECK_INT(req("PUT", "/api/tags/" MAC1, later, 1000), 409);
    CHECK(HAS(resp, "PUT /api/time"));
    esl_sched_set_time(&sched, T0, 1000, ESL_TIME_NTP);

    CHECK_INT(req("PUT", "/api/tags/" MAC1, later, 1000), 200);
    CHECK_STR(resp, "{\"mac\":\"" MAC1 "\",\"scheduled\":true,\"at\":1792069200}");
    CHECK(!shows(1, "Tilbud"));
    CHECK_INT(req("PUT", "/api/tags/" MAC1, "{\"layout\":\"price\",\"fields\":[\"A\"],\"at\":\"soon\"}", 1000), 422);

    /* a time that has passed is now */
    CHECK_INT(req("PUT", "/api/tags/" MAC1, "{\"layout\":\"price\",\"fields\":[\"Ord\"],\"at\":1792000000}", 1000),
              200);
    CHECK(HAS(resp, "\"changed\":true"));
    CHECK(shows(1, "Ord"));

    /* batch: one now, two later for the same tag, one later for another */
    const char *batch =
        "{\"tags\":[{\"mac\":\"" MAC2 "\",\"layout\":\"text\",\"fields\":[\"Nå\"]},"
        "{\"mac\":\"" MAC2 "\",\"layout\":\"text\",\"fields\":[\"Fra 13\"],\"at\":1792069200},"
        "{\"mac\":\"" MAC2 "\",\"layout\":\"text\",\"fields\":[\"Fra 14\"],\"at\":1792072800}]}";
    CHECK_INT(req("PUT", "/api/tags", batch, 1000), 200);
    CHECK(HAS(resp, "\"scheduled\":true,\"at\":1792072800"));
    CHECK(shows(2, "N\xe5"));
    CHECK_INT(esl_sched_count(&sched), 3);

    /* full: the batch changes nothing */
    const char *two = "{\"tags\":[{\"mac\":\"0000000000000003\",\"layout\":\"text\",\"fields\":[\"x\"],\"at\":1792069200},"
                      "{\"mac\":\"0000000000000004\",\"layout\":\"text\",\"fields\":[\"x\"],\"at\":1792069200}]}";
    CHECK_INT(req("PUT", "/api/tags", two, 1000), 507);
    CHECK_STR(resp, "{\"error\":\"schedule full\"}");
    CHECK_INT(esl_sched_count(&sched), 3);
    /* replacing an entry takes no room */
    CHECK_INT(req("PUT", "/api/tags/" MAC1, later, 1000), 200);

    CHECK_INT(req("GET", "/api/tags/" MAC2, NULL, 1000), 200);
    CHECK(HAS(resp, "\"scheduled\":[1792069200,1792072800]") || HAS(resp, "\"scheduled\":[1792072800,1792069200]"));
    CHECK_INT(req("GET", "/api/tags", NULL, 1000), 200);
    CHECK(HAS(resp, "\"time\":1792065600,\"scheduled\":3,\"max_scheduled\":4"));
    CHECK_INT(req("GET", "/api/schedule", NULL, 1000), 200);
    CHECK(HAS(resp, "{\"mac\":\"" MAC1 "\",\"at\":1792069200,\"layout\":\"price\",\"fields\":[\"Tilbud\",\"9,90\"]"));
    CHECK(HAS(resp, "\"fields\":[\"Fra 14\"]"));

    /* at 13:00 MAC1 and MAC2 change; at 14:00 MAC2 again */
    CHECK_INT(esl_sched_tick(&sched, &st, 1000 + 3600 * 1000), 2);
    CHECK(shows(1, "Tilbud"));
    CHECK(shows(2, "Fra 13"));
    CHECK_INT(esl_sched_tick(&sched, &st, 1000 + 7200 * 1000), 1);
    CHECK(shows(2, "Fra 14"));

    /* deleting */
    CHECK_INT(req("PUT", "/api/tags/" MAC1, later, 1000), 200);
    CHECK_INT(req("PUT", "/api/tags/" MAC2, later, 1000), 200);
    CHECK_INT(req("DELETE", "/api/schedule/" MAC1, NULL, 1000), 200);
    CHECK_STR(resp, "{\"deleted\":1}");
    CHECK_INT(req("DELETE", "/api/schedule/" MAC1, NULL, 1000), 404);
    CHECK_INT(req("DELETE", "/api/schedule/123", NULL, 1000), 400);
    CHECK_INT(req("GET", "/api/schedule/" MAC1, NULL, 1000), 405);
    CHECK_INT(req("DELETE", "/api/tags/" MAC2, NULL, 1000), 200); /* takes its schedule along */
    CHECK_INT(esl_sched_count(&sched), 0);
    CHECK_INT(req("PUT", "/api/tags/" MAC1, later, 1000), 200);
    CHECK_INT(req("DELETE", "/api/schedule", NULL, 1000), 200);
    CHECK_STR(resp, "{\"deleted\":1}");
    CHECK_INT(req("DELETE", "/api/schedule", NULL, 1000), 404);
}

void test_sched(void)
{
    esl_sb_init(&out, resp, sizeof(resp));
    test_clock();
    test_core();
    test_api_time();
    test_api_schedule();
}
