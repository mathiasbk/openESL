/* test_source.c - esl_source: data sources (the configuration, /api/sources, MET Norway's
 * forecast as values, and the content of the tags) */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "tinytest.h"
#include "esl_api.h"
#include "esl_source.h"
#include "esl_crc32.h"
#include "esl_image.h"
#include "esl_png.h"
#include "esl_render.h"

#define CAP 8
#define AUTH "Bearer s3cret"
#define MAC1 "00000123456789A8"
#define MAC2 "00000123456789B9"

/* 2026-10-26T16:00:00Z, a Monday: the first hour of the test forecast */
#define T0 1793030400u

static esl_store_t st;
static esl_layouts_t layouts;
static esl_sources_t sources;
static esl_sched_entry_t entries[4];
static esl_sched_t sched;
static esl_api_t api = {&st, "s3cret", "0.1.0", &layouts, NULL, NULL, &sched, NULL, NULL, NULL, NULL, NULL,
                        NULL, &sources, NULL, false, NULL, NULL, 0, NULL, NULL, NULL};
static char resp[ESL_API_RESP_SIZE(CAP)];
static esl_sb_t out;
static char saved[ESL_SOURCE_CFG_MAX];
static int saves, wakes;
static bool save_fails;

static bool persist(void *ctx, const char *js, size_t len)
{
    (void)ctx;
    if (save_fails)
        return false;
    memcpy(saved, js, len);
    saved[len] = '\0';
    saves++;
    return true;
}

static void wake(void *ctx)
{
    (void)ctx;
    wakes++;
}

static int req(const char *method, const char *uri, const char *body)
{
    esl_sb_init(&out, resp, sizeof(resp));
    return esl_api_handle(&api, method, uri, AUTH, body, body ? strlen(body) : 0, 1000, &out);
}

/* Central European time: +1 h, +2 h from the last Sunday of March to the last of October
   (01:00 UTC). Enough for the dates in these tests. */
static int32_t cet(uint32_t t)
{
    return t >= 1774746000u && t < 1792890000u ? 7200 : 3600; /* 2026-03-29, 2026-10-25 */
}

/* A forecast in MET's compact format from T0: hourly for 60 hours, then every 6 hours to
   9 days. The temperature is 8 at 16:00 UTC and falls 1 degree an hour to the night, the
   weather goes from clear to showers to snow. */
static char *met_json(uint32_t start, unsigned hourly, unsigned six, size_t *len)
{
    static const char *const syms[] = {"clearsky_day", "partlycloudy_night", "lightrainshowers_day",
                                       "heavyrainandthunder", "cloudy", "snow", "fog", "fair_night"};
    size_t cap = 700 * (hourly + six) + 1024, n = 0;
    char *b = malloc(cap);
    n += (size_t)snprintf(b + n, cap - n,
                          "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[10.7522,59.9139,14]},"
                          "\"properties\":{\"meta\":{\"updated_at\":\"2026-10-26T15:42:07Z\",\"units\":{"
                          "\"air_temperature\":\"celsius\",\"precipitation_amount\":\"mm\",\"wind_speed\":\"m/s\"}},"
                          "\"timeseries\":[");
    for (unsigned i = 0; i < hourly + six; i++) {
        uint32_t t = i < hourly ? start + i * 3600 : start + hourly * 3600 + (i - hourly) * 6 * 3600;
        uint32_t d = t / 86400, s = t % 86400;
        /* civil date from days (2026-10 to 2026-11 only) */
        unsigned day = d - 20752 + 26, mon = 10;
        if (day > 31) {
            day -= 31;
            mon = 11;
        }
        double temp = 8.0 - (double)(i < hourly ? i : hourly + (i - hourly) * 6) * 0.5;
        const char *sym = syms[(i / 3) % 8];
        n += (size_t)snprintf(b + n, cap - n,
                              "%s{\"time\":\"2026-%02u-%02uT%02u:00:00Z\",\"data\":{\"instant\":{\"details\":{"
                              "\"air_pressure_at_sea_level\":1012.4,\"air_temperature\":%.1f,\"cloud_area_fraction\":"
                              "88.3,\"relative_humidity\":81.6,\"wind_from_direction\":%u.4,\"wind_speed\":%u.6}},"
                              "\"next_12_hours\":{\"summary\":{\"symbol_code\":\"%s\"},\"details\":{}}",
                              i ? "," : "", mon, day, (unsigned)(s / 3600), temp, (i * 20) % 360, 3 + i % 4, sym);
        if (i < hourly)
            n += (size_t)snprintf(b + n, cap - n,
                                  ",\"next_1_hours\":{\"summary\":{\"symbol_code\":\"%s\"},\"details\":{"
                                  "\"precipitation_amount\":%u.%u}}",
                                  sym, i % 3, i % 10);
        n += (size_t)snprintf(b + n, cap - n,
                              ",\"next_6_hours\":{\"summary\":{\"symbol_code\":\"%s\"},\"details\":{"
                              "\"precipitation_amount\":%u.%u}}}}",
                              sym, (i % 5) * 3, i % 10);
    }
    n += (size_t)snprintf(b + n, cap - n, "]}}");
    *len = n;
    return b;
}

static const char CFG[] =
    "{\"sources\":[{\"id\":\"hjemme\",\"type\":\"met\",\"lat\":59.91391,\"lon\":10.7522,\"place\":\"Oslo\","
    "\"layout\":\"weather\",\"tags\":[\"" MAC1 "\"]}]}";

static void test_parse(void)
{
    static esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    char err[ESL_SOURCE_ERR_MAX + 16];
    uint8_t n;

    CHECK(esl_sources_parse(CFG, sizeof(CFG) - 1, cfg, &n, err, sizeof(err)));
    CHECK_INT(n, 1);
    CHECK_STR(cfg[0].id, "hjemme");
    CHECK_INT(cfg[0].type, ESL_SOURCE_MET);
    CHECK_INT(cfg[0].met.lat, 599139);
    CHECK_INT(cfg[0].met.lon, 107522);
    CHECK_STR(cfg[0].lang, "nb");
    CHECK_INT(cfg[0].every_min, 30);
    CHECK_INT(cfg[0].nfields, 6); /* the weather template's */
    CHECK_STR(cfg[0].fields[4], "{min}° / {max}°");
    CHECK_INT(cfg[0].ntags, 1);
    CHECK(cfg[0].tags[0] == 0x00000123456789A8ull);

    /* written back, it reads the same */
    {
        static char buf[2048];
        esl_sb_t sb;
        esl_sb_init(&sb, buf, sizeof(buf));
        esl_sb_puts(&sb, "{\"sources\":[");
        esl_source_write_cfg(&cfg[0], &sb);
        esl_sb_puts(&sb, "]}");
        CHECK(HAS(buf, "\"lat\":59.9139,\"lon\":10.7522"));
        static esl_source_cfg_t again[ESL_SOURCE_MAX];
        CHECK(esl_sources_parse(buf, sb.len, again, &n, err, sizeof(err)));
        CHECK(n == 1 && memcmp(&again[0], &cfg[0], sizeof(cfg[0])) == 0);
    }
    /* south and west, own fields, "text" */
    const char *ok = "{\"sources\":[{\"id\":\"a-1\",\"type\":\"met\",\"lat\":-33.8688,\"lon\":-151.2,\"lang\":\"en\","
                     "\"every_min\":60,\"layout\":\"text\",\"fields\":[\"{temp} C\",\"{desc}\"],\"tags\":[],"
                     "\"status\":{\"ok\":true}}]}";
    CHECK(esl_sources_parse(ok, strlen(ok), cfg, &n, err, sizeof(err)));
    CHECK(cfg[0].met.lat == -338688 && cfg[0].met.lon == -1512000 && cfg[0].nfields == 2 && cfg[0].every_min == 60);
    CHECK(esl_sources_parse("{\"sources\":[]}", 14, cfg, &n, err, sizeof(err)) && n == 0);

    static const struct {
        const char *js, *msg;
    } bad[] = {
        {"[]", "must be a JSON object"},
        {"{}", "sources: a list of up to 8"},
        {"{\"sources\":[{\"id\":\"A\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\"}]}", "sources[0].id: 1 to 16"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"caldav\",\"lat\":1,\"lon\":1,\"layout\":\"w\"}]}", "sources[0].type: one of \"met\", \"image\", \"power\", \"countdown\", \"rss\", \"ical\""},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":91,\"lon\":1,\"layout\":\"w\"}]}", "lat: a number from -90"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"layout\":\"w\"}]}", "lon: a number from -180"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\",\"every_min\":10}]}",
         "every_min: a whole number from 30 to 1440"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\",\"every_min\":45.5}]}",
         "every_min"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"price\"}]}", "layout:"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\",\"fields\":[\"{nope}\"]}]}",
         "fields[0]: {nope} is not a value of type met"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\",\"fields\":[\"{temp\"]}]}",
         "a { with no } after it"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\",\"fields\":[]}]}",
         "fields: a list of 1 to 6"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\",\"tags\":[\"12\"]}]}",
         "tags[0]: a MAC"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\",\"tags\":[\"" MAC1 "\",\"" MAC1
         "\"]}]}",
         "tags[1]: the same tag twice"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\",\"color\":1}]}",
         "color: unknown key"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\",\"lang\":\"de\"}]}", "lang"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\"},"
         "{\"id\":\"a\",\"type\":\"met\",\"lat\":2,\"lon\":2,\"layout\":\"w\"}]}",
         "sources[1].id: \"a\" is used twice"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\",\"tags\":[\"00000000000000A1\"]},"
         "{\"id\":\"b\",\"type\":\"met\",\"lat\":2,\"lon\":2,\"layout\":\"w\",\"tags\":[\"1\",\"a1\"]}]}",
         "sources[1].tags[0]: a MAC"},
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"met\",\"lat\":1,\"lon\":1,\"layout\":\"w\",\"tags\":[\"00000000000000A1\"]},"
         "{\"id\":\"b\",\"type\":\"met\",\"lat\":2,\"lon\":2,\"layout\":\"w\",\"tags\":[\"00000000000000a1\"]}]}",
         "sources[1].tags[0]: 00000000000000A1 is already in source \"a\""},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!esl_sources_parse(bad[i].js, strlen(bad[i].js), cfg, &n, err, sizeof(err)));
        if (!HAS(err, bad[i].msg))
            printf("    %s\n    -> %s\n", bad[i].js, err);
        CHECK(HAS(err, bad[i].msg));
    }

    char url[160];
    CHECK(esl_sources_parse(CFG, sizeof(CFG) - 1, cfg, &n, err, sizeof(err)));
    CHECK(esl_source_url(&cfg[0], url, sizeof(url)) > 0);
    CHECK_STR(url, "https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=59.9139&lon=10.7522");
    cfg[0].met.lat = -5;
    cfg[0].met.lon = -1234567;
    esl_source_url(&cfg[0], url, sizeof(url));
    CHECK(HAS(url, "lat=-0.0005&lon=-123.4567"));
    CHECK(esl_source_url(&cfg[0], url, 20) == 0);
}

static void test_times(void)
{
    CHECK(esl_iso_time("2026-10-26T16:00:00Z", 20) == T0);
    CHECK(esl_iso_time("1970-01-01T00:00:00Z", 20) == 0);
    CHECK(esl_iso_time("2026-10-26 16:00:00Z", 20) == 0);
    CHECK(esl_iso_time("2026-13-26T16:00:00Z", 20) == 0);
    CHECK(esl_http_date("Mon, 26 Oct 2026 16:30:05 GMT") == T0 + 1805);
    CHECK(esl_http_date("Mon, 26 Okt 2026 16:30:05 GMT") == 0);
    CHECK(esl_http_date("26 Oct 2026") == 0);
    CHECK(esl_http_date(NULL) == 0);
    CHECK(esl_iso_time("2024-02-29T00:00:00Z", 20) == 1709164800u);
}

static void test_met(void)
{
    static esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    char err[ESL_SOURCE_ERR_MAX + 16];
    uint8_t n;
    esl_values_t v;
    uint32_t data_at;
    size_t len;
    char *js = met_json(T0, 60, 28, &len);

    CHECK(len > 20000 && len < 65535);
    CHECK(esl_sources_parse(CFG, sizeof(CFG) - 1, cfg, &n, err, sizeof(err)));
    /* 17:30 local (16:30 UTC): the 16:00 entry */
    CHECK(esl_source_values(&cfg[0], js, len, T0 + 1800, cet, &v, &data_at, err, sizeof(err)));
    CHECK(data_at == T0 - 18 * 60 + 7);
    CHECK_STR(esl_values_get(&v, "place"), "Oslo");
    CHECK_STR(esl_values_get(&v, "temp"), "8");
    CHECK_STR(esl_values_get(&v, "symbol"), "sun");
    CHECK_STR(esl_values_get(&v, "desc"), "Klarvær");
    CHECK_STR(esl_values_get(&v, "wind"), "4");      /* 3.6 m/s */
    CHECK_STR(esl_values_get(&v, "dir"), "N");       /* from 0.4 degrees */
    CHECK_STR(esl_values_get(&v, "rain"), "0");
    CHECK_STR(esl_values_get(&v, "rain1h"), "0");
    CHECK_STR(esl_values_get(&v, "humidity"), "82");
    /* 24 hours: 8 down to 8 - 23 * 0.5 = -3.5, which rounds away from zero */
    CHECK_STR(esl_values_get(&v, "max"), "8");
    CHECK_STR(esl_values_get(&v, "min"), "-4");
    /* in 3 hours: 19 UTC, 20 local, 6.5 degrees, partly cloudy (night); in 6 rain showers */
    CHECK_STR(esl_values_get(&v, "h3"), "20");
    CHECK_STR(esl_values_get(&v, "t3"), "7");
    CHECK_STR(esl_values_get(&v, "s3"), "partmoon");
    CHECK_STR(esl_values_get(&v, "h6"), "23");
    CHECK_STR(esl_values_get(&v, "s6"), "rain");
    CHECK_STR(esl_values_get(&v, "updated"), "16:42");
    /* tomorrow (Tuesday, local): 23:00 UTC Monday to 22:00 UTC Tuesday */
    CHECK_STR(esl_values_get(&v, "tmax"), "5");   /* 23 UTC: 8 - 7 * 0.5 = 4.5 */
    CHECK_STR(esl_values_get(&v, "tmin"), "-7");  /* 22 UTC: 8 - 30 * 0.5 = -7 */
    CHECK(esl_values_get(&v, "tsym") != NULL);
    CHECK(esl_values_get(&v, "nope") == NULL);

    /* two hours on: the 18:00 entry, which has rain in the next hour and next 6 */
    CHECK(esl_source_values(&cfg[0], js, len, T0 + 2 * 3600 + 59, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "temp"), "7");
    CHECK_STR(esl_values_get(&v, "rain"), "6,2");
    CHECK_STR(esl_values_get(&v, "rain1h"), "2,2");
    CHECK_STR(esl_values_get(&v, "dir"), "NØ"); /* 40.4 */
    /* thunder and showers in words, in English too */
    CHECK(esl_source_values(&cfg[0], js, len, T0 + 9 * 3600, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "desc"), "Kraftig regn og torden");
    CHECK_STR(esl_values_get(&v, "symbol"), "thunder");
    strcpy(cfg[0].lang, "en");
    CHECK(esl_source_values(&cfg[0], js, len, T0 + 9 * 3600, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "desc"), "Heavy rain and thunder");
    CHECK(esl_source_values(&cfg[0], js, len, T0 + 3 * 3600, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "desc"), "Partly cloudy");
    CHECK_STR(esl_values_get(&v, "symbol"), "partmoon");
    strcpy(cfg[0].lang, "nb");
    /* before the first entry: the first; long after the last: an error */
    CHECK(esl_source_values(&cfg[0], js, len, T0 - 600, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "temp"), "8");
    CHECK(!esl_source_values(&cfg[0], js, len, T0 + 12 * 86400, cet, &v, &data_at, err, sizeof(err)));
    CHECK(HAS(err, "the forecast is old"));
    CHECK(!esl_source_values(&cfg[0], "<html>", 6, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK(HAS(err, "not JSON"));
    CHECK(!esl_source_values(&cfg[0], "{\"properties\":{}}", 17, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK(HAS(err, "no properties.timeseries"));
    free(js);

    /* an answer over 64 KB is cut after the last whole entry that fits */
    js = met_json(T0, 200, 0, &len);
    CHECK(len > 65535);
    CHECK(esl_source_values(&cfg[0], js, len, T0 + 1800, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "temp"), "8");
    CHECK_STR(esl_values_get(&v, "min"), "-4");
    free(js);
}

static void test_content(void)
{
    static esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    char err[ESL_SOURCE_ERR_MAX + 16];
    uint8_t n, len;
    esl_values_t v = {0};
    esl_content_t c;

    CHECK(esl_sources_parse(CFG, sizeof(CFG) - 1, cfg, &n, err, sizeof(err)));
    esl_values_set(&v, "place", "Tromsø");
    esl_values_set(&v, "temp", "-3");
    esl_values_set(&v, "symbol", "snow");
    esl_values_set(&v, "desc", "Lett snø");
    esl_values_set(&v, "min", "-5");
    esl_values_set(&v, "max", "-1");
    esl_values_set(&v, "wind", "7");
    esl_values_set(&v, "rain", "1,2");
    esl_values_set(&v, "temp", "-4"); /* set again: replaced */
    CHECK_INT(v.n, 8);
    CHECK(esl_source_content(&cfg[0], &v, &c, err, sizeof(err)));
    CHECK_STR(c.name, "weather");
    CHECK_INT(c.nfields, 6);
    const uint8_t *f = esl_content_field(&c, 0, &len);
    CHECK(len == 6 && memcmp(f, "Troms\xf8", 6) == 0);
    f = esl_content_field(&c, 1, &len);
    CHECK(len == 2 && memcmp(f, "-4", 2) == 0);
    f = esl_content_field(&c, 4, &len);
    CHECK(len == 9 && memcmp(f, "-5\xb0 / -1\xb0", 9) == 0);
    f = esl_content_field(&c, 5, &len);
    CHECK(len == 13 && memcmp(f, "7 m/s  1,2 mm", 13) == 0);
    /* a value the answer did not have is empty; empty fields at the end are dropped */
    strcpy(cfg[0].fields[0], "{dir}");
    strcpy(cfg[0].fields[1], "x{h3}y");
    cfg[0].nfields = 3;
    strcpy(cfg[0].fields[2], "{h6}");
    CHECK(esl_source_content(&cfg[0], &v, &c, err, sizeof(err)));
    CHECK_INT(c.nfields, 2);
    f = esl_content_field(&c, 1, &len);
    CHECK(len == 2 && memcmp(f, "xy", 2) == 0);
    /* too long for a tag */
    esl_values_set(&v, "desc", "0123456789012345678901234567890123456789");
    for (int i = 0; i < 4; i++)
        strcpy(cfg[0].fields[i], "{desc}{desc}{desc}");
    cfg[0].nfields = 4;
    CHECK(!esl_source_content(&cfg[0], &v, &c, err, sizeof(err)));
    CHECK(HAS(err, "longer than a tag's 256 bytes"));
    /* the built-in text layout */
    strcpy(cfg[0].layout, "text");
    cfg[0].nfields = 1;
    strcpy(cfg[0].fields[0], "{temp}°");
    CHECK(esl_source_content(&cfg[0], &v, &c, err, sizeof(err)));
    CHECK_INT(c.layout, ESL_LAYOUT_TEXT);
}

static const char LAYOUT_29[] =
    "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":[{\"name\":\"weather\","
    "\"fields\":[\"place\",\"temp\",\"symbol\",\"desc\",\"range\",\"detail\"],\"elements\":["
    "{\"type\":\"text\",\"field\":\"place\",\"x\":0,\"y\":0,\"w\":296,\"h\":30,\"fonts\":[\"medium\"]}]}]}";

static void poll(uint64_t mac, uint8_t model)
{
    uint8_t reply[ESL_MAX_PAYLOAD];
    esl_layout_env_t env;
    esl_poll_t p = {2, 2950, 0, ESL_POLL_F_IMAGE, model, true, 0, false, 0, 0, 0, 0, false, 0};
    esl_layouts_env(&layouts, model, &env);
    esl_store_on_poll(&st, mac, &p, -61, 1000, &env, reply);
}

static void test_apply(void)
{
    static esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    static esl_layout_bin_t bin;
    char err[ESL_SOURCE_ERR_MAX + 16];
    uint8_t n;
    esl_source_state_t run;
    esl_values_t v = {0};
    bool changed;

    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    esl_layouts_init(&layouts, NULL);
    CHECK(esl_layout_compile(LAYOUT_29, sizeof(LAYOUT_29) - 1, ESL_MODEL_29, &bin, err, sizeof(err)));
    CHECK(esl_layouts_set(&layouts, ESL_MODEL_29, LAYOUT_29, sizeof(LAYOUT_29) - 1, &bin, &changed));
    CHECK(esl_sources_parse(CFG, sizeof(CFG) - 1, cfg, &n, err, sizeof(err)));
    cfg[0].tags[cfg[0].ntags++] = 0x00000123456789B9ull;
    esl_values_set(&v, "place", "Oslo");
    esl_values_set(&v, "temp", "8");

    /* MAC1 a 2.9", MAC2 not heard yet: both get it */
    poll(0x00000123456789A8ull, ESL_MODEL_29);
    esl_source_apply(&cfg[0], &v, &st, &layouts, NULL, &run);
    CHECK(run.tags_set == 2 && run.tags_changed == 2 && run.tags_failed == 0 && !run.error[0]);
    esl_tag_t t;
    CHECK(esl_store_get(&st, 0x00000123456789A8ull, &t) && t.has_content);
    /* the same values again: nothing changes, so the tags are not sent anything */
    esl_source_apply(&cfg[0], &v, &st, &layouts, NULL, &run);
    CHECK(run.tags_set == 2 && run.tags_changed == 0);
    esl_values_set(&v, "temp", "9");
    esl_source_apply(&cfg[0], &v, &st, &layouts, NULL, &run);
    CHECK(run.tags_changed == 2);
    /* MAC2 got other content while the source was fetched: its run (a copy of the
       configuration from before) leaves MAC2 alone */
    static esl_sources_t s;
    esl_sources_init(&s, NULL);
    s.cfg[s.n++] = cfg[0];
    CHECK(esl_sources_release(&s, 0x00000123456789B9ull));
    esl_values_set(&v, "temp", "10");
    esl_source_apply(&cfg[0], &v, &st, &layouts, &s, &run);
    CHECK(run.tags_set == 1 && run.tags_changed == 1 && run.tags_failed == 0);
    /* a 1.54" has no such layout here */
    poll(0x00000123456789B9ull, ESL_MODEL_154);
    esl_source_apply(&cfg[0], &v, &st, &layouts, NULL, &run);
    CHECK(run.tags_set == 1 && run.tags_failed == 1);
    CHECK_STR(run.error, "layout \"weather\" is not in the layout file for model 1.54");
    /* content too long: every tag fails */
    esl_values_set(&v, "desc", "0123456789012345678901234567890123456789");
    for (int i = 0; i < 3; i++)
        strcpy(cfg[0].fields[i], "{desc}{desc}");
    esl_source_apply(&cfg[0], &v, &st, &layouts, NULL, &run);
    CHECK(run.tags_set == 0 && run.tags_failed == 2 && HAS(run.error, "longer than"));
    esl_layouts_free(&layouts);
}

static void test_api(void)
{
    static const esl_sources_ops_t ops = {NULL, NULL, NULL, persist, wake};
    esl_source_cfg_t cfg;
    char lm[40];
    bool fetch;

    esl_sources_init(&sources, &ops);
    static const esl_sched_ops_t sched_ops = {0};
    esl_sched_init(&sched, entries, 4, &sched_ops);
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    saves = wakes = 0;

    CHECK_INT(req("GET", "/api/sources", NULL), 200);
    CHECK(HAS(resp, "\"types\":{\"met\":{\"values\":[\"place\",\"temp\","));
    CHECK(HAS(resp, "\"fields\":[\"{place}\",\"{temp}\",\"{symbol}\""));
    CHECK(HAS(resp, "\"sources\":[]}"));
    CHECK_INT(req("GET", "/api/sources/", NULL), 200);
    CHECK_INT(req("DELETE", "/api/sources", NULL), 405);
    CHECK_INT(req("GET", "/api/sources/x", NULL), 404);
    CHECK_INT(req("GET", "/api/sources/refresh", NULL), 405);
    CHECK_INT(req("GET", "/api/sourcesx", NULL), 404);
    CHECK_INT(esl_api_handle(&api, "GET", "/api/sources", NULL, NULL, 0, 1000, &out), 401);

    /* nothing is due without sources */
    CHECK(esl_sources_due(&sources, T0, &cfg, lm, sizeof(lm), &fetch) < 0);
    CHECK(esl_sources_wait(&sources, T0, 60) == 60);

    CHECK_INT(req("PUT", "/api/sources", "{\"sources\":[{\"id\":\"x\"}]}"), 422);
    CHECK(HAS(resp, "sources[0].type"));
    CHECK_INT(req("PUT", "/api/sources", NULL), 400);
    CHECK_INT(saves, 0);
    save_fails = true;
    CHECK_INT(req("PUT", "/api/sources", CFG), 507);
    save_fails = false;
    CHECK_INT(sources.n, 0);
    CHECK_INT(req("PUT", "/api/sources", CFG), 200);
    CHECK(saves == 1 && wakes == 1 && sources.n == 1);
    CHECK(HAS(saved, "{\"sources\":[{\"id\":\"hjemme\",\"type\":\"met\",\"lat\":59.9139,\"lon\":10.7522,"));
    CHECK(!HAS(saved, "status"));
    CHECK(HAS(resp, "\"tags\":[\"" MAC1 "\"],\"status\":{\"ok\":false,\"fetched_at\":null"));

    /* due at once; handed out once */
    CHECK(esl_sources_wait(&sources, T0, 60) == 0);
    CHECK_INT(esl_sources_due(&sources, T0, &cfg, lm, sizeof(lm), &fetch), 0);
    CHECK(fetch && !strcmp(cfg.id, "hjemme") && !lm[0]);
    CHECK(esl_sources_due(&sources, T0, &cfg, lm, sizeof(lm), &fetch) < 0);
    /* an answer: next fetch after every_min, or at Expires when that is later */
    esl_values_t v = {0};
    esl_source_state_t run = {0};
    esl_values_set(&v, "temp", "8");
    run.tags_set = run.tags_changed = 1;
    esl_sources_done(&sources, 0, "hjemme", T0, 200, T0 + 3600, "Mon, 26 Oct 2026 15:42:07 GMT", &v, T0 - 1000,
                     &run, NULL);
    CHECK(sources.state[0].next_at == T0 + 3600 && sources.state[0].fetched_at == T0);
    CHECK(esl_sources_wait(&sources, T0 + 600, 7200) == 3000);
    CHECK_INT(req("GET", "/api/sources", NULL), 200);
    CHECK(HAS(resp, "\"status\":{\"ok\":true,\"fetched_at\":1793030400,\"tried_at\":1793030400,\"data_at\":1793029400,"
                    "\"next_at\":1793034000,\"http\":200,\"tags_set\":1,\"tags_changed\":1,\"tags_failed\":0,"
                    "\"error\":null,\"values\":{\"temp\":\"8\"}}"));
    /* a refresh before Expires uses the last answer again */
    CHECK_INT(req("POST", "/api/sources/refresh", NULL), 202);
    CHECK_STR(resp, "{\"refreshing\":1}");
    CHECK_INT(esl_sources_due(&sources, T0 + 1000, &cfg, lm, sizeof(lm), &fetch), 0);
    CHECK(!fetch && !strcmp(lm, "Mon, 26 Oct 2026 15:42:07 GMT"));
    /* after Expires a refresh fetches */
    esl_sched_set_time(&sched, T0 + 3600, 1000, ESL_TIME_API);
    CHECK_INT(req("POST", "/api/sources/refresh", NULL), 202);
    CHECK_INT(esl_sources_due(&sources, T0 + 1000, &cfg, lm, sizeof(lm), &fetch), 0);
    CHECK(fetch);
    /* 304 keeps Last-Modified; no answer tries again in 1, 3, then 10 minutes with the error */
    esl_sources_done(&sources, 0, "hjemme", T0 + 4000, 304, 0, NULL, &v, T0 - 1000, &run, NULL);
    CHECK(sources.state[0].next_at == T0 + 4000 + 1800);
    CHECK(!strcmp(sources.state[0].last_modified, "Mon, 26 Oct 2026 15:42:07 GMT"));
    esl_sources_done(&sources, 0, "hjemme", T0 + 9000, 0, 0, NULL, NULL, 0, NULL, "connect failed");
    CHECK(sources.state[0].next_at == T0 + 9060 && sources.state[0].tried_at == T0 + 9000);
    CHECK(sources.state[0].http == 0);
    esl_sources_done(&sources, 0, "hjemme", T0 + 9060, 0, 0, NULL, NULL, 0, NULL, "connect failed");
    CHECK(sources.state[0].next_at == T0 + 9240);
    esl_sources_done(&sources, 0, "hjemme", T0 + 9240, 0, 0, NULL, NULL, 0, NULL, "connect failed");
    CHECK(sources.state[0].next_at == T0 + 9840);
    CHECK_INT(req("GET", "/api/sources", NULL), 200);
    CHECK(HAS(resp, "\"ok\":false") && HAS(resp, "\"error\":\"connect failed\",\"values\":{\"temp\":\"8\"}"));
    CHECK(HAS(resp, "\"tried_at\":1793039640"));
    /* an answer with an HTTP error: 10 minutes, and the count of tries without one starts over after data */
    esl_sources_done(&sources, 0, "hjemme", T0 + 9300, 503, 0, NULL, NULL, 0, NULL, "HTTP 503");
    CHECK(sources.state[0].next_at == T0 + 9900 && sources.state[0].http == 503);
    esl_sources_done(&sources, 0, "hjemme", T0 + 9400, 304, 0, NULL, &v, T0 - 1000, &run, NULL);
    CHECK(sources.state[0].fails == 0);
    esl_sources_done(&sources, 0, "hjemme", T0 + 9500, 0, 0, NULL, NULL, 0, NULL, "connect failed");
    CHECK(sources.state[0].next_at == T0 + 9560);
    /* 203 (a deprecated API version) is data, with the caller's warning as the error */
    esl_sources_done(&sources, 0, "hjemme", T0 + 9560, 203, T0 + 12000, "Mon, 26 Oct 2026 18:00:00 GMT", &v,
                     T0 - 1000, &run, "deprecated");
    CHECK(sources.state[0].fetched_at == T0 + 9560 && sources.state[0].next_at == T0 + 12000);
    CHECK(!strcmp(sources.state[0].last_modified, "Mon, 26 Oct 2026 18:00:00 GMT"));
    CHECK(!strcmp(sources.state[0].error, "deprecated") && sources.state[0].http == 203);
    /* another id at that index (the configuration changed meanwhile): ignored */
    esl_sources_done(&sources, 0, "other", T0 + 9000, 200, 0, NULL, NULL, 0, NULL, NULL);
    CHECK(sources.state[0].fetched_at == T0 + 9560);

    /* a new configuration keeps what is known of a source with the same id and place, and
       makes its content again from the last answer; another place starts over */
    CHECK_INT(req("PUT", "/api/sources", CFG), 200);
    CHECK(sources.state[0].fetched_at == T0 + 9560 && sources.state[0].reapply);
    CHECK_INT(esl_sources_due(&sources, T0 + 9100, &cfg, lm, sizeof(lm), &fetch), 0);
    CHECK(!fetch);
    const char *moved = "{\"sources\":[{\"id\":\"hjemme\",\"type\":\"met\",\"lat\":60,\"lon\":10.7522,"
                        "\"layout\":\"weather\"}]}";
    CHECK_INT(req("PUT", "/api/sources", moved), 200);
    CHECK(sources.state[0].fetched_at == 0 && !sources.state[0].last_modified[0]);
    CHECK_INT(esl_sources_due(&sources, T0 + 9100, &cfg, lm, sizeof(lm), &fetch), 0);
    CHECK(fetch);

    /* restored at boot from what was saved */
    esl_sources_init(&sources, &ops);
    CHECK(esl_sources_restore(&sources, saved, strlen(saved)));
    CHECK(sources.n == 1 && sources.cfg[0].met.lat == 600000);
    CHECK(!esl_sources_restore(&sources, "{", 1));

    /* a tag given other content leaves its source (saved), and a source left without tags goes */
    const char *two = "{\"sources\":[{\"id\":\"hjemme\",\"type\":\"met\",\"lat\":59.9,\"lon\":10.7,"
                      "\"layout\":\"weather\",\"tags\":[\"" MAC1 "\",\"0000000000000002\"]}]}";
    CHECK_INT(req("PUT", "/api/sources", two), 200);
    const char *text = "{\"layout\":\"text\",\"fields\":[\"Hei\"]}";
    CHECK_INT(req("PUT", "/api/tags/" MAC1, text), 200);
    CHECK(sources.n == 1 && sources.cfg[0].ntags == 1 && sources.cfg[0].tags[0] == 2);
    CHECK(HAS(saved, "\"tags\":[\"0000000000000002\"]"));
    CHECK(!esl_sources_release(&sources, 0x00000123456789A8ull));
    CHECK_INT(req("PUT", "/api/tags", "{\"tags\":[{\"mac\":\"0000000000000002\",\"layout\":\"text\",\"fields\":[\"Hei\"]}]}"), 200);
    CHECK(sources.n == 0 && !strcmp(saved, "{\"sources\":[]}"));

    /* no sources on this AP: 404 */
    api.sources = NULL;
    CHECK_INT(req("GET", "/api/sources", NULL), 404);
    api.sources = &sources;
    esl_store_free(&st);
}


/* ---- type "image": a PNG from a URL as the tags' picture ---- */

static uint8_t pics[CAP][ESL_IMAGE_STORED_MAX(5000)];
static size_t pic_len[CAP];

static bool pic_save(void *ctx, unsigned slot, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (slot >= CAP || len > sizeof(pics[0]))
        return false;
    if (data)
        memcpy(pics[slot], data, len);
    pic_len[slot] = data ? len : 0;
    return true;
}

static size_t pic_load(void *ctx, unsigned slot, uint8_t *buf, size_t cap)
{
    (void)ctx;
    if (slot >= CAP || !pic_len[slot] || pic_len[slot] > cap)
        return 0;
    memcpy(buf, pics[slot], pic_len[slot]);
    return pic_len[slot];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static size_t chunk(uint8_t *out, const char *type, const uint8_t *data, size_t len)
{
    put32(out, (uint32_t)len);
    memcpy(out + 4, type, 4);
    if (len)
        memcpy(out + 8, data, len);
    put32(out + 8 + len, esl_crc32(out + 4, 4 + len));
    return 12 + len;
}

/* An 8-bit grey PNG of w x h, black where (x / 8 + y / 8) is odd, in stored deflate blocks */
static size_t checker_png(uint8_t *out, uint32_t w, uint32_t h)
{
    static uint8_t rows[297 * 128], z[297 * 128 + 64];
    size_t n = 0, o = 0;
    for (uint32_t y = 0; y < h; y++) {
        rows[n++] = 0;
        for (uint32_t x = 0; x < w; x++)
            rows[n++] = ((x / 8 + y / 8) & 1) ? 0 : 255;
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
    put32(z + o, b << 16 | a);
    o += 4;
    uint8_t hdr[13] = {0};
    put32(hdr, w);
    put32(hdr + 4, h);
    hdr[8] = 8;
    size_t m = 8;
    memcpy(out, esl_png_signature, 8);
    m += chunk(out + m, "IHDR", hdr, 13);
    m += chunk(out + m, "IDAT", z, o);
    m += chunk(out + m, "IEND", NULL, 0);
    return m;
}

struct mem {
    const uint8_t *p;
    size_t left;
};

static bool mem_read(void *ctx, uint8_t *buf, size_t n)
{
    struct mem *m = ctx;
    if (n > m->left)
        return false;
    memcpy(buf, m->p, n);
    m->p += n;
    m->left -= n;
    return true;
}

static void test_image(void)
{
    static esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    static uint8_t png[297 * 128 + 256];
    char err[ESL_SOURCE_ERR_MAX + 16], url[ESL_SOURCE_URL_MAX];
    uint8_t n;

    /* the configuration: url and dither; no layout or fields */
    static const char ok[] = "{\"sources\":[{\"id\":\"chart\",\"type\":\"image\",\"url\":\"http://ha.local:8123/local/"
                             "chart.png\",\"dither\":true,\"every_min\":5,\"tags\":[\"" MAC1 "\",\"" MAC2 "\"]}]}";
    CHECK(esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));
    CHECK(n == 1 && cfg[0].type == ESL_SOURCE_IMAGE && cfg[0].image.dither && cfg[0].every_min == 5 && cfg[0].ntags == 2);
    CHECK(esl_source_is_picture(&cfg[0]));
    CHECK(esl_source_url(&cfg[0], url, sizeof(url)) > 0);
    CHECK_STR(url, "http://ha.local:8123/local/chart.png");
    {
        /* written as it is read, without a layout or fields */
        static char js[ESL_SOURCE_CFG_MAX];
        static esl_source_cfg_t again[ESL_SOURCE_MAX];
        esl_sb_t sb;
        esl_sb_init(&sb, js, sizeof(js));
        esl_sb_puts(&sb, "{\"sources\":[");
        esl_source_write_cfg(&cfg[0], &sb);
        esl_sb_puts(&sb, "]}");
        CHECK(!HAS(js, "layout") && !HAS(js, "fields") && HAS(js, "\"dither\":true"));
        CHECK(esl_sources_parse(js, sb.len, again, &n, err, sizeof(err)));
        CHECK(n == 1 && !strcmp(again[0].image.url, cfg[0].image.url) && again[0].image.dither);
    }
    static const struct {
        const char *src, *msg;
    } bad[] = {
        {"\"url\":\"ftp://x/a.png\"", "sources[0].url: an http:// or https:// URL"},
        {"\"url\":\"https://x/a b.png\"", "sources[0].url: "},
        {"\"url\":\"https:///a.png\"", "sources[0].url: "},
        {"\"url\":\"http://me:secret@ha.local/a.png\"", "sources[0].url: "},
        {"\"url\":7", "sources[0].url: "},
        {"\"lat\":1", "sources[0].lat: unknown key"},
        {"\"url\":\"https://x/a.png\",\"dither\":1", "sources[0].dither: true or false"},
        {"\"url\":\"https://x/a.png\",\"layout\":\"weather\"", "sources[0].layout: a source of type image gives"},
        {"\"url\":\"https://x/a.png\",\"fields\":[\"{temp}\"]", "sources[0].fields: a source of type image gives"},
        {"\"url\":\"https://x/a.png\",\"every_min\":4", "sources[0].every_min: a whole number from 5"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char js[256];
        snprintf(js, sizeof(js), "{\"sources\":[{\"id\":\"c\",\"type\":\"image\",%s}]}", bad[i].src);
        CHECK(!esl_sources_parse(js, strlen(js), cfg, &n, err, sizeof(err)));
        if (strncmp(err, bad[i].msg, strlen(bad[i].msg)) != 0)
            printf("  image cfg %zu: %s\n", i, err);
        CHECK(strncmp(err, bad[i].msg, strlen(bad[i].msg)) == 0);
    }
    CHECK(esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));

    /* the answer's values: its size, and only a PNG is taken */
    esl_values_t v;
    uint32_t data_at;
    size_t len = checker_png(png, 296, 128);
    CHECK(!esl_source_values(&cfg[0], "<html>", 6, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(err, "the answer is not a PNG file");
    CHECK(esl_source_values(&cfg[0], (const char *)png, len, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "width"), "296");
    CHECK_STR(esl_values_get(&v, "height"), "128");

    /* the picture on the tags: MAC1 a 2.9", MAC2 not heard yet */
    static const esl_store_ops_t ops = {.persist_image = pic_save, .load_image = pic_load};
    esl_source_state_t run;
    esl_tag_t t;
    esl_store_free(&st);
    memset(pic_len, 0, sizeof(pic_len));
    esl_store_init(&st, CAP, &ops, 0);
    esl_layouts_init(&layouts, NULL);
    poll(0x00000123456789A8ull, ESL_MODEL_29);
    esl_source_apply_body(&cfg[0], (const char *)png, len, &st, NULL, &run);
    CHECK(run.tags_set == 1 && run.tags_changed == 1 && run.tags_failed == 1);
    CHECK_STR(run.error, MAC2 ": a tag has not polled yet, so its screen is not known");
    CHECK(esl_store_get(&st, 0x00000123456789A8ull, &t) && t.has_content && t.has_image);
    {
        /* the same picture as PUT /api/tags/{mac}/image makes of this PNG (dithered) */
        static uint8_t want[5000], packed[ESL_IMAGE_STORED_MAX(5000)];
        size_t raw = esl_render_size(ESL_MODEL_29), ink = 0;
        struct mem m = {png, len};
        esl_content_t c, w;
        uint32_t crc;
        uint8_t model;
        CHECK(esl_image_picture(ESL_MODEL_29, true, len, mem_read, &m, want) == ESL_PIC_OK);
        esl_content_init_image(&w, ESL_MODEL_29,
                               esl_crc32(packed, esl_image_pack(0x00000123456789A8ull, ESL_MODEL_29, want, packed)));
        CHECK(esl_store_get_content(&st, 0x00000123456789A8ull, &c) && esl_content_image(&c, &model, &crc));
        CHECK(esl_content_equal(&c, &w));
        for (size_t i = 0; i < raw; i++)
            ink += want[i] != 0;
        CHECK(ink > raw / 4); /* the checkers */
    }
    /* the same picture again: not changed; a 1.54" gets a message about the size */
    esl_source_apply_body(&cfg[0], (const char *)png, len, &st, NULL, &run);
    CHECK(run.tags_set == 1 && run.tags_changed == 0);
    poll(0x00000123456789B9ull, ESL_MODEL_154);
    esl_source_apply_body(&cfg[0], (const char *)png, len, &st, NULL, &run);
    CHECK(run.tags_set == 1 && run.tags_failed == 1);
    CHECK_STR(run.error, MAC2 ": the picture is 296x128 pixels, the screen of model 1.54 is 200x200");
    /* a tag that left the source while it was fetched is left alone */
    static esl_sources_t s;
    esl_sources_init(&s, NULL);
    s.cfg[s.n++] = cfg[0];
    CHECK(esl_sources_release(&s, 0x00000123456789B9ull));
    esl_source_apply_body(&cfg[0], (const char *)png, len, &st, &s, &run);
    CHECK(run.tags_set == 1 && run.tags_failed == 0 && !run.error[0]);
    /* not a picture the decoder takes, and an AP that keeps no pictures */
    png[30] ^= 0xff;
    esl_source_apply_body(&cfg[0], (const char *)png, len, &st, NULL, &run);
    CHECK(run.tags_set == 0 && run.tags_failed == 2);
    png[30] ^= 0xff;
    esl_store_free(&st);
    esl_store_init(&st, CAP, NULL, 0);
    poll(0x00000123456789A8ull, ESL_MODEL_29);
    esl_source_apply_body(&cfg[0], (const char *)png, len, &st, NULL, &run);
    CHECK(run.tags_set == 0 && run.tags_failed == 2 && HAS(run.error, "keeps no pictures"));
    esl_layouts_free(&layouts);
}

/* ---- type "power": today's electricity price from hvakosterstrommen.no ---- */

/* A day of prices for 2026-10-27 (CET, +01:00) in steps of step_min minutes: NOK 1.00 at
   midnight, up 0.10 an hour to 23:00, except 0.20 from 03:00 to 04:00 (the cheapest) */
static char *power_json(unsigned step_min, size_t *len)
{
    size_t cap = 200 * (1440 / step_min) + 16, n = 0;
    char *b = malloc(cap);
    n += (size_t)snprintf(b + n, cap - n, "[");
    for (unsigned m = 0; m < 1440; m += step_min) {
        unsigned h = m / 60, e = m + step_min;
        double nok = h == 3 ? 0.2 : 1.0 + 0.1 * h;
        n += (size_t)snprintf(b + n, cap - n,
                              "%s{\"NOK_per_kWh\":%.5f,\"EUR_per_kWh\":0.1,\"EXR\":11.6,"
                              "\"time_start\":\"2026-10-27T%02u:%02u:00+01:00\",\"time_end\":\"2026-10-%02uT%02u:%02u:00+01:00\"}",
                              m ? "," : "", nok, h, m % 60, e == 1440 ? 28 : 27, e % 1440 / 60, e % 60);
    }
    n += (size_t)snprintf(b + n, cap - n, "]");
    *len = n;
    return b;
}

static void test_civil(int32_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)yoe + era * 400 + (*m <= 2);
}

/* A day of prices from start to end (Unix s) in steps of step_min, the times written in
   Norway's time with their offset as hvakosterstrommen.no does; NOK 1.00 + 0.01 per step */
static char *power_day(uint32_t start, uint32_t end, unsigned step_min, size_t *len)
{
    size_t cap = 200 * ((end - start) / 60 / step_min) + 16, n = 0;
    char *b = malloc(cap);
    n += (size_t)snprintf(b + n, cap - n, "[");
    unsigned i = 0;
    for (uint32_t t = start; t < end; t += step_min * 60, i++) {
        char ts[2][64];
        for (int k = 0; k < 2; k++) {
            uint32_t u = t + (uint32_t)k * step_min * 60;
            int32_t o = cet(u);
            uint32_t l = u + (uint32_t)o;
            int y;
            unsigned mo, d;
            test_civil((int32_t)(l / 86400), &y, &mo, &d);
            snprintf(ts[k], sizeof(ts[k]), "%04d-%02u-%02uT%02u:%02u:00+%02d:00", y, mo, d, (unsigned)(l % 86400 / 3600),
                     (unsigned)(l % 3600 / 60), (int)(o / 3600));
        }
        n += (size_t)snprintf(b + n, cap - n, "%s{\"NOK_per_kWh\":%.5f,\"time_start\":\"%s\",\"time_end\":\"%s\"}",
                              i ? "," : "", 1.0 + 0.01 * i, ts[0], ts[1]);
    }
    n += (size_t)snprintf(b + n, cap - n, "]");
    *len = n;
    return b;
}

static void test_power(void)
{
    static esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    char err[ESL_SOURCE_ERR_MAX + 16], url[ESL_SOURCE_URL_MAX];
    uint8_t n;
    static const char ok[] = "{\"sources\":[{\"id\":\"strom\",\"type\":\"power\",\"area\":\"NO1\",\"layout\":\"text\","
                             "\"tags\":[\"" MAC1 "\"]},{\"id\":\"nord\",\"type\":\"power\",\"area\":\"NO4\",\"lang\":\"en\","
                             "\"layout\":\"text\"}]}";
    CHECK(esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));
    CHECK(n == 2 && cfg[0].type == ESL_SOURCE_POWER && cfg[0].power.area == 1 && cfg[0].power.vat);
    CHECK(cfg[1].power.area == 4 && !cfg[1].power.vat); /* no VAT in Northern Norway */
    CHECK(cfg[0].every_min == 15 && cfg[0].nfields == 4 && esl_source_own_pace(&cfg[0]) && !esl_source_is_picture(&cfg[0]));
    CHECK(esl_source_url(&cfg[0], url, sizeof(url)) > 0);
    CHECK_STR(url, "https://www.hvakosterstrommen.no/api/v1/prices/NO1");
    /* the local date: 23:30 UTC on the 26th is the 27th in Norway */
    CHECK(esl_source_fetch_url(&cfg[0], T0 + 7 * 3600 + 1800, cet, url, sizeof(url)) > 0);
    CHECK_STR(url, "https://www.hvakosterstrommen.no/api/v1/prices/2026/10-27_NO1.json");
    CHECK(esl_source_fetch_url(&cfg[1], T0, cet, url, sizeof(url)) > 0);
    CHECK_STR(url, "https://www.hvakosterstrommen.no/api/v1/prices/2026/10-26_NO4.json");
    {
        static char js[ESL_SOURCE_CFG_MAX];
        esl_sb_t sb;
        esl_sb_init(&sb, js, sizeof(js));
        esl_source_write_cfg(&cfg[1], &sb);
        CHECK(HAS(js, "\"area\":\"NO4\",\"vat\":false"));
    }
    static const struct {
        const char *src, *msg;
    } bad[] = {
        {"\"area\":\"NO6\"", "sources[0].area: the price area"},
        {"\"area\":\"no1\"", "sources[0].area: "},
        {"\"area\":1", "sources[0].area: "},
        {"\"area\":\"NO1\",\"vat\":\"yes\"", "sources[0].vat: true or false"},
        {"\"area\":\"NO1\",\"every_min\":10", "sources[0].every_min: a whole number from 15"},
        {"\"area\":\"NO1\",\"fields\":[\"{temp}\"]", "sources[0].fields[0]: {temp} is not a value of type power"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char js[256];
        snprintf(js, sizeof(js), "{\"sources\":[{\"id\":\"p\",\"type\":\"power\",\"layout\":\"text\",%s}]}", bad[i].src);
        CHECK(!esl_sources_parse(js, strlen(js), cfg, &n, err, sizeof(err)));
        if (strncmp(err, bad[i].msg, strlen(bad[i].msg)) != 0)
            printf("  power cfg %zu: %s\n", i, err);
        CHECK(strncmp(err, bad[i].msg, strlen(bad[i].msg)) == 0);
    }
    CHECK(esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));

    /* hourly and quarterly answers give the same values; 13:20 on 2026-10-27 in Norway */
    uint32_t at = 1793055600u + 13 * 3600 + 20 * 60;
    static const unsigned steps[] = {60, 15};
    for (size_t k = 0; k < 2; k++) {
        size_t len;
        char *body = power_json(steps[k], &len);
        esl_values_t v;
        uint32_t data_at;
        CHECK(esl_source_values(&cfg[0], body, len, at, cet, &v, &data_at, err, sizeof(err)));
        CHECK_STR(esl_values_get(&v, "area"), "NO1");
        CHECK_STR(esl_values_get(&v, "now"), "2,88");    /* 2.30 + 25 % VAT */
        CHECK_STR(esl_values_get(&v, "nowore"), "288");
        CHECK_STR(esl_values_get(&v, "next"), steps[k] == 60 ? "3,00" : "2,88");
        CHECK_STR(esl_values_get(&v, "min"), "0,25");
        CHECK_STR(esl_values_get(&v, "minat"), "03:00");
        CHECK_STR(esl_values_get(&v, "max"), "4,13");    /* 3.30 at 23:00 */
        CHECK_STR(esl_values_get(&v, "maxat"), steps[k] == 60 ? "23:00" : "23:00");
        CHECK_STR(esl_values_get(&v, "level"), "normal");
        CHECK_STR(esl_values_get(&v, "date"), "27.10.");
        /* in English and without VAT; at 03:10 the price is low */
        CHECK(esl_source_values(&cfg[1], body, len, at - 10 * 3600 - 600, cet, &v, &data_at, err, sizeof(err)));
        CHECK_STR(esl_values_get(&v, "now"), "0.20");
        CHECK_STR(esl_values_get(&v, "level"), "low");
        /* another day than the answer's */
        CHECK(!esl_source_values(&cfg[0], body, len, at + 86400, cet, &v, &data_at, err, sizeof(err)));
        CHECK(HAS(err, "no price for now"));
        /* the content of the built-in text layout */
        esl_content_t c;
        CHECK(esl_source_values(&cfg[0], body, len, at, cet, &v, &data_at, err, sizeof(err)));
        CHECK(esl_source_content(&cfg[0], &v, &c, err, sizeof(err)));
        free(body);
    }
    esl_values_t v;
    uint32_t data_at;
    CHECK(!esl_source_values(&cfg[0], "{\"a\":1}", 7, at, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(err, "the answer is not a JSON list of prices");
    CHECK(!esl_source_values(&cfg[0], "[]", 2, at, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(err, "the answer has no prices");

    /* Norway's time whatever the AP's time zone (none: UTC): at 00:30 in Oslo the file is today's,
       and the times shown are Oslo's */
    CHECK(esl_source_fetch_url(&cfg[0], 1793057400u, NULL, url, sizeof(url)) > 0);
    CHECK_STR(url, "https://www.hvakosterstrommen.no/api/v1/prices/2026/10-27_NO1.json");
    {
        size_t len;
        char *body = power_json(60, &len);
        CHECK(esl_source_values(&cfg[0], body, len, 1793057400u, NULL, &v, &data_at, err, sizeof(err)));
        CHECK_STR(esl_values_get(&v, "minat"), "03:00");
        CHECK_STR(esl_values_get(&v, "date"), "27.10.");
        free(body);
    }
    /* the days summer time ends (25 hours) and starts (23 hours), by the quarter and the hour */
    static const struct {
        uint32_t start, end, at;
        unsigned step;
        const char *now, *maxat, *url;
    } dst[] = {
        {1792879200u, 1792969200u, 1792888200u, 15, "1.10", "23:45", "2026/10-25_NO4"}, /* 02:30 CEST */
        {1792879200u, 1792969200u, 1792891800u, 15, "1.14", "23:45", "2026/10-25_NO4"}, /* 02:30 CET */
        {1792879200u, 1792969200u, 1792888200u, 60, "1.02", "23:00", "2026/10-25_NO4"},
        {1792879200u, 1792969200u, 1792891800u, 60, "1.03", "23:00", "2026/10-25_NO4"},
        {1774738800u, 1774821600u, 1774746000u, 15, "1.08", "23:45", "2026/03-29_NO4"}, /* 03:00 CEST */
        {1774738800u, 1774821600u, 1774746000u, 60, "1.02", "23:00", "2026/03-29_NO4"},
    };
    for (size_t i = 0; i < sizeof(dst) / sizeof(dst[0]); i++) {
        size_t len;
        char *body = power_day(dst[i].start, dst[i].end, dst[i].step, &len);
        CHECK(esl_source_values(&cfg[1], body, len, dst[i].at, NULL, &v, &data_at, err, sizeof(err)));
        CHECK_STR(esl_values_get(&v, "now"), dst[i].now);
        CHECK_STR(esl_values_get(&v, "minat"), "00:00");
        CHECK_STR(esl_values_get(&v, "maxat"), dst[i].maxat);
        CHECK_STR(esl_values_get(&v, "date"), dst[i].start == 1792879200u ? "25.10." : "29.03.");
        CHECK(esl_source_fetch_url(&cfg[1], dst[i].at, NULL, url, sizeof(url)) > 0);
        CHECK(HAS(url, dst[i].url));
        free(body);
    }

    /* the next run is at the next quarter, a few s in; a held file (nothing fetched) counts no
       fetch */
    {
        static esl_sources_t ss;
        static const char one[] = "{\"sources\":[{\"id\":\"p\",\"type\":\"power\",\"area\":\"NO1\",\"layout\":\"text\"}]}";
        esl_sources_init(&ss, NULL);
        CHECK(esl_sources_restore(&ss, one, sizeof(one) - 1));
        esl_values_set(&v, "now", "1");
        esl_sources_done(&ss, 0, "p", 1793055600u + 7 * 60, 200, 0, NULL, &v, 0, NULL, NULL);
        CHECK_INT((long)ss.state[0].next_at, (long)(1793055600u + 900 + 2));
        CHECK_INT((long)ss.state[0].fetched_at, (long)(1793055600u + 7 * 60));
        esl_sources_done(&ss, 0, "p", 1793055600u + 902, ESL_SOURCE_HELD, 0, NULL, &v, 0, NULL, NULL);
        CHECK_INT((long)ss.state[0].next_at, (long)(1793055600u + 1800 + 2));
        CHECK_INT((long)ss.state[0].fetched_at, (long)(1793055600u + 7 * 60));
        CHECK_INT(ss.state[0].http, 200);
        CHECK(esl_source_dated(&ss.cfg[0]) && esl_source_own_pace(&ss.cfg[0]));
    }
}

/* ---- type "countdown": the days to or since a date, with no network ---- */

static void test_countdown(void)
{
    static esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    char err[ESL_SOURCE_ERR_MAX + 16], url[ESL_SOURCE_URL_MAX];
    uint8_t n;
    esl_values_t v;
    uint32_t data_at;
    static const char ok[] =
        "{\"sources\":[{\"id\":\"jul\",\"type\":\"countdown\",\"place\":\"Jul\",\"date\":\"2026-12-24\",\"layout\":\"text\","
        "\"tags\":[\"" MAC1 "\"]},"
        "{\"id\":\"fly\",\"type\":\"countdown\",\"date\":\"2026-10-27\",\"time\":\"18:30\",\"lang\":\"en\",\"layout\":\"text\"},"
        "{\"id\":\"bday\",\"type\":\"countdown\",\"place\":\"Ola\",\"date\":\"1990-02-29\",\"yearly\":true,\"layout\":\"text\"}]}";
    /* 29 February 1990 is not a date */
    CHECK(!esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));
    CHECK(HAS(err, "sources[2].date: a date"));
    char js[sizeof(ok)];
    memcpy(js, ok, sizeof(ok));
    memcpy(strstr(js, "1990-02-29"), "1992-02-29", 10);
    CHECK(esl_sources_parse(js, strlen(js), cfg, &n, err, sizeof(err)));
    CHECK(n == 3 && cfg[0].type == ESL_SOURCE_COUNTDOWN && esl_source_is_local(&cfg[0]) && esl_source_is_local(&cfg[1]));
    CHECK(cfg[0].countdown.minute == -1 && cfg[1].countdown.minute == 18 * 60 + 30 && cfg[2].countdown.yearly);
    CHECK(cfg[0].every_min == 1 && cfg[0].nfields == 3);
    CHECK(esl_source_url(&cfg[1], url, sizeof(url)) > 0);
    CHECK_STR(url, "countdown:2026-10-27/1110/0");
    {
        static char out[ESL_SOURCE_CFG_MAX];
        esl_sb_t sb;
        esl_sb_init(&sb, out, sizeof(out));
        esl_source_write_cfg(&cfg[1], &sb);
        CHECK(HAS(out, "\"date\":\"2026-10-27\",\"time\":\"18:30\",\"yearly\":false"));
    }
    static const struct {
        const char *src, *msg;
    } bad[] = {
        {"\"date\":\"2026-13-01\"", "sources[0].date: a date"},
        {"\"date\":\"2026-1-1\"", "sources[0].date: a date"},
        {"\"date\":20261224", "sources[0].date: a date"},
        {"\"date\":\"2026-12-24\",\"time\":\"24:00\"", "sources[0].time: a local time"},
        {"\"date\":\"2026-12-24\",\"time\":\"9:00\"", "sources[0].time: a local time"},
        {"\"date\":\"2026-12-24\",\"yearly\":1", "sources[0].yearly: true or false"},
        {"\"date\":\"2026-12-24\",\"fields\":[\"{temp}\"]", "sources[0].fields[0]: {temp} is not a value of type countdown"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char b[256];
        snprintf(b, sizeof(b), "{\"sources\":[{\"id\":\"c\",\"type\":\"countdown\",\"layout\":\"text\",%s}]}", bad[i].src);
        CHECK(!esl_sources_parse(b, strlen(b), cfg, &n, err, sizeof(err)));
        if (strncmp(err, bad[i].msg, strlen(bad[i].msg)) != 0)
            printf("  countdown cfg %zu: %s\n", i, err);
        CHECK(strncmp(err, bad[i].msg, strlen(bad[i].msg)) == 0);
    }
    CHECK(esl_sources_parse(js, strlen(js), cfg, &n, err, sizeof(err)));

    /* 2026-10-27 13:20 in Norway (CET) */
    uint32_t at = 1793055600u + 13 * 3600 + 20 * 60;
    CHECK(esl_source_values(&cfg[0], NULL, 0, at, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "place"), "Jul");
    CHECK_STR(esl_values_get(&v, "left"), "om 58 dager");
    CHECK_STR(esl_values_get(&v, "days"), "58");
    CHECK_STR(esl_values_get(&v, "weeks"), "8");
    CHECK_STR(esl_values_get(&v, "since"), "0");
    CHECK_STR(esl_values_get(&v, "date"), "24.12.2026");
    CHECK_STR(esl_values_get(&v, "weekday"), "torsdag");
    CHECK(esl_values_get(&v, "years") == NULL);
    /* 23:59 local on the 23rd is tomorrow; 00:00 on the 24th today; then days since */
    uint32_t xmas = 1793055600u + 58 * 86400; /* midnight 24 December, CET */
    CHECK(esl_source_values(&cfg[0], NULL, 0, xmas - 60, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "left"), "i morgen");
    CHECK(esl_source_values(&cfg[0], NULL, 0, xmas, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "left"), "i dag");
    CHECK_STR(esl_values_get(&v, "days"), "0");
    CHECK(esl_source_values(&cfg[0], NULL, 0, xmas + 86400 + 3600, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "left"), "i går");
    CHECK(esl_source_values(&cfg[0], NULL, 0, xmas + 10 * 86400, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "left"), "for 10 dager siden");
    CHECK_STR(esl_values_get(&v, "since"), "10");

    /* with a time, the last day counts in hours and minutes */
    CHECK(esl_source_values(&cfg[1], NULL, 0, at, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "left"), "in 5 h"); /* whole hours until the last one */
    CHECK_STR(esl_values_get(&v, "hours"), "5");
    CHECK_STR(esl_values_get(&v, "weekday"), "Tuesday");
    CHECK(esl_source_values(&cfg[1], NULL, 0, at + 5 * 3600 + 9 * 60 + 30, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "left"), "in 1 min");
    CHECK(esl_source_values(&cfg[1], NULL, 0, at + 4 * 3600 + 20 * 60, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "left"), "in 50 min");
    CHECK(esl_source_values(&cfg[1], NULL, 0, at + 4 * 3600 + 9 * 60, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "left"), "in 1 h");
    /* a summer time switch between now and the time: the real hours left */
    {
        static const char sw[] = "{\"sources\":[{\"id\":\"a\",\"type\":\"countdown\",\"date\":\"2026-10-25\","
                                 "\"time\":\"12:00\",\"lang\":\"en\",\"layout\":\"text\"},"
                                 "{\"id\":\"b\",\"type\":\"countdown\",\"date\":\"2026-03-29\",\"time\":\"12:00\","
                                 "\"lang\":\"en\",\"layout\":\"text\"},"
                                 "{\"id\":\"c\",\"type\":\"countdown\",\"date\":\"2030-05-01\",\"yearly\":true,"
                                 "\"layout\":\"text\"}]}";
        static esl_source_cfg_t sc[ESL_SOURCE_MAX];
        CHECK(esl_sources_parse(sw, sizeof(sw) - 1, sc, &n, err, sizeof(err)));
        /* 01:00 CEST on 25 October: 12 hours to 12:00 CET */
        CHECK(esl_source_values(&sc[0], NULL, 0, 1792882800u, cet, &v, &data_at, err, sizeof(err)));
        CHECK_STR(esl_values_get(&v, "left"), "in 12 h");
        CHECK_STR(esl_values_get(&v, "hours"), "12");
        /* three days before: 72 + 12 */
        CHECK(esl_source_values(&sc[0], NULL, 0, 1792882800u - 3 * 86400, cet, &v, &data_at, err, sizeof(err)));
        CHECK_STR(esl_values_get(&v, "hours"), "84");
        /* 01:00 CET on 29 March: 10 hours to 12:00 CEST */
        CHECK(esl_source_values(&sc[1], NULL, 0, 1774742400u, cet, &v, &data_at, err, sizeof(err)));
        CHECK_STR(esl_values_get(&v, "left"), "in 10 h");
        /* yearly from a date still ahead: that date, 0 years */
        CHECK(esl_source_values(&sc[2], NULL, 0, at, cet, &v, &data_at, err, sizeof(err)));
        CHECK_STR(esl_values_get(&v, "date"), "01.05.2030");
        CHECK_STR(esl_values_get(&v, "years"), "0");
    }
    CHECK(esl_source_values(&cfg[1], NULL, 0, at + 6 * 3600, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "left"), "today");
    CHECK_STR(esl_values_get(&v, "hours"), "0");

    /* yearly: the next 29 February is 28 February in 2027, when Ola turns 35 */
    CHECK(esl_source_values(&cfg[2], NULL, 0, at, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "date"), "28.02.2027");
    CHECK_STR(esl_values_get(&v, "years"), "35");
    CHECK_STR(esl_values_get(&v, "days"), "124");
    /* a yearly date alone holds for its whole day, then the next year's */
    cfg[0].countdown.yearly = true;
    CHECK(esl_source_values(&cfg[0], NULL, 0, xmas + 23 * 3600, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "left"), "i dag");
    CHECK(esl_source_values(&cfg[0], NULL, 0, xmas + 86400, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "date"), "24.12.2027");
    CHECK_STR(esl_values_get(&v, "days"), "364");
    CHECK_STR(esl_values_get(&v, "years"), "1");
    /* the built-in text layout takes the default fields */
    esl_content_t c;
    CHECK(esl_source_content(&cfg[0], &v, &c, err, sizeof(err)));
}

/* ---- type "rss": the newest headlines of a feed ---- */

static void test_rss(void)
{
    static esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    char err[ESL_SOURCE_ERR_MAX + 16], url[ESL_SOURCE_URL_MAX];
    uint8_t n;
    esl_values_t v;
    uint32_t data_at;
    static const char ok[] = "{\"sources\":[{\"id\":\"news\",\"type\":\"rss\",\"url\":\"https://news.example/top.rss\","
                             "\"layout\":\"text\",\"tags\":[\"" MAC1 "\"]}]}";
    CHECK(esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));
    CHECK(n == 1 && cfg[0].type == ESL_SOURCE_RSS && cfg[0].every_min == 15 && cfg[0].nfields == 4);
    CHECK(esl_source_url(&cfg[0], url, sizeof(url)) > 0);
    CHECK_STR(url, "https://news.example/top.rss");
    static const char *const bad[] = {"\"url\":\"ftp://x/a\"", "\"url\":\"http://u:p@x/a\"", "\"url\":\"http://x/a b\"",
                                      "\"url\":1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char js[256];
        snprintf(js, sizeof(js), "{\"sources\":[{\"id\":\"r\",\"type\":\"rss\",\"layout\":\"text\",%s}]}", bad[i]);
        CHECK(!esl_sources_parse(js, strlen(js), cfg, &n, err, sizeof(err)));
        CHECK(HAS(err, "sources[0].url: an http:// or https:// URL of an RSS or Atom feed"));
    }
    CHECK(esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));

    /* RSS 2.0: the channel's title, then each item's; CDATA, entities, tags inside */
    static const char rss[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rss version=\"2.0\"><channel>\n"
        "  <title>Nyheter &amp; sport</title><link>https://news.example/</link>\n"
        "  <image><title>logo</title></image>\n"
        "  <item><title><![CDATA[Første sak: <b>regn</b> i Bergen]]></title><link>x</link></item>\n"
        "  <item>\n    <title>Pris &#248;ker &#x2013; igjen</title>\n  </item>\n"
        "  <item><title/></item>\n"
        "  <item><title>En veldig lang overskrift som ikke får plass på en linje i det hele tatt</title></item>\n"
        "</channel></rss>";
    CHECK(esl_source_values(&cfg[0], rss, sizeof(rss) - 1, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "title"), "Nyheter & sport");
    CHECK_STR(esl_values_get(&v, "h1"), "Første sak: regn i Bergen");
    CHECK_STR(esl_values_get(&v, "h2"), "Pris øker – igjen");
    CHECK_STR(esl_values_get(&v, "h3"), "En veldig lang overskrift som ikke f...");
    CHECK(strlen(esl_values_get(&v, "h3")) <= ESL_VALUE_MAX);
    CHECK_STR(esl_values_get(&v, "h4"), "");
    CHECK_STR(esl_values_get(&v, "count"), "3");
    esl_content_t c;
    CHECK(esl_source_content(&cfg[0], &v, &c, err, sizeof(err)));

    /* Atom, with more entries than are given */
    static const char atom[] =
        "<feed xmlns=\"http://www.w3.org/2005/Atom\"><title type=\"text\">Blogg</title>"
        "<entry><title>1</title></entry><entry><title>2</title></entry><entry><title>3</title></entry>"
        "<entry><title>4</title></entry><entry><title>5</title></entry><entry><title>6</title></entry>"
        "<entry><title>7</title></entry></feed>";
    CHECK(esl_source_values(&cfg[0], atom, sizeof(atom) - 1, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "title"), "Blogg");
    CHECK_STR(esl_values_get(&v, "h6"), "6");
    CHECK_STR(esl_values_get(&v, "count"), "6");
    /* a feed with no items yet, and not a feed */
    static const char empty[] = "<rss><channel><title>Tom</title></channel></rss>";
    CHECK(esl_source_values(&cfg[0], empty, sizeof(empty) - 1, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "count"), "0");
    CHECK_STR(esl_values_get(&v, "h1"), "");
    CHECK(!esl_source_values(&cfg[0], "<html><body>404</body></html>", 29, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(err, "the answer is not an RSS or Atom feed");
    /* broken input reads no further than the body */
    static const char cut[] = "<rss><channel><item><title><![CDATA[abc";
    CHECK(esl_source_values(&cfg[0], cut, sizeof(cut) - 1, T0, cet, &v, &data_at, err, sizeof(err)));
    static const char cut2[] = "<rss><item><title>a &#x";
    CHECK(esl_source_values(&cfg[0], cut2, sizeof(cut2) - 1, T0, cet, &v, &data_at, err, sizeof(err)));
    static const char cut3[] = "<rss><item><title>a <b";
    CHECK(esl_source_values(&cfg[0], cut3, sizeof(cut3) - 1, T0, cet, &v, &data_at, err, sizeof(err)));

    /* hostile bodies are read in linear time: 128 KB of "<item>" or "<item><title>" without
       closing tags (each item is read only up to the next) */
    {
        static const char *const unit[] = {"<item>", "<item><title>", "<entry><title>x"};
        static char big[ESL_SOURCE_BODY_MAX];
        for (size_t u = 0; u < 3; u++) {
            size_t ul = strlen(unit[u]), bl = 0;
            memcpy(big, "<rss>", 5);
            bl = 5;
            while (bl + ul <= sizeof(big)) {
                memcpy(big + bl, unit[u], ul);
                bl += ul;
            }
            clock_t t0 = clock();
            CHECK(esl_source_values(&cfg[0], big, bl, T0, cet, &v, &data_at, err, sizeof(err)));
            double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
            if (ms > 200)
                printf("  rss hostile %zu: %.0f ms\n", u, ms);
            CHECK(ms < 200);
            CHECK_STR(esl_values_get(&v, "count"), "0");
        }
    }
    /* what feeds really hold: HTML in CDATA and type="html" read twice, typographic entities,
       control characters, surrogates and the BOM left out, comments skipped, Latin-1 */
    static const char real[] =
        "<?xml version=\"1.0\"?><rss><channel><title>T</title>"
        "<!-- <item><title>old</title></item> -->"
        "<item><title><![CDATA[Rock &amp; roll &rsquo;n&hellip;]]></title></item>"
        "<item><title>a&#1;b&#x7f;c&#xD800;d&#xFEFF;e&#0;f</title></item>"
        "<item><title>raw \xff\xfe bytes</title></item>"
        "</channel></rss>";
    CHECK(esl_source_values(&cfg[0], real, sizeof(real) - 1, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "h1"), "Rock & roll \xe2\x80\x99n\xe2\x80\xa6");
    CHECK_STR(esl_values_get(&v, "h2"), "abcdef");
    CHECK_STR(esl_values_get(&v, "h3"), "raw ?? bytes");
    CHECK_STR(esl_values_get(&v, "count"), "3");
    static const char html[] = "<feed><title type=\"html\">Tom &amp;amp; Jerry &lt;b&gt;x&lt;/b&gt;</title>"
                               "<entry><title type='html'>&lt;i&gt;Ny&lt;/i&gt; sak</title></entry></feed>";
    CHECK(esl_source_values(&cfg[0], html, sizeof(html) - 1, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "title"), "Tom & Jerry x");
    CHECK_STR(esl_values_get(&v, "h1"), "Ny sak");
    static const char l1[] = "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><rss><item><title>Bl\xe5" "b\xe6" "r</title></item></rss>";
    CHECK(esl_source_values(&cfg[0], l1, sizeof(l1) - 1, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "h1"), "Bl\xc3\xa5" "b\xc3\xa6r");
    /* windows-1252's curly quotes and dashes (0x91-0x97), its euro sign, and 0x81 (no
       character: dropped) */
    static const char w[] = "<?xml version=\"1.0\" encoding=\"windows-1252\"?><rss><item>"
                            "<title>\x93Hei\x94 \x96 \x80" "5\x81</title></item>"
                            "<item><title><![CDATA[a < b, 2<3 &lt;ok]]></title></item>"
                            "<item><title><![CDATA[x <y and more]]></title></item></rss>";
    CHECK(esl_source_values(&cfg[0], w, sizeof(w) - 1, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "h1"), "\xe2\x80\x9cHei\xe2\x80\x9d \xe2\x80\x93 \xe2\x82\xac" "5");
    /* a '<' that starts no tag is text */
    CHECK_STR(esl_values_get(&v, "h2"), "a < b, 2<3 <ok");
    /* nor does one with no '>' after it: the rest of the title stays */
    CHECK_STR(esl_values_get(&v, "h3"), "x <y and more");
    CHECK(esl_source_prefix(&cfg[0]) && !esl_source_prefix(&cfg[1]));
}

/* cet(), counting the calls: the time zone lookups an ical reader makes */
static long offs;
static int32_t cet_counted(uint32_t t)
{
    offs++;
    return cet(t);
}

/* Feeds an answer to source c's reader k bytes at a time; its short body and the values */
static bool ical_read(const esl_source_cfg_t *c, const char *ics, size_t k, uint32_t now, esl_values_t *v, char *err,
                      size_t errlen)
{
    esl_source_stream_t *st = esl_source_stream_new(c, now, cet);
    if (!st)
        return false;
    size_t n = strlen(ics);
    for (size_t i = 0; i < n; i += k)
        esl_source_stream_feed(st, ics + i, n - i < k ? n - i : k);
    char *body = NULL;
    size_t len = 0;
    uint32_t data_at;
    bool ok = esl_source_stream_end(st, &body, &len, err, errlen) &&
              esl_source_values(c, body, len, now, cet, v, &data_at, err, errlen);
    free(body);
    return ok;
}

static void test_ical(void)
{
    static esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    char err[ESL_SOURCE_ERR_MAX + 16], url[ESL_SOURCE_URL_MAX];
    uint8_t n;
    esl_values_t v;
    static const char ok[] = "{\"sources\":[{\"id\":\"cal\",\"type\":\"ical\",\"url\":\"webcal://cal.example/a.ics\","
                             "\"layout\":\"text\",\"tags\":[\"" MAC1 "\"]},"
                             "{\"id\":\"en\",\"type\":\"ical\",\"url\":\"https://cal.example/b.ics\",\"lang\":\"en\","
                             "\"layout\":\"text\",\"tags\":[\"" MAC2 "\"]}]}";
    CHECK(esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));
    CHECK(n == 2);
    /* webcal:// is the same address over https */
    CHECK(esl_source_url(&cfg[0], url, sizeof(url)) && !strcmp(url, "https://cal.example/a.ics"));
    CHECK(cfg[0].every_min == 15);
    static const char *const bad[] = {
        "{\"sources\":[{\"id\":\"a\",\"type\":\"ical\",\"layout\":\"text\"}]}",
        "{\"sources\":[{\"id\":\"a\",\"type\":\"ical\",\"url\":\"ftp://x/a.ics\",\"layout\":\"text\"}]}",
        "{\"sources\":[{\"id\":\"a\",\"type\":\"ical\",\"url\":\"webcal://u:p@x/a.ics\",\"layout\":\"text\"}]}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!esl_sources_parse(bad[i], strlen(bad[i]), cfg + 2, &n, err, sizeof(err)));
        CHECK(HAS(err, "url: an http://, https:// or webcal:// URL"));
    }
    /* T0 is Monday 26 October 2026, 17:00 in Norway (CET, the day after summer time ended) */
    static const char ics[] =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nX-WR-CALNAME:Familie\\, uke\r\n"
        "BEGIN:VTIMEZONE\r\nTZID:Europe/Oslo\r\nBEGIN:STANDARD\r\nDTSTART:19701025T030000\r\nEND:STANDARD\r\n"
        "END:VTIMEZONE\r\n"
        /* under way: 16:00 to 18:00 CET; the alarm's SUMMARY is not the event's */
        "BEGIN:VEVENT\r\nUID:a\r\nDTSTART:20261026T150000Z\r\nDTEND:20261026T170000Z\r\nSUMMARY:M\xc3\xb8te\r\n"
        "BEGIN:VALARM\r\nSUMMARY:alarm\r\nTRIGGER:-PT15M\r\nEND:VALARM\r\nEND:VEVENT\r\n"
        /* Tuesdays and Thursdays at 19:00 local from summer time, one left out, one moved */
        "BEGIN:VEVENT\r\nUID:b\r\nDTSTART;TZID=Europe/Oslo:20261020T190000\r\nDURATION:PT1H\r\n"
        "RRULE:FREQ=WEEKLY;BYDAY=TU,TH\r\nEXDATE;TZID=Europe/Oslo:20261027T190000\r\n"
        "SUMMARY:Trening med et veldig langt navn som m\xc3\xa5 kuttes ett eller annet sted \r\n her\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:b\r\nRECURRENCE-ID;TZID=Europe/Oslo:20261029T190000\r\n"
        "DTSTART;TZID=Europe/Oslo:20261029T200000\r\nDTEND;TZID=Europe/Oslo:20261029T210000\r\n"
        "SUMMARY:Trening (flyttet)\r\nEND:VEVENT\r\n"
        /* two whole days from tomorrow */
        "BEGIN:VEVENT\r\nUID:c\r\nDTSTART;VALUE=DATE:20261027\r\nDTEND;VALUE=DATE:20261029\r\nSUMMARY:Ferie\r\n"
        "END:VEVENT\r\n"
        /* a birthday every year since 1990 */
        "BEGIN:VEVENT\r\nUID:d\r\nDTSTART;VALUE=DATE:19901102\r\nRRULE:FREQ=YEARLY\r\nSUMMARY:Bursdag\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:e\r\nDTSTART:20261026T180000Z\r\nSTATUS:CANCELLED\r\nSUMMARY:Avlyst\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:f\r\nDTSTART:20261025T100000Z\r\nDTEND:20261025T110000Z\r\nSUMMARY:Gammel\r\n"
        "END:VEVENT\r\n"
        /* the last Friday of each month at 09:00 CET */
        "BEGIN:VEVENT\r\nUID:g\r\nDTSTART:20260130T080000Z\r\nRRULE:FREQ=MONTHLY;BYDAY=-1FR\r\nSUMMARY:L\xc3\xb8nn\r\n"
        "END:VEVENT\r\n"
        /* five days, all past; a rule this does not follow gives the first one only (past) */
        "BEGIN:VEVENT\r\nUID:h\r\nDTSTART:20261001T070000Z\r\nRRULE:FREQ=DAILY;COUNT=5\r\nSUMMARY:Kurs\r\n"
        "END:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:i\r\nDTSTART:20261001T070000Z\r\nRRULE:FREQ=MONTHLY;BYSETPOS=1;BYDAY=MO\r\n"
        "SUMMARY:Rart\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    static const size_t steps[] = {1, 7, 4096};
    for (size_t k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
        CHECK(ical_read(&cfg[0], ics, steps[k], T0, &v, err, sizeof(err)));
        CHECK_STR(esl_values_get(&v, "title"), "Familie, uke");
        CHECK_STR(esl_values_get(&v, "today"), "1");
        CHECK_STR(esl_values_get(&v, "day1"), "i dag");
        CHECK_STR(esl_values_get(&v, "time1"), "til 18:00");
        CHECK_STR(esl_values_get(&v, "what1"), "M\xc3\xb8te");
        CHECK_STR(esl_values_get(&v, "day2"), "i morgen");
        CHECK_STR(esl_values_get(&v, "time2"), "hele dagen");
        CHECK_STR(esl_values_get(&v, "what2"), "Ferie");
        CHECK_STR(esl_values_get(&v, "day3"), "tor. 29. okt");
        CHECK_STR(esl_values_get(&v, "time3"), "20:00");
        CHECK_STR(esl_values_get(&v, "what3"), "Trening (flyttet)");
        CHECK_STR(esl_values_get(&v, "day4"), "fre. 30. okt");
        CHECK_STR(esl_values_get(&v, "time4"), "09:00");
        CHECK_STR(esl_values_get(&v, "what4"), "L\xc3\xb8nn");
        CHECK_STR(esl_values_get(&v, "day5"), "man. 2. nov");
        CHECK_STR(esl_values_get(&v, "what5"), "Bursdag");
        CHECK_STR(esl_values_get(&v, "day6"), "tir. 3. nov");
        CHECK_STR(esl_values_get(&v, "time6"), "19:00");
        CHECK_STR(esl_values_get(&v, "what6"), "Trening med et veldig langt navn som...");
    }
    /* more rules: every day since 2010 at 07:00 UTC (skipped ahead), every other week until a
       date, the last day of the month, three times from a date */
    static const char rules[] =
        "BEGIN:VCALENDAR\r\n"
        "BEGIN:VEVENT\r\nUID:1\r\nDTSTART:20100101T070000Z\r\nRRULE:FREQ=DAILY\r\nSUMMARY:Daglig\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:2\r\nDTSTART:20261012T120000Z\r\nRRULE:FREQ=WEEKLY;INTERVAL=2;UNTIL=20261110T000000Z\r\n"
        "SUMMARY:Annenhver\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:3\r\nDTSTART;VALUE=DATE:20260131\r\nRRULE:FREQ=MONTHLY;BYMONTHDAY=-1\r\n"
        "SUMMARY:Siste\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:4\r\nDTSTART:20261020T100000Z\r\nRRULE:FREQ=WEEKLY;COUNT=3\r\nSUMMARY:Tre\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    static const struct {
        const char *day, *what;
    } want[] = {{"i morgen", "Daglig"},      {"i morgen", "Tre"},       {"ons. 28. okt", "Daglig"},
                {"tor. 29. okt", "Daglig"}, {"fre. 30. okt", "Daglig"}, {"lør. 31. okt", "Siste"}};
    CHECK(ical_read(&cfg[0], rules, 4096, T0, &v, err, sizeof(err)));
    for (unsigned i = 0; i < 6; i++) {
        char k[8];
        snprintf(k, sizeof(k), "day%u", i + 1);
        CHECK_STR(esl_values_get(&v, k), want[i].day);
        snprintf(k, sizeof(k), "what%u", i + 1);
        CHECK_STR(esl_values_get(&v, k), want[i].what);
    }
    /* "Annenhver" was today at 12:00 UTC. Seen from Monday 2 November 07:00, "Tre" (20 and 27 October, 3 November) comes tomorrow for the last time */
    CHECK(ical_read(&cfg[0], rules, 4096, T0 + 7 * 86400 - 3600 * 10, &v, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "what1"), "Daglig");
    CHECK_STR(esl_values_get(&v, "day1"), "i dag");
    CHECK_STR(esl_values_get(&v, "day2"), "i morgen");
    CHECK_STR(esl_values_get(&v, "what2"), "Daglig");
    CHECK_STR(esl_values_get(&v, "what3"), "Tre");
    CHECK_STR(esl_values_get(&v, "time3"), "11:00");
    /* the last day of the month, all day; "Annenhver" ended with its UNTIL, "Tre" after three */
    CHECK(ical_read(&cfg[0], rules, 4096, 1795734000u /* 2026-11-26 21:00 UTC */, &v, err, sizeof(err)));
    bool last = false;
    for (unsigned i = 1; i <= 6; i++) {
        char k[8];
        snprintf(k, sizeof(k), "what%u", i);
        if (!strcmp(esl_values_get(&v, k), "Siste")) {
            snprintf(k, sizeof(k), "day%u", i);
            last = !strcmp(esl_values_get(&v, k), "man. 30. nov");
        }
        CHECK(strcmp(esl_values_get(&v, k), "Annenhver") && strcmp(esl_values_get(&v, k), "Tre"));
    }
    CHECK(last);

    /* UNTIL: 9 November is the last; COUNT with a skip: "Tre" ends on 3 November */
    static const char until[] =
        "BEGIN:VCALENDAR\r\n"
        "BEGIN:VEVENT\r\nUID:2\r\nDTSTART:20261012T120000Z\r\nRRULE:FREQ=WEEKLY;INTERVAL=2;UNTIL=20261110T000000Z\r\n"
        "SUMMARY:Annenhver\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:4\r\nDTSTART:20261020T100000Z\r\nRRULE:FREQ=WEEKLY;COUNT=3\r\nSUMMARY:Tre\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    CHECK(ical_read(&cfg[0], until, 4096, T0, &v, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "what1"), "Tre");
    CHECK_STR(esl_values_get(&v, "what2"), "Tre");
    CHECK_STR(esl_values_get(&v, "day2"), "tir. 3. nov");
    CHECK_STR(esl_values_get(&v, "what3"), "Annenhver");
    CHECK_STR(esl_values_get(&v, "day3"), "man. 9. nov");
    CHECK_STR(esl_values_get(&v, "what4"), "");

    /* a long series: its 30 EXDATEs and 40 moved times from the past do not push out the ones
       that matter now (next Monday 2 November left out, then 9 November moved to 12:00); a
       cancelled single time is left out */
    {
        static char big[16384];
        size_t k = (size_t)sprintf(big, "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:w\r\n"
                                        "DTSTART;TZID=Europe/Oslo:20200106T090000\r\nRRULE:FREQ=WEEKLY\r\n");
        for (int i = 0; i < 29; i++)
            k += (size_t)sprintf(big + k, "EXDATE;TZID=Europe/Oslo:2020%02d%02dT090000\r\n", 1 + i / 4,
                                 6 + 7 * (i % 4));
        k += (size_t)sprintf(big + k, "EXDATE;TZID=Europe/Oslo:20261102T090000\r\nSUMMARY:Ukentlig\r\nEND:VEVENT\r\n");
        for (int i = 0; i < 40; i++)
            k += (size_t)sprintf(big + k,
                                 "BEGIN:VEVENT\r\nUID:w\r\nRECURRENCE-ID;TZID=Europe/Oslo:2021%02d%02dT090000\r\n"
                                 "DTSTART;TZID=Europe/Oslo:2021%02d%02dT100000\r\nSUMMARY:Gammel flytting\r\nEND:VEVENT\r\n",
                                 1 + i / 4, 4 + 7 * (i % 4), 1 + i / 4, 4 + 7 * (i % 4));
        k += (size_t)sprintf(big + k,
                             "BEGIN:VEVENT\r\nUID:w\r\nRECURRENCE-ID;TZID=Europe/Oslo:20261109T090000\r\n"
                             "DTSTART;TZID=Europe/Oslo:20261109T120000\r\nSUMMARY:Ukentlig (flyttet)\r\nEND:VEVENT\r\n"
                             "BEGIN:VEVENT\r\nUID:t\r\nDTSTART:20261020T090000Z\r\nRRULE:FREQ=WEEKLY\r\n"
                             "SUMMARY:Tirsdag\r\nEND:VEVENT\r\n"
                             "BEGIN:VEVENT\r\nUID:t\r\nRECURRENCE-ID:20261027T090000Z\r\nDTSTART:20261027T090000Z\r\n"
                             "STATUS:CANCELLED\r\nSUMMARY:Tirsdag\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
        CHECK(k < sizeof(big));
        CHECK(ical_read(&cfg[0], big, 4096, T0, &v, err, sizeof(err)));
        CHECK_STR(esl_values_get(&v, "day1"), "tir. 3. nov");
        CHECK_STR(esl_values_get(&v, "what1"), "Tirsdag");
        CHECK_STR(esl_values_get(&v, "day2"), "man. 9. nov");
        CHECK_STR(esl_values_get(&v, "time2"), "12:00");
        CHECK_STR(esl_values_get(&v, "what2"), "Ukentlig (flyttet)");
        CHECK_STR(esl_values_get(&v, "day3"), "tir. 10. nov");
        CHECK_STR(esl_values_get(&v, "day4"), "man. 16. nov");
        CHECK_STR(esl_values_get(&v, "time4"), "09:00");
    }
    /* WKST=SU: every other week from Sunday 1 November, Sundays and Tuesdays of the same week */
    static const char wkst[] = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nDTSTART:20261101T100000Z\r\n"
                               "RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=TU,SU;WKST=SU\r\nSUMMARY:x\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    CHECK(ical_read(&cfg[0], wkst, 4096, T0, &v, err, sizeof(err)));
    static const char *const wk[] = {"søn. 1. nov", "tir. 3. nov", "søn. 15. nov", "tir. 17. nov", "søn. 29. nov",
                                     "tir. 1. des"};
    for (unsigned i = 0; i < 6; i++) {
        char key[8];
        snprintf(key, sizeof(key), "day%u", i + 1);
        CHECK_STR(esl_values_get(&v, key), wk[i]);
    }
    /* a connection that ended early */
    CHECK(!ical_read(&cfg[0], "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nDTSTART:20261101T100000Z\r\nEND:VEVENT\r\n", 4096, T0,
                     &v, err, sizeof(err)));
    CHECK(HAS(err, "cut off"));
    /* held an hour: the runs between make the values again from the short body */
    CHECK(esl_source_hold_s(&cfg[0]) == 3600);

    /* in English, a day later: the meeting is over and the holiday under way */
    CHECK(ical_read(&cfg[1], ics, 4096, T0 + 86400, &v, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "today"), "1");
    CHECK_STR(esl_values_get(&v, "day1"), "today");
    CHECK_STR(esl_values_get(&v, "time1"), "all day");
    CHECK_STR(esl_values_get(&v, "what1"), "Ferie");
    CHECK_STR(esl_values_get(&v, "day2"), "Thu 29 Oct");
    /* the body is made at the fetch; the values follow the clock: two hours later the
       meeting is gone and fewer events leave the last lines empty */
    static const char one[] = "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nDTSTART:20261026T150000Z\r\n"
                              "DTEND:20261026T170000Z\r\nSUMMARY:M\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    esl_source_stream_t *st = esl_source_stream_new(&cfg[0], T0, cet);
    CHECK(st != NULL);
    esl_source_stream_feed(st, one, strlen(one));
    char *body = NULL;
    size_t len = 0;
    uint32_t data_at;
    CHECK(esl_source_stream_end(st, &body, &len, err, sizeof(err)));
    CHECK(esl_source_values(&cfg[0], body, len, T0, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "what1"), "M");
    CHECK(esl_source_values(&cfg[0], body, len, T0 + 7200, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "what1"), "");
    CHECK_STR(esl_values_get(&v, "day6"), "");
    CHECK_STR(esl_values_get(&v, "today"), "0");
    free(body);
    /* not a calendar; a reader that is only freed */
    CHECK(!ical_read(&cfg[0], "<html><body>Sign in</body></html>", 4096, T0, &v, err, sizeof(err)));
    CHECK(HAS(err, "not an iCalendar file"));
    st = esl_source_stream_new(&cfg[0], T0, cet);
    esl_source_stream_feed(st, ics, 100);
    CHECK(!esl_source_stream_end(st, NULL, NULL, err, sizeof(err)));
    /* types that read their answer whole have no reader */
    static esl_source_cfg_t met;
    met.type = ESL_SOURCE_MET;
    CHECK(esl_source_stream_new(&met, T0, cet) == NULL);

    /* hostile: 3 MB of rules that each give thousands of times, in a time zone (each time near
       now looks the zone up, which is slow on the AP), a 200 KB line, deep nesting. The work
       has a bound: the zone lookups are counted, not timed. */
    static const char *const evs[] = {
        "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Oslo:19000101T100000\r\n"
        "RRULE:FREQ=YEARLY;COUNT=100000;BYMONTH=1,2,3,4,5,6,7,8,9,10,11,12;BYDAY=MO,TU,WE,TH,FR,SA,SU\r\n"
        "SUMMARY:y\r\nEND:VEVENT\r\n",
        "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Oslo:19000101T100000\r\n"
        "RRULE:FREQ=WEEKLY;COUNT=100000;BYDAY=MO,TU,WE,TH,FR,SA,SU\r\nSUMMARY:w\r\nEND:VEVENT\r\n",
        "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Oslo:20261027T100000\r\nRRULE:FREQ=DAILY\r\nSUMMARY:d\r\nEND:VEVENT\r\n",
    };
    size_t big = 3u << 20;
    char *h = malloc(big + 1);
    CHECK(h != NULL);
    for (size_t e = 0; h && e < sizeof(evs) / sizeof(evs[0]); e++) {
        size_t k = (size_t)sprintf(h, "BEGIN:VCALENDAR\r\nX-WR-CALNAME:");
        memset(h + k, 'a', 200000);
        k += 200000;
        memcpy(h + k, "\r\n", 2);
        k += 2;
        for (int i = 0; i < 90; i++)
            k += (size_t)sprintf(h + k, "BEGIN:X\r\n");
        for (int i = 0; i < 90; i++)
            k += (size_t)sprintf(h + k, "END:X\r\n");
        size_t el = strlen(evs[e]);
        while (k + el + 32 < big) {
            memcpy(h + k, evs[e], el);
            k += el;
        }
        k += (size_t)sprintf(h + k, "END:VCALENDAR\r\n");
        offs = 0;
        st = esl_source_stream_new(&cfg[0], T0, cet_counted);
        for (size_t i = 0; i < k; i += 4096)
            esl_source_stream_feed(st, h + i, k - i < 4096 ? k - i : 4096);
        body = NULL;
        CHECK(esl_source_stream_end(st, &body, &len, err, sizeof(err)));
        CHECK(offs < 300000);
        CHECK(len < 4096);
        CHECK(esl_source_values(&cfg[0], body, len, T0, cet, &v, &data_at, err, sizeof(err)));
        CHECK(strlen(esl_values_get(&v, "title")) <= ESL_VALUE_MAX);
        CHECK(esl_values_get(&v, "what1")[0]);
        free(body);
    }
    free(h);
}

/* Norges Bank's answer for USD (format=csv, lastNObservations=31), 2026-11-13 */
static const char fx_usd[] =
    "FREQ;Frequency;BASE_CUR;Base Currency;QUOTE_CUR;Quote Currency;TENOR;Tenor;DECIMALS;CALCULATED;UNIT_MULT;Unit Multiplier;COLLECTION;Collection Indicator;TIME_PERIOD;OBS_VALUE\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-01;9.6256\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-02;9.6494\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-05;9.6015\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-06;9.5643\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-07;9.584\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-08;9.5807\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-09;9.5623\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-12;9.5658\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-13;9.5357\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-14;9.4716\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-15;9.5039\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-16;9.441\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-19;9.4036\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-20;9.3843\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-21;9.3573\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-22;9.3601\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-23;9.3509\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-26;9.3444\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-27;9.317\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-28;9.2944\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-29;9.2826\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-10-30;9.2302\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-02;9.2524\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-03;9.2303\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-04;9.2278\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-05;9.2984\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-06;9.4277\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-09;9.4099\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-10;9.4663\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-11;9.5394\r\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-12;9.5325\r\n";
/* USD and EUR in one answer (USD in EUR), the last three days, the currencies one after the other */
static const char fx_usd_eur[] =
    "FREQ;Frequency;BASE_CUR;Base Currency;QUOTE_CUR;Quote Currency;TENOR;Tenor;DECIMALS;CALCULATED;UNIT_MULT;Unit Multiplier;COLLECTION;Collection Indicator;TIME_PERIOD;OBS_VALUE\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-10;9.4663\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-11;9.5394\n"
    "B;Business;USD;US dollar;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-12;9.5325\n"
    "B;Business;EUR;Euro;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-10;10.6155\n"
    "B;Business;EUR;Euro;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-11;10.6145\n"
    "B;Business;EUR;Euro;NOK;Norwegian krone;SP;Spot;4;false;0;Units;C;ECB concertation time 14:15 CET;2026-11-12;10.563\n";
/* JPY: the price of 100 yen (UNIT_MULT 2) */
static const char fx_jpy[] =
    "FREQ;Frequency;BASE_CUR;Base Currency;QUOTE_CUR;Quote Currency;TENOR;Tenor;DECIMALS;CALCULATED;UNIT_MULT;Unit Multiplier;COLLECTION;Collection Indicator;TIME_PERIOD;OBS_VALUE\r\n"
    "B;Business;JPY;Japanese yen;NOK;Norwegian krone;SP;Spot;4;false;2;Hundreds;C;ECB concertation time 14:15 CET;2026-11-10;5.9846\r\n"
    "B;Business;JPY;Japanese yen;NOK;Norwegian krone;SP;Spot;4;false;2;Hundreds;C;ECB concertation time 14:15 CET;2026-11-11;5.9908\r\n"
    "B;Business;JPY;Japanese yen;NOK;Norwegian krone;SP;Spot;4;false;2;Hundreds;C;ECB concertation time 14:15 CET;2026-11-12;5.9691\r\n";
/* CoinGecko's answer (bitcoin, vs_currency=nok, days=30, interval=daily), 2026-11-13 09:18 UTC, without its
   market_caps and total_volumes */
static const char btc_nok[] = "{\"prices\":[[1792022400000,774898.1040617452],[1792108800000,778540.4825969964],[1792195200000,768580.5907490051],[1792281600000,767493.4561665673],[1792368000000,773459.1023149301],[1792454400000,771603.1309262724],[1792540800000,780111.618404427],[1792627200000,787399.735400581],[1792713600000,758630.8845524334],[1792800000000,756194.8049679559],[1792886400000,753474.6111586688],[1792972800000,745654.2180768467],[1793059200000,745356.2128246878],[1793145600000,754361.0123472981],[1793232000000,751893.0485939824],[1793318400000,719907.4550813531],[1793404800000,711587.3184263493],[1793491200000,713851.0817652891],[1793577600000,721812.8249144698],[1793664000000,713872.9912940208],[1793750400000,717582.7327583198],[1793836800000,747781.7768588108],[1793923200000,707103.801477383],[1794009600000,716826.5853427355],[1794096000000,716836.9506751421],[1794182400000,707125.2215276941],[1794268800000,707776.5408874542],[1794355200000,700912.0431199573],[1794441600000,706948.3385914712],[1794528000000,682747.5896281559],[1794561530000,658464.4571587405]],\"market_caps\":[[1792022400000,1.5e13]],\"total_volumes\":[]}";

/* Every value of a chart series is a whole number 0..99, the lowest 0 and the highest 99 */
static unsigned series_points(const char *s)
{
    unsigned n = 0, lo = 100, hi = 0;
    for (const char *p = s; *p;) {
        char *end;
        long x = strtol(p, &end, 10);
        if (end == p || x < 0 || x > 99 || (*end && *end != ' '))
            return 0;
        if ((unsigned)x < lo)
            lo = (unsigned)x;
        if ((unsigned)x > hi)
            hi = (unsigned)x;
        n++;
        p = *end ? end + 1 : end;
    }
    return lo == 0 && hi == 99 ? n : 0;
}

static void test_fx(void)
{
    static esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    char err[ESL_SOURCE_ERR_MAX + 16], url[ESL_SOURCE_URL_MAX];
    uint8_t n;
    esl_values_t v;
    uint32_t data_at;
    const uint32_t now = 1794561530u; /* 2026-11-13 09:18 UTC */
    static const char ok[] =
        "{\"sources\":[{\"id\":\"usd\",\"type\":\"fx\",\"currency\":\"usd\",\"layout\":\"text\"},"
        "{\"id\":\"usdeur\",\"type\":\"fx\",\"currency\":\"USD\",\"in\":\"EUR\",\"lang\":\"en\",\"days\":5,\"layout\":\"text\"},"
        "{\"id\":\"yen\",\"type\":\"fx\",\"currency\":\"JPY\",\"layout\":\"text\"},"
        "{\"id\":\"eurnok\",\"type\":\"fx\",\"currency\":\"NOK\",\"in\":\"EUR\",\"unit\":100,\"layout\":\"text\"}]}";
    CHECK(esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));
    CHECK(n == 4 && cfg[0].type == ESL_SOURCE_FX && cfg[0].every_min == 60 && esl_source_own_pace(&cfg[0]));
    CHECK_STR(cfg[0].fx.cur, "USD");
    CHECK_STR(cfg[0].fx.in, "NOK");
    CHECK(cfg[0].fx.days == 30 && cfg[0].fx.unit == 0 && cfg[0].nfields == 4);
    CHECK(esl_source_url(&cfg[0], url, sizeof(url)) > 0);
    CHECK_STR(url, "https://data.norges-bank.no/api/data/EXR/B.USD.NOK.SP?format=csv&lastNObservations=31&locale=en");
    CHECK(esl_source_url(&cfg[1], url, sizeof(url)) > 0);
    CHECK_STR(url, "https://data.norges-bank.no/api/data/EXR/B.USD+EUR.NOK.SP?format=csv&lastNObservations=6&locale=en");
    CHECK(esl_source_url(&cfg[3], url, sizeof(url)) > 0);
    CHECK_STR(url, "https://data.norges-bank.no/api/data/EXR/B.EUR.NOK.SP?format=csv&lastNObservations=31&locale=en");
    {
        static char js[ESL_SOURCE_CFG_MAX];
        esl_sb_t sb;
        esl_sb_init(&sb, js, sizeof(js));
        esl_source_write_cfg(&cfg[1], &sb);
        CHECK(HAS(js, "\"currency\":\"USD\",\"in\":\"EUR\",\"days\":5"));
        CHECK(!HAS(js, "\"unit\""));
        esl_sb_init(&sb, js, sizeof(js));
        esl_source_write_cfg(&cfg[3], &sb);
        CHECK(HAS(js, "\"unit\":100"));
    }
    static const struct {
        const char *src, *msg;
    } bad[] = {
        {"", "sources[0].currency: a currency code"},
        {"\"currency\":\"US\"", "sources[0].currency: "},
        {"\"currency\":\"U$D\"", "sources[0].currency: "},
        {"\"currency\":\"USD\",\"in\":\"usd\"", "sources[0].in: another currency"},
        {"\"currency\":\"USD\",\"in\":7", "sources[0].in: the currency code"},
        {"\"currency\":\"USD\",\"days\":31", "sources[0].days: a whole number from 5 to 30"},
        {"\"currency\":\"USD\",\"unit\":10", "sources[0].unit: 1 or 100"},
        {"\"currency\":\"USD\",\"every_min\":15", "sources[0].every_min: a whole number from 60"},
        {"\"currency\":\"USD\",\"fields\":[\"{price}\"]", "sources[0].fields[0]: {price} is not a value of type fx"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char js[256];
        snprintf(js, sizeof(js), "{\"sources\":[{\"id\":\"f\",\"type\":\"fx\",\"layout\":\"text\"%s%s}]}",
                 bad[i].src[0] ? "," : "", bad[i].src);
        CHECK(!esl_sources_parse(js, strlen(js), cfg, &n, err, sizeof(err)));
        if (strncmp(err, bad[i].msg, strlen(bad[i].msg)) != 0)
            printf("  fx cfg %zu: %s\n", i, err);
        CHECK(strncmp(err, bad[i].msg, strlen(bad[i].msg)) == 0);
    }
    CHECK(esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));

    /* USD in NOK: 31 days, the first only for the change */
    CHECK(esl_source_values(&cfg[0], fx_usd, sizeof(fx_usd) - 1, now, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "pair"), "USD/NOK");
    CHECK_STR(esl_values_get(&v, "unit"), "1");
    CHECK_STR(esl_values_get(&v, "rate"), "9,5325");
    CHECK_STR(esl_values_get(&v, "rate2"), "9,53");
    CHECK_STR(esl_values_get(&v, "chg"), "-0,0069");
    CHECK_STR(esl_values_get(&v, "pct"), "-0,1");
    CHECK_STR(esl_values_get(&v, "pctp"), "-1,2"); /* from 9.6494 on 2 October */
    CHECK_STR(esl_values_get(&v, "hi"), "9,6494");
    CHECK_STR(esl_values_get(&v, "lo"), "9,2278");
    CHECK_STR(esl_values_get(&v, "date"), "12.11.");
    CHECK_STR(esl_values_get(&v, "day"), "i går");
    CHECK_STR(esl_values_get(&v, "days"), "30");
    CHECK_STR(esl_values_get(&v, "period"), "30 dager");
    CHECK(series_points(esl_values_get(&v, "chart")) == 30);
    CHECK(strlen(esl_values_get(&v, "chart")) <= ESL_VALUE_LONG);
    CHECK(data_at == 1794441600u);
    esl_content_t c;
    CHECK(esl_source_content(&cfg[0], &v, &c, err, sizeof(err)));
    /* the same day: today */
    CHECK(esl_source_values(&cfg[0], fx_usd, sizeof(fx_usd) - 1, data_at + 17 * 3600, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "day"), "i dag");

    /* USD in EUR, in English: day by day, 9.5325 / 10.563 */
    CHECK(esl_source_values(&cfg[1], fx_usd_eur, sizeof(fx_usd_eur) - 1, now, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "pair"), "USD/EUR");
    CHECK_STR(esl_values_get(&v, "rate"), "0.9024");
    CHECK_STR(esl_values_get(&v, "day"), "yesterday");
    CHECK_STR(esl_values_get(&v, "period"), "3 days");
    CHECK(series_points(esl_values_get(&v, "chart")) == 3);
    /* the yen: the price of 100, as Norges Bank gives it */
    CHECK(esl_source_values(&cfg[2], fx_jpy, sizeof(fx_jpy) - 1, now, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "pair"), "100 JPY/NOK");
    CHECK_STR(esl_values_get(&v, "rate"), "5,9691");
    CHECK_STR(esl_values_get(&v, "chg"), "-0,0217");
    /* NOK in EUR, for 100 kroner: 100 / 10.563 */
    CHECK(esl_source_values(&cfg[3], fx_usd_eur, sizeof(fx_usd_eur) - 1, now, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "pair"), "100 NOK/EUR");
    CHECK_STR(esl_values_get(&v, "rate"), "9,4670");
    /* answers that are not rates, or not of the currency */
    CHECK(!esl_source_values(&cfg[0], "<html>", 6, now, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(err, "the answer is not Norges Bank's exchange rates in CSV");
    CHECK(!esl_source_values(&cfg[0], "", 0, now, cet, &v, &data_at, err, sizeof(err)));
    CHECK(!esl_source_values(&cfg[0], fx_jpy, sizeof(fx_jpy) - 1, now, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(err, "the answer has no rates for USD in NOK");
}

static void test_btc(void)
{
    static esl_source_cfg_t cfg[ESL_SOURCE_MAX];
    char err[ESL_SOURCE_ERR_MAX + 16], url[ESL_SOURCE_URL_MAX];
    uint8_t n;
    esl_values_t v;
    uint32_t data_at;
    const uint32_t now = 1794561600u;
    static const char ok[] = "{\"sources\":[{\"id\":\"btc\",\"type\":\"btc\",\"layout\":\"text\"},"
                             "{\"id\":\"btcusd\",\"type\":\"btc\",\"currency\":\"USD\",\"days\":7,\"lang\":\"en\","
                             "\"layout\":\"text\"}]}";
    CHECK(esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));
    CHECK(n == 2 && cfg[0].type == ESL_SOURCE_BTC && cfg[0].every_min == 15 && esl_source_own_pace(&cfg[0]));
    CHECK_STR(cfg[0].btc.cur, "nok");
    CHECK_STR(cfg[1].btc.cur, "usd");
    CHECK(esl_source_url(&cfg[0], url, sizeof(url)) > 0);
    CHECK_STR(url, "https://api.coingecko.com/api/v3/coins/bitcoin/market_chart?vs_currency=nok&days=30&interval=daily");
    CHECK(esl_source_url(&cfg[1], url, sizeof(url)) > 0);
    CHECK(HAS(url, "vs_currency=usd&days=7&"));
    static const char *const bad[] = {"\"currency\":\"no\"", "\"currency\":\"n0k\"", "\"currency\":\"nokkkk\"",
                                      "\"days\":6", "\"days\":31", "\"every_min\":10"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char js[256];
        snprintf(js, sizeof(js), "{\"sources\":[{\"id\":\"b\",\"type\":\"btc\",\"layout\":\"text\",%s}]}", bad[i]);
        CHECK(!esl_sources_parse(js, strlen(js), cfg, &n, err, sizeof(err)));
    }
    CHECK(esl_sources_parse(ok, sizeof(ok) - 1, cfg, &n, err, sizeof(err)));

    CHECK(esl_source_values(&cfg[0], btc_nok, sizeof(btc_nok) - 1, now, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "cur"), "NOK");
    CHECK_STR(esl_values_get(&v, "price"), "658 464");
    CHECK_STR(esl_values_get(&v, "pct"), "-3,6");   /* since 682 748 at 00:00 UTC */
    CHECK_STR(esl_values_get(&v, "today"), "i dag");
    CHECK_STR(esl_values_get(&v, "pctp"), "-15,0"); /* since 774 898 on 14 October */
    CHECK_STR(esl_values_get(&v, "hi"), "787 400");
    CHECK_STR(esl_values_get(&v, "lo"), "658 464");
    CHECK_STR(esl_values_get(&v, "updated"), "10:18");
    CHECK_STR(esl_values_get(&v, "date"), "13.11.");
    CHECK_STR(esl_values_get(&v, "days"), "30");
    CHECK_STR(esl_values_get(&v, "period"), "30 dager");
    CHECK(series_points(esl_values_get(&v, "chart")) == 31);
    CHECK(data_at == 1794561530u);
    esl_content_t c;
    CHECK(esl_source_content(&cfg[0], &v, &c, err, sizeof(err)));
    CHECK(esl_source_values(&cfg[1], btc_nok, sizeof(btc_nok) - 1, now, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "cur"), "USD");
    CHECK_STR(esl_values_get(&v, "pct"), "-3.6");
    CHECK_STR(esl_values_get(&v, "period"), "30 days");
    /* a price under 100 has two decimals */
    static const char small[] = "{\"prices\":[[1794528000000,12.5],[1794561530000,99.994]]}";
    CHECK(esl_source_values(&cfg[1], small, sizeof(small) - 1, now, cet, &v, &data_at, err, sizeof(err)));
    CHECK_STR(esl_values_get(&v, "price"), "99.99");
    CHECK_STR(esl_values_get(&v, "chart"), "0 99");
    static const char *const no[] = {"[]", "{\"prices\":{}}", "{\"prices\":[[1,2]]}", "{\"error\":\"rate limit\"}"};
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
        CHECK(!esl_source_values(&cfg[0], no[i], strlen(no[i]), now, cet, &v, &data_at, err, sizeof(err)));

    /* the shared helpers */
    char buf[ESL_VALUE_LONG + 1];
    esl_source_fmt(1234567.891, 2, false, true, false, buf, sizeof(buf));
    CHECK_STR(buf, "1 234 567,89");
    esl_source_fmt(-0.004, 2, true, false, true, buf, sizeof(buf));
    CHECK_STR(buf, "0.00");
    esl_source_fmt(0.5, 0, true, false, true, buf, sizeof(buf));
    CHECK_STR(buf, "+1");
    esl_source_fmt(-999.5, 0, false, true, false, buf, sizeof(buf));
    CHECK_STR(buf, "-1 000");
    double same[] = {3, 3, 3};
    esl_source_series(same, 3, buf, sizeof(buf));
    CHECK_STR(buf, "50 50 50");
    double many[40];
    for (unsigned i = 0; i < 40; i++)
        many[i] = i;
    esl_source_series(many, 40, buf, sizeof(buf));
    CHECK(strlen(buf) <= ESL_VALUE_LONG && buf[strlen(buf) - 1] != ' ');
}

void test_source(void)
{
    test_parse();
    test_times();
    test_met();
    test_content();
    test_apply();
    test_image();
    test_power();
    test_countdown();
    test_rss();
    test_ical();
    test_fx();
    test_btc();
    test_api();
}
