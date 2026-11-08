#include "tinytest.h"
#include "esl_clock.h"
#include "esl_text.h"
#include "esl_layout.h"
#include <stdio.h>
#include <string.h>

static const char *fill(const char *t, int32_t days)
{
    static char out[ESL_CLOCK_TEXT_MAX + 1];
    size_t n = esl_clock_fill((const uint8_t *)t, strlen(t), days, (uint8_t *)out, ESL_CLOCK_TEXT_MAX);
    out[n] = 0;
    return out;
}

static uint8_t res(const char *t)
{
    return esl_clock_res((const uint8_t *)t, strlen(t));
}

static void test_fill(void)
{
    /* Thursday 29 October 2026 is day 20755, ISO week 44 */
    CHECK_STR(fill("{weekday} {date}", 20755), "Thursday 29 October 2026");
    CHECK_STR(fill("Oppdatert {date_short}", 20755), "Oppdatert 29.10.2026");
    CHECK_STR(fill("Week {week}", 20755), "Week 44");
    CHECK_STR(fill("{weekday}", 0), "Thursday");            /* 1 January 1970 */
    CHECK_STR(fill("{date_short} {week}", 0), "01.01.1970 1");
    CHECK_STR(fill("{date} {week}", 19782), "29 February 2024 9");
    CHECK_STR(fill("{date_short} uke {week}", 20087), "30.12.2024 uke 1");  /* week 1 of 2025 */
    CHECK_STR(fill("{date_short} uke {week}", 20819), "01.01.2027 uke 53"); /* week 53 of 2026 */
    CHECK_STR(fill("{weekday}", 20758), "Sunday");
    /* Norwegian (Latin-1): the weekday as a title, the date as written in Norway */
    esl_clock_set_lang(ESL_LANG_NB);
    CHECK_INT(esl_clock_lang(), ESL_LANG_NB);
    CHECK_STR(fill("{weekday} {date}", 20755), "Torsdag 29. oktober 2026");
    CHECK_STR(fill("{weekday} {date_short} uke {week}", 20757), "L\xf8rdag 31.10.2026 uke 44");
    CHECK_STR(fill("{weekday}", 20758), "S\xf8ndag");
    esl_clock_set_lang(7); /* anything else: English */
    CHECK_INT(esl_clock_lang(), ESL_LANG_EN);
    CHECK_STR(fill("{date}", 20755), "29 October 2026");
    CHECK_INT(esl_lang_parse("nb", 2), ESL_LANG_NB);
    CHECK_INT(esl_lang_parse("en", 2), ESL_LANG_EN);
    CHECK_INT(esl_lang_parse("no", 2), -1);
    CHECK_INT(esl_lang_parse("en-GB", 5), -1);
    CHECK_STR(esl_lang_name(ESL_LANG_NB), "nb");
    CHECK_STR(esl_lang_name(ESL_LANG_EN), "en");
    /* other words in braces, and broken ones, stay as they are */
    CHECK_STR(fill("{price} {date {week}}", 20755), "{price} {date 44}");
    CHECK_STR(fill("{", 20755), "{");
    /* the time unknown: empty */
    CHECK_STR(fill("Week {week}: {date}!", -1), "Week : !");
    /* cut where the buffer ends */
    {
        uint8_t out[10];
        CHECK_INT((int)esl_clock_fill((const uint8_t *)"{date}", 6, 20755, out, sizeof(out)), 10);
        CHECK(memcmp(out, "29 Octobe", 9) == 0);
    }
}

static void test_res_and_stamp(void)
{
    CHECK_INT(res("inkl. mva"), ESL_CLOCK_NONE);
    CHECK_INT(res("{weeks}"), ESL_CLOCK_NONE);
    CHECK_INT(res("Week {week}"), ESL_CLOCK_WEEK);
    CHECK_INT(res("{week} {weekday}"), ESL_CLOCK_DAY);
    CHECK_INT(res("{date_short}"), ESL_CLOCK_DAY);

    esl_clock_set_today(-1);
    int32_t days = 5;
    CHECK(!esl_clock_today(&days));
    CHECK_INT((int)esl_clock_stamp(ESL_CLOCK_NONE), 0);
    uint32_t unknown = esl_clock_stamp(ESL_CLOCK_DAY);
    CHECK(unknown != 0);
    esl_clock_set_today(20755); /* Thursday */
    CHECK(esl_clock_today(&days) && days == 20755);
    uint32_t thu = esl_clock_stamp(ESL_CLOCK_DAY), thu_w = esl_clock_stamp(ESL_CLOCK_WEEK);
    CHECK(thu != unknown && thu != thu_w && thu != 0 && thu_w != 0);
    esl_clock_set_today(20758); /* Sunday, the same week */
    CHECK(esl_clock_stamp(ESL_CLOCK_DAY) != thu);
    CHECK(esl_clock_stamp(ESL_CLOCK_WEEK) == thu_w);
    esl_clock_set_today(20759); /* Monday */
    CHECK(esl_clock_stamp(ESL_CLOCK_WEEK) != thu_w);
    esl_clock_set_today(0);
    CHECK(esl_clock_stamp(ESL_CLOCK_DAY) != 0);
    /* English keeps the stamps from before the language setting, and another language
       changes them, so the tags are drawn again in it */
    esl_clock_set_today(20755);
    CHECK_INT((int)esl_clock_stamp(ESL_CLOCK_DAY), (int)((2u << 28) ^ 20755u));
    esl_clock_set_lang(ESL_LANG_NB);
    CHECK(esl_clock_stamp(ESL_CLOCK_DAY) != thu && esl_clock_stamp(ESL_CLOCK_DAY) != 0);
    CHECK(esl_clock_stamp(ESL_CLOCK_WEEK) != thu_w);
    esl_clock_set_lang(ESL_LANG_EN);
    CHECK(esl_clock_stamp(ESL_CLOCK_DAY) == thu);
    esl_clock_set_today(-1);
}

static void test_compile(void)
{
    static esl_layout_bin_t bin;
    char err[ESL_LAYOUT_ERR_MAX];
    const char *ok =
        "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":["
        "{\"name\":\"a\",\"fields\":[\"x\"],\"elements\":[{\"type\":\"text\",\"field\":\"x\",\"x\":0,\"y\":0,\"w\":90,\"h\":24,\"fonts\":[\"medium\"]},"
        "{\"type\":\"text\",\"text\":\"Week {week}\",\"x\":100,\"y\":0,\"w\":90,\"h\":24,\"fonts\":[\"medium\"]}]},"
        "{\"name\":\"b\",\"fields\":[\"x\"],\"elements\":[{\"type\":\"text\",\"text\":\"Week {week}\",\"x\":0,\"y\":0,\"w\":90,\"h\":24,\"fonts\":[\"medium\"]},"
        "{\"type\":\"text\",\"text\":\"{date}\",\"x\":100,\"y\":0,\"w\":90,\"h\":24,\"fonts\":[\"medium\"]}]},"
        "{\"name\":\"c\",\"fields\":[\"x\"],\"elements\":[{\"type\":\"text\",\"text\":\"{prices}\",\"x\":0,\"y\":0,\"w\":90,\"h\":24,\"fonts\":[\"medium\"]}]}]}";
    CHECK(esl_layout_compile(ok, strlen(ok), ESL_MODEL_29, &bin, err, sizeof(err)));
    CHECK_INT(bin.clock[0], ESL_CLOCK_WEEK);
    CHECK_INT(bin.clock[1], ESL_CLOCK_DAY);
    CHECK_INT(bin.clock[2], ESL_CLOCK_NONE);

    /* a fifth text with date fields in one layout */
    char many[1024];
    int n = snprintf(many, sizeof(many),
                     "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":["
                     "{\"name\":\"a\",\"fields\":[\"x\"],\"elements\":[");
    for (int i = 0; i < ESL_CLOCK_TEXTS + 1; i++)
        n += snprintf(many + n, sizeof(many) - (size_t)n,
                      "%s{\"type\":\"text\",\"text\":\"{week}\",\"x\":0,\"y\":%d,\"w\":90,\"h\":24,\"fonts\":[\"medium\"]}", i ? "," : "",
                      i * 24);
    snprintf(many + n, sizeof(many) - (size_t)n, "]}]}");
    CHECK(!esl_layout_compile(many, strlen(many), ESL_MODEL_29, &bin, err, sizeof(err)));
    CHECK(strstr(err, "at most 4 texts with date fields") != NULL);

    /* the version an image is drawn with: the file's, with the day or week in it */
    static esl_layouts_t ls;
    CHECK(esl_layout_compile(ok, strlen(ok), ESL_MODEL_29, &bin, err, sizeof(err)));
    esl_layouts_init(&ls, NULL);
    bool changed;
    uint32_t ver = esl_layouts_set(&ls, ESL_MODEL_29, ok, strlen(ok), &bin, &changed);
    CHECK(ver != 0);
    esl_clock_set_today(20755);
    uint32_t a = esl_layouts_image_ver(&ls, ESL_MODEL_29, esl_layout_name_hash("a"));
    uint32_t b = esl_layouts_image_ver(&ls, ESL_MODEL_29, esl_layout_name_hash("b"));
    CHECK(a != ver && b != ver && a != b);
    CHECK(esl_layouts_image_ver(&ls, ESL_MODEL_29, esl_layout_name_hash("c")) == ver);
    CHECK(esl_layouts_image_ver(&ls, ESL_MODEL_29, 0) == ver);                    /* built-in */
    CHECK(esl_layouts_image_ver(&ls, ESL_MODEL_29, esl_layout_name_hash("zz")) == ver);
    CHECK(esl_layouts_image_ver(&ls, ESL_MODEL_154, esl_layout_name_hash("a")) == 0); /* no file */
    esl_clock_set_today(20756);
    CHECK(esl_layouts_image_ver(&ls, ESL_MODEL_29, esl_layout_name_hash("a")) == a);
    CHECK(esl_layouts_image_ver(&ls, ESL_MODEL_29, esl_layout_name_hash("b")) != b);
    esl_clock_set_today(-1);
    esl_layouts_free(&ls);

    /* a decimal point (#402) draws again the layouts that format numbers, and only them; the
       comma (no "lang", or "nb") keeps the version from before */
    const char *nums = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"2.9\",\"layouts\":["
                       "{\"name\":\"p\",\"fields\":[\"x\"],\"elements\":[{\"type\":\"text\",\"field\":\"x\",\"x\":0,\"y\":0,\"w\":90,\"h\":24,\"fonts\":[\"medium\"],\"decimals\":2}]},"
                       "{\"name\":\"q\",\"fields\":[\"x\"],\"elements\":[{\"type\":\"text\",\"field\":\"x\",\"x\":0,\"y\":0,\"w\":90,\"h\":24,\"fonts\":[\"medium\"]}]}]}";
    CHECK(esl_layout_compile(nums, strlen(nums), ESL_MODEL_29, &bin, err, sizeof(err)));
    CHECK_INT(bin.nums[0], 1);
    CHECK_INT(bin.nums[1], 0);
    esl_layouts_init(&ls, NULL);
    ver = esl_layouts_set(&ls, ESL_MODEL_29, nums, strlen(nums), &bin, &changed);
    CHECK(ver != 0);
    uint32_t p = esl_layouts_image_ver(&ls, ESL_MODEL_29, esl_layout_name_hash("p"));
    CHECK(p == ver);
    CHECK(esl_layouts_image_ver(&ls, ESL_MODEL_29, esl_layout_name_hash("q")) == ver);
    esl_number_set_point(true);
    CHECK(esl_layouts_image_ver(&ls, ESL_MODEL_29, esl_layout_name_hash("p")) != p);
    CHECK(esl_layouts_image_ver(&ls, ESL_MODEL_29, esl_layout_name_hash("q")) == ver);
    CHECK(esl_layouts_image_ver(&ls, ESL_MODEL_154, esl_layout_name_hash("p")) == 0); /* no file */
    esl_number_set_point(false);
    CHECK(esl_layouts_image_ver(&ls, ESL_MODEL_29, esl_layout_name_hash("p")) == p);
    esl_layouts_free(&ls);
}

void test_clock(void)
{
    test_fill();
    test_res_and_stamp();
    test_compile();
}
