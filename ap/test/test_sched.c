#include "tinytest.h"
#include "esl_api.h"
#include <stdlib.h>

#define CAP 4
#define AUTH "Bearer s3cret"
#define MAC1 "0000000000000001"
#define MAC2 "0000000000000002"
#define T0 1792065600u /* 2026-10-15 12:00 UTC */

static esl_store_t st;
static esl_sched_entry_t entries[CAP];
static esl_sched_t sched;
static esl_api_t api = {&st, "s3cret", "0.1.0", NULL, NULL, NULL, &sched, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, false, NULL, NULL, 0, NULL, NULL, NULL};
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

static const esl_sched_ops_t ops = {NULL, NULL, NULL, blob_save, blob_load, blob_erase, NULL};

static void reset(void)
{
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
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
    esl_content_t c;
    uint8_t len;
    if (!esl_store_get_content(&st, mac, &c))
        return false;
    const uint8_t *f = esl_content_field(&c, 0, &len);
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

static uint16_t poll_next(uint64_t mac, uint64_t now_ms, uint8_t *type)
{
    esl_poll_t p = {3, 2950, 0, 0, ESL_MODEL_154, false, 0, false, 0, 0, 0, 0, false, 0};
    uint8_t reply[ESL_MAX_PAYLOAD];
    CHECK(esl_store_on_poll(&st, mac, &p, -61, now_ms, NULL, reply) > 0);
    if (type)
        *type = reply[ESL_OFF_TYPE];
    return (uint16_t)(reply[ESL_OFF_NEXT_POLL] | reply[ESL_OFF_NEXT_POLL + 1] << 8);
}

/* Tags with an entry due before their next poll are told to come back just after it */
static void test_wake(void)
{
    esl_content_t a;
    uint8_t type;
    price("A", &a);
    reset();
    esl_store_set_next_poll(&st, 300);
    poll_next(1, 1000, NULL);

    CHECK_INT(esl_sched_add(&sched, 1, T0 + 100, &a), ESL_SCHED_OK);
    CHECK_INT(esl_sched_add(&sched, 2, T0 + 90, &a), ESL_SCHED_OK);
    esl_sched_tick(&sched, &st, 1000);
    CHECK_INT(poll_next(1, 11000, NULL), 300); /* time unknown: nothing to wake for */

    esl_sched_set_time(&sched, T0, 1000, ESL_TIME_NTP);
    esl_sched_tick(&sched, &st, 1000);
    CHECK_INT(poll_next(1, 11000, &type), 92); /* due at T0 + 100, now T0 + 10, plus the margin */
    CHECK_INT(type, ESL_MSG_IDLE);

    /* tag 2 was not in the table: it gets the hint at the refresh a minute later */
    CHECK_INT(poll_next(2, 12000, NULL), 300);
    esl_sched_tick(&sched, &st, 13000);
    CHECK_INT(poll_next(2, 13000, NULL), 300);
    esl_sched_tick(&sched, &st, 61000);
    CHECK_INT(poll_next(2, 61000, NULL), 32);

    CHECK_INT(poll_next(1, 100000, NULL), 3);  /* a bit early: come back in 3 s */
    CHECK_INT(poll_next(1, 101500, NULL), 2);  /* due, but the tick has not run yet */
    CHECK_INT(esl_sched_tick(&sched, &st, 101000), 2);
    CHECK_INT(poll_next(1, 103000, &type), 300);
    CHECK_INT(type, ESL_MSG_IDLE); /* the content is set: the image follows the IDLE */
    CHECK(shows(1, "A"));
    CHECK(shows(2, "A"));

    /* far away: the normal interval; removing the entry takes the hint away */
    CHECK_INT(esl_sched_add(&sched, 1, T0 + 1000, &a), ESL_SCHED_OK);
    CHECK_INT(esl_sched_add(&sched, 2, T0 + 200, &a), ESL_SCHED_OK);
    esl_sched_tick(&sched, &st, 104000);
    CHECK_INT(poll_next(1, 104000, NULL), 300);
    CHECK_INT(poll_next(2, 104000, NULL), 99);
    CHECK_INT(esl_sched_remove(&sched, 2, false), 1);
    esl_sched_tick(&sched, &st, 105000);
    CHECK_INT(poll_next(2, 105000, NULL), 300);

    /* the AP sends 0 (tags keep their built-in 60 s): shortened only below that */
    esl_store_set_next_poll(&st, 0);
    CHECK_INT(esl_sched_add(&sched, 2, T0 + 200, &a), ESL_SCHED_OK);
    esl_sched_tick(&sched, &st, 106000);
    CHECK_INT(poll_next(2, 106000, NULL), 0);
    CHECK_INT(poll_next(2, 160000, NULL), 43);
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

/* A tag back from NVS after a restart, not heard since: GET /api/tags shows what
   it said last, and how long ago in wall-clock time once the AP knows the time */
static void test_api_restored_tag(void)
{
    reset();
    esl_seen_t rec = {ESL_MODEL_154, 0x0401, 2600, -88, T0 - 7200};
    CHECK(esl_store_restore_seen(&st, 1, 0x2, &rec));
    CHECK_INT(req("GET", "/api/tags/" MAC2, NULL, 1000), 200);
    CHECK(strstr(resp, "\"seen_s_ago\":null,\"battery_mv\":2600,\"rssi\":-88,\"fw\":1025,\"model\":\"1.54\"") != NULL);
    CHECK_INT(req("PUT", "/api/time", "{\"time\":1792065600}", 1000), 200);
    CHECK_INT(req("GET", "/api/tags/" MAC2, NULL, 1000), 200);
    CHECK(strstr(resp, "\"seen_s_ago\":7200,") != NULL);
    /* the dashboard shows it late from that, and so does the status screen */
    esl_tag_t t;
    CHECK(esl_store_get(&st, 0x2, &t));
    CHECK_INT(esl_api_tag_level(&api, &t, 1000), ESL_TAG_BUSY);
    /* a clock behind the stored time: unknown rather than negative */
    reset();
    rec.unix_s = T0 + 100;
    CHECK(esl_store_restore_seen(&st, 1, 0x2, &rec));
    CHECK_INT(req("PUT", "/api/time", "{\"time\":1792065600}", 1000), 200);
    CHECK_INT(req("GET", "/api/tags/" MAC2, NULL, 1000), 200);
    CHECK(strstr(resp, "\"seen_s_ago\":null,") != NULL);
    CHECK(esl_store_get(&st, 0x2, &t));
    CHECK_INT(esl_api_tag_level(&api, &t, 1000), ESL_TAG_OK);
}

/* Recurring entries (#415): a fake time zone at +2 h that goes to +1 h ten days after T0 */
#define DST_END (T0 + 10 * 86400u)
static int32_t fake_offset(void *ctx, uint32_t t)
{
    (void)ctx;
    return t < DST_END ? 7200 : 3600;
}
static const esl_sched_ops_t tz_ops = {NULL, NULL, NULL, blob_save, blob_load, blob_erase, fake_offset};
#define AT(t) ((uint64_t)((t) - T0) * 1000u) /* now_ms for a time */
#define DAY 86400u
#define WEEK (7u * DAY)

static uint32_t slot_at(uint64_t mac, uint8_t repeat)
{
    for (unsigned i = 0; i < CAP; i++) {
        uint64_t m;
        uint32_t at;
        if (esl_sched_get_slot(&sched, i, &m, &at, NULL) && m == mac && esl_sched_slot_repeat(&sched, i) == repeat)
            return at;
    }
    return 0;
}

static void test_repeat(void)
{
    esl_content_t a, b, w, d, o;
    esl_set_result_t r;
    price("A", &a);
    price("B", &b);
    price("W", &w);
    price("D", &d);
    price("O", &o);
    reset();
    esl_sched_init(&sched, entries, CAP, &tz_ops);
    esl_sched_set_time(&sched, T0, 0, ESL_TIME_NTP);
    CHECK_STR(esl_sched_repeat_name(ESL_SCHED_WEEKLY), "weekly");
    CHECK(esl_sched_repeat_name(ESL_SCHED_ONCE) == NULL);
    CHECK_INT(esl_sched_repeat_parse("daily", 5), ESL_SCHED_DAILY);
    CHECK_INT(esl_sched_repeat_parse("monthly", 7), -1);

    /* every week at 15:00 local (13:00 UTC at +2 h) */
    uint32_t at = T0 + 3600;
    CHECK_INT(esl_sched_add_repeat(&sched, 1, &at, ESL_SCHED_WEEKLY, T0, &a), ESL_SCHED_OK);
    CHECK_INT(at, T0 + 3600);
    CHECK_INT(esl_sched_tick(&sched, &st, AT(T0 + 3600)), 1);
    CHECK(shows(1, "A"));
    CHECK_INT(esl_sched_count(&sched), 1); /* still there, a week on */
    CHECK_INT(slot_at(1, ESL_SCHED_WEEKLY), T0 + 3600 + WEEK);
    CHECK_INT(esl_store_set(&st, 1, &b, &r), ESL_STORE_OK);
    CHECK_INT(esl_sched_tick(&sched, &st, AT(T0 + 3600 + WEEK - 1)), 0);
    CHECK_INT(esl_sched_tick(&sched, &st, AT(T0 + 3600 + WEEK)), 1);
    CHECK(shows(1, "A"));
    /* the next one is after the change to +1 h: still 15:00 local, now 14:00 UTC */
    CHECK_INT(slot_at(1, ESL_SCHED_WEEKLY), T0 + 7200 + 2 * WEEK);

    /* kept over a restart, with what it repeats */
    uint8_t copy[CAP][ESL_SCHED_BLOB_MAX];
    size_t copy_len[CAP];
    memcpy(copy, blobs, sizeof(copy));
    memcpy(copy_len, blob_len, sizeof(copy_len));
    esl_sched_init(&sched, entries, CAP, &tz_ops);
    for (unsigned i = 0; i < CAP; i++)
        if (copy_len[i]) {
            CHECK(esl_sched_restore(&sched, i, copy[i], copy_len[i]));
            /* firmware before it: the blob is not a one-off entry with extra bytes */
            uint64_t m;
            esl_content_t c;
            CHECK(!esl_persist_decode(&copy[i][4], copy_len[i] - 4, &m, &c));
        }
    CHECK_INT(slot_at(1, ESL_SCHED_WEEKLY), T0 + 7200 + 2 * WEEK);
    esl_sched_set_time(&sched, T0, 0, ESL_TIME_NTP);

    /* tag 2: a weekly, a daily and a one-off entry, all due while the AP was off for three
       days: the daily one was due last and wins; the recurring ones move on, the other goes */
    CHECK_INT(esl_sched_remove(&sched, 1, false), 1);
    at = T0 + 100;
    CHECK_INT(esl_sched_add_repeat(&sched, 2, &at, ESL_SCHED_WEEKLY, T0, &w), ESL_SCHED_OK);
    at = T0 + 200;
    CHECK_INT(esl_sched_add_repeat(&sched, 2, &at, ESL_SCHED_DAILY, T0, &d), ESL_SCHED_OK);
    CHECK_INT(esl_sched_add(&sched, 2, T0 + 300, &o), ESL_SCHED_OK);
    CHECK_INT(esl_sched_tick(&sched, &st, AT(T0 + 3 * DAY)), 1);
    CHECK(shows(2, "D"));
    CHECK_INT(esl_sched_count(&sched), 2);
    CHECK_INT(slot_at(2, ESL_SCHED_WEEKLY), T0 + 100 + WEEK);
    CHECK_INT(slot_at(2, ESL_SCHED_DAILY), T0 + 200 + 3 * DAY);
    /* the one-off one due last wins over the daily one */
    CHECK_INT(esl_sched_add(&sched, 2, T0 + 3 * DAY + 300, &o), ESL_SCHED_OK);
    CHECK_INT(esl_sched_tick(&sched, &st, AT(T0 + 3 * DAY + 300)), 1);
    CHECK(shows(2, "O"));
    CHECK_INT(slot_at(2, ESL_SCHED_DAILY), T0 + 200 + 4 * DAY);

    /* a one-off entry at the series' next time is its own entry and wins then; the series goes on */
    CHECK_INT(esl_sched_add(&sched, 2, T0 + 200 + 4 * DAY, &b), ESL_SCHED_OK);
    CHECK_INT(esl_sched_count(&sched), 3);
    CHECK(esl_sched_has(&sched, 2, T0 + 200 + 4 * DAY));
    CHECK_INT(esl_sched_tick(&sched, &st, AT(T0 + 200 + 4 * DAY)), 1);
    CHECK(shows(2, "B"));
    CHECK_INT(slot_at(2, ESL_SCHED_DAILY), T0 + 200 + 5 * DAY);
    /* the other way round: the series moves onto a one-off entry's time; the one-off wins */
    CHECK_INT(esl_sched_add(&sched, 2, T0 + 200 + 6 * DAY, &o), ESL_SCHED_OK);
    CHECK_INT(esl_sched_tick(&sched, &st, AT(T0 + 200 + 5 * DAY)), 1);
    CHECK(shows(2, "D"));
    CHECK_INT(esl_sched_tick(&sched, &st, AT(T0 + 200 + 6 * DAY)), 1);
    CHECK(shows(2, "O"));
    CHECK_INT(slot_at(2, ESL_SCHED_DAILY), T0 + 200 + 7 * DAY);

    /* a recurring entry whose first time has passed starts at the next one */
    at = T0 - 3600;
    CHECK_INT(esl_sched_add_repeat(&sched, 3, &at, ESL_SCHED_DAILY, T0 + 3 * DAY, &a), ESL_SCHED_OK);
    CHECK_INT(at, T0 - 3600 + 4 * DAY);
    /* daily over the change of offset: 11:00 UTC (13:00 local) becomes 12:00 UTC */
    reset();
    esl_sched_init(&sched, entries, CAP, &tz_ops);
    esl_sched_set_time(&sched, T0, 0, ESL_TIME_NTP);
    at = T0 + 9 * DAY - 3600;
    CHECK_INT(esl_sched_add_repeat(&sched, 4, &at, ESL_SCHED_DAILY, T0, &a), ESL_SCHED_OK);
    CHECK_INT(esl_sched_tick(&sched, &st, AT(T0 + 9 * DAY - 3600)), 1);
    CHECK_INT(slot_at(4, ESL_SCHED_DAILY), T0 + 10 * DAY - 3600); /* the change is at 12:00 UTC that day */
    CHECK_INT(esl_sched_tick(&sched, &st, AT(T0 + 10 * DAY - 3600)), 1);
    CHECK_INT(slot_at(4, ESL_SCHED_DAILY), T0 + 11 * DAY);
}

static void test_api_repeat(void)
{
    reset();
    esl_sched_set_time(&sched, T0, 1000, ESL_TIME_NTP);
    /* every week from an hour ago: starts in a week less an hour */
    CHECK_INT(req("PUT", "/api/tags/" MAC1,
                  "{\"layout\":\"price\",\"fields\":[\"Fredag\"],\"at\":1792062000,\"repeat\":\"weekly\"}", 1000), 200);
    CHECK_STR(resp, "{\"mac\":\"" MAC1 "\",\"scheduled\":true,\"at\":1792666800,\"repeat\":\"weekly\"}");
    CHECK(!shows(1, "Fredag"));
    CHECK_INT(req("GET", "/api/schedule", NULL, 1000), 200);
    CHECK(HAS(resp, "{\"mac\":\"" MAC1 "\",\"at\":1792666800,\"repeat\":\"weekly\",\"layout\":\"price\""));
    CHECK_INT(req("PUT", "/api/tags/" MAC1, "{\"layout\":\"price\",\"fields\":[\"A\"],\"repeat\":\"daily\"}", 1000), 422);
    CHECK(HAS(resp, "repeat needs a time in at"));
    CHECK_INT(req("PUT", "/api/tags/" MAC1,
                  "{\"layout\":\"price\",\"fields\":[\"A\"],\"at\":1792069200,\"repeat\":\"monthly\"}", 1000), 422);
    /* "once" and null are as without it; a past time is now */
    CHECK_INT(req("PUT", "/api/tags/" MAC2,
                  "{\"layout\":\"price\",\"fields\":[\"B\"],\"at\":1792069200,\"repeat\":\"once\"}", 1000), 200);
    CHECK_STR(resp, "{\"mac\":\"" MAC2 "\",\"scheduled\":true,\"at\":1792069200}");
    CHECK_INT(req("PUT", "/api/tags/" MAC2, "{\"layout\":\"price\",\"fields\":[\"C\"],\"at\":1792000000,\"repeat\":null}", 1000),
              200);
    CHECK(shows(2, "C"));
    /* batch */
    CHECK_INT(req("PUT", "/api/tags",
                  "{\"tags\":[{\"mac\":\"" MAC2 "\",\"layout\":\"price\",\"fields\":[\"D\"],\"at\":1792069200,"
                  "\"repeat\":\"daily\"}]}", 1000), 200);
    CHECK(HAS(resp, "\"at\":1792069200,\"repeat\":\"daily\"}"));
    CHECK_INT(esl_sched_tick(&sched, &st, 1000 + 3600 * 1000), 1);
    CHECK(shows(2, "B")); /* the one-off one at the same time wins; the daily one goes on */
    CHECK_INT(esl_sched_count(&sched), 2); /* the daily one and the weekly one */
    /* a full schedule: sending the same recurring entries again (a restore, with the times
       first sent) replaces them, so it takes no room */
    CHECK_INT(req("PUT", "/api/tags", "{\"tags\":[{\"mac\":\"0000000000000003\",\"layout\":\"text\",\"fields\":[\"x\"],\"at\":1792900000},"
                  "{\"mac\":\"0000000000000004\",\"layout\":\"text\",\"fields\":[\"x\"],\"at\":1792900000}]}", 1000), 200);
    CHECK_INT(esl_sched_free(&sched), 0);
    const char *again = "{\"tags\":[{\"mac\":\"" MAC1 "\",\"layout\":\"price\",\"fields\":[\"Fredag\"],\"at\":1792062000,\"repeat\":\"weekly\"},"
                        "{\"mac\":\"" MAC2 "\",\"layout\":\"price\",\"fields\":[\"D\"],\"at\":1792069200,\"repeat\":\"daily\"}]}";
    CHECK_INT(req("PUT", "/api/tags", again, 1000 + 3600 * 1000), 200);
    CHECK(!HAS(resp, "error"));
    CHECK_INT(esl_sched_count(&sched), 4);
}

void test_sched(void)
{
    esl_sb_init(&out, resp, sizeof(resp));
    test_clock();
    test_core();
    test_wake();
    test_api_time();
    test_api_schedule();
    test_repeat();
    test_api_repeat();
    test_api_restored_tag();
}
