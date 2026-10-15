#include "tinytest.h"
#include "esl_json.h"

static int parse(const char *js, esl_jtok_t *t, unsigned max)
{
    return esl_json_parse(js, strlen(js), t, max);
}

static char sunk[256];
static size_t sunk_len;
static bool sink_fails;

static bool sink(esl_sb_t *sb)
{
    if (sink_fails)
        return false;
    memcpy(sunk + sunk_len, sb->buf, sb->len);
    sunk_len += sb->len;
    return true;
}

void test_json(void)
{
    esl_jtok_t t[64];
    const char *js;
    int n;

    js = "{\"a\": [1, \"x\", true], \"b\": {\"c\": null}, \"d\": false}";
    n = parse(js, t, 64);
    CHECK_INT(n, 12);
    CHECK_INT(t[0].type, ESL_JSON_OBJECT);
    CHECK_INT(t[0].size, 3);
    int a = esl_json_obj_get(js, t, 0, "a");
    CHECK_INT(t[a].type, ESL_JSON_ARRAY);
    CHECK_INT(t[a].size, 3);
    CHECK_INT(t[a + 1].type, ESL_JSON_NUMBER);
    CHECK(esl_json_str_eq(js, &t[a + 2], "x"));
    CHECK_INT(t[a + 3].type, ESL_JSON_TRUE);
    int b = esl_json_obj_get(js, t, 0, "b");
    CHECK_INT(t[b].type, ESL_JSON_OBJECT);
    CHECK_INT(t[esl_json_obj_get(js, t, b, "c")].type, ESL_JSON_NULL);
    CHECK_INT(t[esl_json_obj_get(js, t, 0, "d")].type, ESL_JSON_FALSE);
    CHECK_INT(esl_json_obj_get(js, t, 0, "c"), -1); /* not at top level */
    CHECK_INT(esl_json_obj_get(js, t, a, "x"), -1); /* not an object */

    /* top-level primitives, empty containers, whitespace */
    CHECK_INT(parse(" 42 ", t, 64), 1);
    CHECK_INT(parse("{}", t, 64), 1);
    CHECK_INT(parse("[ ]", t, 64), 1);
    CHECK_INT(parse("[[],{}]", t, 64), 3);
    CHECK_INT(t[0].size, 2);
    CHECK_INT(t[0].next, 3);

    /* syntax errors */
    static const char *bad[] = {
        "", "{", "}", "[1,]", "{\"a\":1,}", "{\"a\"}", "{\"a\" 1}", "{1:2}", "[1 2]",
        "\"abc", "\"a\\x\"", "\"a\\u12G4\"", "\"tab\there\"", "{} {}", "[}", "{]", "tru", "nul",
        "{\"a\":}", ",", ":",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        n = parse(bad[i], t, 64);
        if (n >= 0)
            printf("  accepted bad JSON: %s\n", bad[i]);
        CHECK(n < 0);
    }

    /* limits */
    CHECK_INT(parse("[1,2,3]", t, 3), ESL_JSON_ERR_NOMEM);
    CHECK_INT(parse("[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]", t, 64), ESL_JSON_ERR_DEPTH);
    CHECK(parse("[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]", t, 64) > 0);

    /* string decoding */
    char out[32];
    js = "\"a\\\"b\\\\c\\/\\n\\u00e6\\u00C5\\ud83d\\ude00\\ud800x\"";
    CHECK_INT(parse(js, t, 64), 1);
    n = esl_json_str_decode(js, &t[0], out, sizeof(out));
    CHECK_INT(n, 1 + 1 + 1 + 1 + 1 + 1 + 1 + 2 + 2 + 4 + 1 + 1);
    CHECK_MEM(out, "a\"b\\c/\n\xC3\xA6\xC3\x85\xF0\x9F\x98\x80?x", (size_t)n);
    CHECK_INT(esl_json_str_decode(js, &t[0], out, 4), -2);

    js = "\"\xC3\xA6\xC3\xB8\"";
    CHECK_INT(parse(js, t, 64), 1);
    n = esl_json_str_decode(js, &t[0], out, sizeof(out));
    CHECK_INT(n, 4);
    CHECK_MEM(out, "\xC3\xA6\xC3\xB8", 4);

    /* writer */
    char buf[64];
    esl_sb_t sb;
    esl_sb_init(&sb, buf, sizeof(buf));
    esl_sb_json_latin1(&sb, (const uint8_t *)"\xC5pent \"9\"\n\x01", 11);
    CHECK_STR(buf, "\"\xC3\x85pent \\\"9\\\"\\n\\u0001\"");
    CHECK(!sb.overflow);

    esl_sb_init(&sb, buf, 8);
    esl_sb_puts(&sb, "1234567");
    CHECK(!sb.overflow);
    esl_sb_putc(&sb, '8');
    CHECK(sb.overflow);
    CHECK_STR(buf, "1234567");
    esl_sb_init(&sb, buf, 8);
    esl_sb_printf(&sb, "%d", 123456789);
    CHECK(sb.overflow);

    /* streaming: a full buffer goes to the sink instead of overflowing */
    sunk_len = 0;
    esl_sb_init(&sb, buf, 8);
    esl_sb_set_sink(&sb, sink, NULL);
    esl_sb_puts(&sb, "1234567");
    esl_sb_putc(&sb, '8'); /* no esl_sb_stream yet: still an overflow */
    CHECK(sb.overflow);
    CHECK_INT((int)sunk_len, 0);
    esl_sb_init(&sb, buf, 8);
    esl_sb_set_sink(&sb, sink, NULL);
    esl_sb_stream(&sb, 200);
    CHECK(sb.streaming);
    CHECK_INT(sb.status, 200);
    esl_sb_puts(&sb, "123456789abcdef"); /* 7 to the sink, then 7 more, 1 left */
    CHECK(!sb.overflow);
    CHECK_INT((int)sb.sent, 14);
    CHECK_STR(buf, "f");
    esl_sb_printf(&sb, "%d", 123456); /* fits behind "f" */
    CHECK(!sb.overflow);
    CHECK_INT((int)sb.sent, 14);
    CHECK_STR(buf, "f123456");
    esl_sb_printf(&sb, "%d", 1234567); /* does not fit behind it: flush, then it fits */
    CHECK(!sb.overflow);
    CHECK_INT((int)sb.sent, 21);
    CHECK_STR(buf, "1234567");
    esl_sb_printf(&sb, "%d", 12345678); /* never fits in 8: what was there is flushed, then overflow */
    CHECK(sb.overflow);
    CHECK_INT((int)sb.sent, 28);
    esl_sb_init(&sb, buf, 8);
    esl_sb_set_sink(&sb, sink, NULL);
    esl_sb_stream(&sb, 200);
    esl_sb_puts(&sb, "xy");
    esl_sb_flush(&sb);
    CHECK_INT((int)sb.sent, 2);
    CHECK_INT((int)sb.len, 0);
    CHECK_INT((int)sunk_len, 30);
    CHECK_MEM(sunk, "1234567" "89abcde" "f123456" "1234567" "xy", 30);
    /* a sink that fails ends the reply */
    sink_fails = true;
    esl_sb_puts(&sb, "123456789");
    CHECK(sb.overflow);
    sink_fails = false;
}
