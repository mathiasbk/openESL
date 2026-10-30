#include "tinytest.h"
#include "esl_api.h"
#include "esl_wifi.h"
#include <stdlib.h>

/* A fake radio: remembers what was saved. */
static struct {
    esl_wifi_status_t st;
    char pass[ESL_WIFI_PASS_MAX + 1];
    char token[ESL_WIFI_TOKEN_MAX + 1];
    int sets, fail;
} fake;

static void fake_status(void *ctx, esl_wifi_status_t *out)
{
    (void)ctx;
    *out = fake.st;
}

static int fake_set(void *ctx, const char *ssid, const char *pass)
{
    (void)ctx;
    if (fake.fail)
        return -1;
    fake.sets++;
    strcpy(fake.st.ssid, ssid);
    strcpy(fake.pass, pass);
    fake.st.connected = false;
    fake.st.mode = ssid[0] ? ESL_WIFI_STA : ESL_WIFI_SETUP;
    return 0;
}

static int fake_set_token(void *ctx, const char *token)
{
    (void)ctx;
    strcpy(fake.token, token);
    fake.st.token_set = true;
    return 0;
}

static int fake_set_hostname(void *ctx, const char *name)
{
    (void)ctx;
    strcpy(fake.st.hostname, name);
    return 0;
}

static const esl_wifi_ops_t ops = {NULL, fake_status, fake_set, fake_set_token, NULL, fake_set_hostname};

static void reset_fake(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.st.mode = ESL_WIFI_SETUP;
    strcpy(fake.st.setup_ssid, "openesl-1A2B");
    strcpy(fake.st.setup_ip, "192.168.4.1");
}

static void test_form(void)
{
    char v[16];
    const char *b = "ssid=My+Net%C3%A6&pass=&token=a%26b%3Dc&x";
    CHECK_INT(esl_form_get(b, strlen(b), "ssid", v, sizeof(v)), 8);
    CHECK_STR(v, "My Net\xC3\xA6");
    CHECK_INT(esl_form_get(b, strlen(b), "pass", v, sizeof(v)), 0);
    CHECK_STR(v, "");
    CHECK_INT(esl_form_get(b, strlen(b), "token", v, sizeof(v)), 5);
    CHECK_STR(v, "a&b=c");
    CHECK_INT(esl_form_get(b, strlen(b), "x", v, sizeof(v)), -1);   /* no '=' */
    CHECK_INT(esl_form_get(b, strlen(b), "ss", v, sizeof(v)), -1);  /* prefix only */
    CHECK_INT(esl_form_get(b, strlen(b), "nope", v, sizeof(v)), -1);
    CHECK_INT(esl_form_get("a=%4", 4, "a", v, sizeof(v)), -3);
    CHECK_INT(esl_form_get("a=%zz", 5, "a", v, sizeof(v)), -3);
    CHECK_INT(esl_form_get("a=%00", 5, "a", v, sizeof(v)), -3);
    CHECK_INT(esl_form_get("a=0123456789abcdef", 18, "a", v, sizeof(v)), -2);
    CHECK_INT(esl_form_get("a=0123456789abcde", 17, "a", v, sizeof(v)), 15);
    /* the length bounds the search, not a NUL */
    CHECK_INT(esl_form_get("a=1&b=2", 3, "b", v, sizeof(v)), -1);
}

static void test_check(void)
{
    CHECK(esl_wifi_check("net", 3, "", 0) == NULL);
    CHECK(esl_wifi_check("net", 3, "12345678", 8) == NULL);
    CHECK(esl_wifi_check("net", 3, "1234567", 7) != NULL);
    CHECK(esl_wifi_check("123456789012345678901234567890123", 33, "", 0) != NULL);
    CHECK(esl_wifi_check("net", 3, "1234567\n", 8) != NULL);
    char p64[65];
    memset(p64, 'x', 64);
    p64[64] = 0;
    CHECK(esl_wifi_check("net", 3, p64, 63) == NULL);
    CHECK(esl_wifi_check("net", 3, p64, 64) != NULL);
}

static char buf[ESL_WIFI_PAGE_SIZE];

static esl_api_lock_t *post_lock;
static uint64_t post_now;

static int post(const char *token, const char *body, esl_sb_t *out)
{
    esl_sb_init(out, buf, sizeof(buf));
    return esl_wifi_setup_post(&ops, token, "1.0", body, strlen(body), post_lock, post_now, out);
}

static void test_setup_page(void)
{
    esl_sb_t out;
    reset_fake();
    strcpy(fake.st.scan[0], "Home<1>");
    fake.st.scan_rssi[0] = -48;
    strcpy(fake.st.scan[1], "Cafe \"free\"");
    fake.st.scan_rssi[1] = -80;
    fake.st.scan_count = 2;

    esl_sb_init(&out, buf, sizeof(buf));
    esl_wifi_page(&out, &fake.st, false, "1.0", NULL);
    CHECK(!out.overflow);
    CHECK(HAS(buf, "no network saved"));
    CHECK(HAS(buf, "<option>Home&lt;1&gt;</option>"));
    /* the name is read from the option's text, which keeps leading, trailing and double spaces */
    CHECK(HAS(buf, "textContent"));
    CHECK(HAS(buf, "Cafe &quot;free&quot;"));
    CHECK(HAS(buf, "no token is set"));
    CHECK(!HAS(buf, "Current API token"));
    CHECK(HAS(buf, "openesl-1A2B"));

    /* a full scan list with long names still fits */
    for (unsigned i = 0; i < ESL_WIFI_SCAN_MAX; i++)
        strcpy(fake.st.scan[i], "\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"");
    fake.st.scan_count = ESL_WIFI_SCAN_MAX;
    strcpy(fake.st.ssid, fake.st.scan[0]);
    esl_sb_init(&out, buf, sizeof(buf));
    esl_wifi_page(&out, &fake.st, true, "1.0", "Saved.");
    CHECK(!out.overflow);
    CHECK(HAS(buf, "Current API token"));

    /* first setup: no token yet, a new one is optional */
    reset_fake();
    CHECK_INT(post("", "ssid=Home&pass=secret12&new_token=", &out), 200);
    CHECK_STR(fake.st.ssid, "Home");
    CHECK_STR(fake.pass, "secret12");
    CHECK_STR(fake.token, "");
    CHECK(HAS(buf, "joins <b>Home</b>."));
    CHECK(HAS(buf, "http://openesl.local/")); /* no name known: the old default */

    reset_fake();
    strcpy(fake.st.hostname, "openesl-1a2b");
    CHECK_INT(post("", "ssid=Home&new_token=", &out), 200);
    CHECK(HAS(buf, "http://openesl-1a2b.local/"));

    reset_fake();
    CHECK_INT(post(NULL, "ssid=Home&new_token=longtoken1", &out), 200);
    CHECK_STR(fake.token, "longtoken1");
    CHECK_STR(fake.pass, "");
    CHECK(HAS(buf, "new API token"));

    /* bad input changes nothing */
    reset_fake();
    CHECK_INT(post("", "ssid=Home&new_token=short", &out), 422);
    CHECK_INT(post("", "ssid=Home&new_token=has+space", &out), 422);
    CHECK_INT(post("", "ssid=&pass=", &out), 422);
    CHECK_INT(post("", "pass=12345678", &out), 422);
    CHECK_INT(post("", "ssid=Home&pass=1234", &out), 422);
    CHECK_INT(post("", "ssid=123456789012345678901234567890123", &out), 422);
    CHECK_INT(fake.sets, 0);
    CHECK_STR(fake.token, "");

    /* with a token: the form must carry it */
    CHECK_INT(post("s3cret", "ssid=Home", &out), 401);
    CHECK_INT(post("s3cret", "ssid=Home&token=s3cre", &out), 401);
    CHECK_INT(post("s3cret", "ssid=Home&token=s3cret2", &out), 401);
    CHECK_INT(post("s3cret", "ssid=Home&token=", &out), 401);
    CHECK_INT(fake.sets, 0);
    CHECK_INT(post("s3cret", "ssid=Home&token=s3cret&new_token=another1", &out), 200);
    CHECK_STR(fake.token, "another1");
    CHECK_INT(fake.sets, 1);

    /* the API's lockout counts the form's wrong tokens too, and the form is
     * refused while the API is locked; without a token there is nothing to guess */
    esl_api_lock_t lock = {0, 0};
    post_lock = &lock;
    post_now = 5000;
    for (int i = 0; i < ESL_API_AUTH_TRIES - 1; i++)
        CHECK_INT(post("s3cret", "ssid=Home&token=nope", &out), 401);
    CHECK_INT(post("s3cret", "ssid=Home&token=s3cret", &out), 200); /* a right one resets the count */
    for (int i = 0; i < ESL_API_AUTH_TRIES; i++)
        CHECK_INT(post("s3cret", "ssid=Home&token=nope", &out), 401);
    CHECK_INT(post("s3cret", "ssid=Home&token=s3cret", &out), 429);
    CHECK(strstr(buf, "Too many wrong API tokens. Try again in 60 s.") != NULL);
    post_now = 5000 + 59 * 1000;
    CHECK_INT(post("s3cret", "ssid=Home&token=s3cret", &out), 429);
    CHECK(strstr(buf, "Try again in 1 s.") != NULL);
    CHECK_INT(post("", "ssid=Home", &out), 200); /* no token set: the lock does not apply */
    post_now = 5000 + 60 * 1000;
    CHECK_INT(post("s3cret", "ssid=Home&token=s3cret", &out), 200);
    post_lock = NULL;

    /* the radio side failing */
    fake.fail = 1;
    CHECK_INT(post("", "ssid=Home", &out), 500);
}

#define AUTH "Bearer tok12345"

static int wreq(const esl_api_t *api, const char *method, const char *body, char *resp, size_t cap)
{
    esl_sb_t out;
    esl_sb_init(&out, resp, cap);
    return esl_api_handle(api, method, "/api/wifi", AUTH, body, body ? strlen(body) : 0, 0, &out);
}

static void test_api_wifi(void)
{
    static esl_store_t st;
    static char resp[ESL_API_RESP_SIZE(4)];
    esl_sb_t out;
    esl_store_free(&st);
    esl_store_init(&st, 4, NULL, 0);
    esl_api_t api = {&st, "tok12345", "1.0", NULL, NULL, NULL, NULL, NULL, NULL, NULL, false, NULL, NULL, NULL, NULL, NULL, false, NULL, NULL};

    esl_sb_init(&out, resp, sizeof(resp));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/wifi", AUTH, NULL, 0, 0, &out), 404);
    api.wifi = &ops;
    reset_fake();

#define REQ(m, b) wreq(&api, m, b, resp, sizeof(resp))

    esl_sb_init(&out, resp, sizeof(resp));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/wifi", NULL, NULL, 0, 0, &out), 401);
    CHECK_INT(REQ("GET", NULL), 200);
    CHECK_STR(resp, "{\"mode\":\"setup\",\"ssid\":null,\"connected\":false,\"setup_ssid\":\"openesl-1A2B\",\"token_set\":false,\"networks\":[]}");

    CHECK_INT(REQ("PUT", "{\"ssid\":\"Home\",\"password\":\"secret12\"}"), 200);
    CHECK_STR(fake.pass, "secret12");
    CHECK(HAS(resp, "\"mode\":\"sta\",\"ssid\":\"Home\""));
    CHECK(!HAS(resp, "secret12"));

    fake.st.connected = true;
    strcpy(fake.st.ip, "10.0.0.7");
    fake.st.rssi = -55;
    fake.st.setup_ssid[0] = 0;
    CHECK_INT(REQ("GET", NULL), 200);
    CHECK(HAS(resp, "\"connected\":true,\"ip\":\"10.0.0.7\",\"rssi\":-55,\"setup_ssid\":null,\"token_set\":false"));

    /* open network, escapes, forgetting */
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Caf\\u00e9 \\\"x\\\"\"}"), 200);
    CHECK_STR(fake.st.ssid, "Caf\xC3\xA9 \"x\"");
    CHECK_STR(fake.pass, "");
    CHECK(HAS(resp, "\"ssid\":\"Caf\xC3\xA9 \\\"x\\\"\""));
    CHECK_INT(REQ("PUT", "{\"ssid\":null}"), 200);
    CHECK(HAS(resp, "\"mode\":\"setup\",\"ssid\":null"));

    int sets = fake.sets;
    CHECK_INT(REQ("PUT", "{}"), 422);
    CHECK_INT(REQ("PUT", "{\"ssid\":5}"), 422);
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Home\",\"password\":\"short\"}"), 422);
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Home\",\"password\":7}"), 422);
    CHECK_INT(REQ("PUT", "{\"ssid\":\"123456789012345678901234567890123\"}"), 422);
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Home\",\"password\":\"1234567890123456789012345678901234567890123456789012345678901234\"}"), 422);
    CHECK_INT(REQ("PUT", "{\"ssid\":\"a\\u0000b\"}"), 422);
    CHECK_INT(REQ("PUT", NULL), 400);
    CHECK_INT(REQ("DELETE", NULL), 405);
    CHECK_INT(fake.sets, sets);
    fake.fail = 1;
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Home\"}"), 500);
    fake.fail = 0;

    /* a new token, with or without the network (the page sets both after flashing) */
    sets = fake.sets;
    CHECK_INT(REQ("PUT", "{\"token\":\"newtoken9\"}"), 200);
    CHECK_STR(fake.token, "newtoken9");
    CHECK_INT(fake.sets, sets);
    CHECK(!HAS(resp, "newtoken9"));
    CHECK(HAS(resp, "\"token_set\":true"));
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Home\",\"password\":\"secret12\",\"token\":\"tok-\\u0041bcdefg\"}"), 200);
    CHECK_STR(fake.token, "tok-Abcdefg");
    CHECK_STR(fake.st.ssid, "Home");
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Home\",\"token\":null}"), 200);
    CHECK_STR(fake.token, "tok-Abcdefg");
    /* a bad token saves nothing, not even the network */
    sets = fake.sets;
    strcpy(fake.token, "kept");
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Other\",\"token\":\"short\"}"), 422);
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Other\",\"token\":\"has space1\"}"), 422);
    CHECK_INT(REQ("PUT", "{\"token\":12345678}"), 422);
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Other\",\"password\":\"short\",\"token\":\"newtoken9\"}"), 422);
    char longtok[200] = "{\"token\":\"";
    for (int i = 0; i < 129; i++)
        strcat(longtok, "x");
    strcat(longtok, "\"}");
    CHECK_INT(REQ("PUT", longtok), 422);
    CHECK_INT(fake.sets, sets);
    CHECK_STR(fake.token, "kept");

    /* the name on the network */
    strcpy(fake.st.hostname, "openesl");
    CHECK_INT(REQ("GET", NULL), 200);
    CHECK(HAS(resp, "\"token_set\":true,\"hostname\":\"openesl\","));
    CHECK_INT(REQ("PUT", "{\"hostname\":\"Butikk-AP-2\"}"), 200);
    CHECK_STR(fake.st.hostname, "Butikk-AP-2");
    CHECK_INT(fake.sets, sets);
    CHECK(HAS(resp, "\"hostname\":\"Butikk-AP-2\""));
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Home\",\"hostname\":\"butikk\",\"token\":\"newtoken9\"}"), 200);
    CHECK_STR(fake.st.hostname, "butikk");
    CHECK_STR(fake.token, "newtoken9");
    CHECK_INT(fake.sets, sets + 1);
    sets = fake.sets;
    CHECK_INT(REQ("PUT", "{\"hostname\":\"\"}"), 422);
    CHECK_INT(REQ("PUT", "{\"hostname\":\"-ap\"}"), 422);
    CHECK_INT(REQ("PUT", "{\"hostname\":\"ap-\"}"), 422);
    CHECK_INT(REQ("PUT", "{\"hostname\":\"my ap\"}"), 422);
    CHECK_INT(REQ("PUT", "{\"hostname\":\"butikk.local\"}"), 422);
    CHECK_INT(REQ("PUT", "{\"hostname\":\"123456789012345678901234567890123\"}"), 422);
    CHECK_INT(REQ("PUT", "{\"hostname\":5}"), 422);
    CHECK_INT(REQ("PUT", "{\"ssid\":\"Other\",\"hostname\":\"bad name\"}"), 422);
    CHECK_INT(fake.sets, sets);
    CHECK_STR(fake.st.hostname, "butikk");
    CHECK_INT(REQ("PUT", "{\"hostname\":null,\"ssid\":\"Home\"}"), 200);
    CHECK_STR(fake.st.hostname, "butikk");

    esl_sb_init(&out, resp, sizeof(resp));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/wifi/", AUTH, NULL, 0, 0, &out), 200);
    esl_sb_init(&out, resp, sizeof(resp));
    CHECK_INT(esl_api_handle(&api, "GET", "/api/wifix", AUTH, NULL, 0, 0, &out), 404);
#undef REQ
}

void test_wifi(void)
{
    test_form();
    test_check();
    test_setup_page();
    test_api_wifi();
}
