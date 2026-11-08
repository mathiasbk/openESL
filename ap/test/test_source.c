/* test_source.c - esl_source: data sources (the configuration, /api/sources, MET Norway's
 * forecast as values, and the content of the tags) */
#include <stdio.h>
#include <stdlib.h>
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
                        NULL, &sources, NULL, false, NULL, NULL, 0, NULL};
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
        {"{\"sources\":[{\"id\":\"a\",\"type\":\"rss\",\"lat\":1,\"lon\":1,\"layout\":\"w\"}]}", "sources[0].type: one of \"met\" (the forecast from MET Norway)"},
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
    esl_poll_t p = {2, 2950, 0, ESL_POLL_F_IMAGE, model, true, 0, false, 0, 0, 0, 0};
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

void test_source(void)
{
    test_parse();
    test_times();
    test_met();
    test_content();
    test_apply();
    test_image();
    test_api();
}
