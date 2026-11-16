#include "tinytest.h"
#include "esl_api.h"
#include "esl_remote.h"

/* The SoluM remote (esl_remote.h): frames as they were heard on the air */

#define AUTH "Bearer s3cret"

static uint8_t frame_w[14] = {0x41, 0x88, 0x07, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0xBE, 0x19, 0x01, 0x00, 0xA5, 0x5A};

static void frame(uint8_t *f, uint8_t seq, uint8_t button)
{
    memcpy(f, frame_w, sizeof(frame_w));
    f[2] = seq;
    f[10] = button;
}

static void test_parse(void)
{
    uint8_t f[14], b = 0, s = 0;
    CHECK(esl_remote_parse(frame_w, 14, &b, &s));
    CHECK_INT(b, ESL_REMOTE_W);
    CHECK_INT(s, 7);
    CHECK(esl_remote_parse(frame_w, 12, &b, &s)); /* without the FCS */
    frame(f, 9, ESL_REMOTE_RIGHT);
    CHECK(esl_remote_parse(f, 14, &b, &s) && b == ESL_REMOTE_RIGHT && s == 9);
    CHECK(!esl_remote_parse(f, 13, &b, &s));
    frame(f, 9, 0);
    CHECK(!esl_remote_parse(f, 14, &b, &s));
    frame(f, 9, 4);
    CHECK(!esl_remote_parse(f, 14, &b, &s));
    frame(f, 9, 1);
    f[8] = 0xBF; /* another sender */
    CHECK(!esl_remote_parse(f, 14, &b, &s));
    frame(f, 9, 1);
    f[0] = 0x61; /* asks for an ACK: not the remote's */
    CHECK(!esl_remote_parse(f, 14, &b, &s));
    frame(f, 9, 1);
    f[9] = 0x18;
    CHECK(!esl_remote_parse(f, 14, &b, &s));
    CHECK_STR(esl_remote_button_name(ESL_REMOTE_LEFT), "left");
    CHECK(esl_remote_button_name(9) == NULL);
}

static int locks;
static void count_lock(esl_remote_t *r, bool take)
{
    (void)r;
    locks += take ? 1 : -1;
    CHECK(locks == 0 || locks == 1);
}

static void test_bursts(void)
{
    esl_remote_t r = {0};
    r.lock = count_lock;
    uint8_t f[14];
    esl_remote_press_t p;
    /* one burst: ten frames, five 10 ms apart, 80 ms, five more */
    unsigned presses = 0;
    for (unsigned i = 0; i < 10; i++) {
        frame(f, 7, ESL_REMOTE_W);
        presses += esl_remote_frame(&r, f, 14, -60, 1000 + i * 10 + (i >= 5 ? 80 : 0), &p);
    }
    CHECK_INT(presses, 1);
    CHECK_INT(r.frames, 10);
    CHECK_INT(r.n, 1);
    CHECK_INT(r.log[0].id, 1);
    CHECK_INT(r.log[0].button, ESL_REMOTE_W);
    CHECK_INT(r.log[0].at_ms, 1000);
    CHECK_INT(locks, 0);
    /* the next press has the next seq, even right after */
    frame(f, 8, ESL_REMOTE_LEFT);
    CHECK(esl_remote_frame(&r, f, 14, -61, 1300, &p));
    CHECK_INT(p.id, 2);
    CHECK_INT(p.button, ESL_REMOTE_LEFT);
    /* the same seq long after is a new press (seq wraps after 256 presses) */
    CHECK(esl_remote_frame(&r, f, 14, -61, 1300 + ESL_REMOTE_SAME_MS, &p));
    CHECK_INT(p.id, 3);
    /* not a remote frame: nothing */
    f[7] = 0;
    CHECK(!esl_remote_frame(&r, f, 14, -61, 9000, &p));
    CHECK_INT(r.frames, 12);
    /* the log keeps the last ESL_REMOTE_LOG presses */
    for (unsigned i = 0; i < 20; i++) {
        frame(f, (uint8_t)(20 + i), ESL_REMOTE_RIGHT);
        CHECK(esl_remote_frame(&r, f, 14, -50, 10000 + i * 100, NULL));
    }
    CHECK_INT(r.n, ESL_REMOTE_LOG);
    CHECK_INT(r.log[ESL_REMOTE_LOG - 1].id, 23);
    CHECK_INT(r.log[0].id, 23 - ESL_REMOTE_LOG + 1);

    char buf[160];
    esl_sb_t out;
    esl_sb_init(&out, buf, sizeof(buf));
    r.boot = 42;
    esl_remote_press_json(&r, &r.log[ESL_REMOTE_LOG - 1], &out);
    CHECK_STR(buf, "{\"event\":\"remote\",\"button\":\"right\",\"id\":23,\"boot\":42,\"rssi\":-50}");
}

static esl_store_t st;
static esl_remote_t remote;
static int sets;
static void set_remote(esl_remote_t *r, bool listen, const char *webhook)
{
    sets++;
    r->listen = listen;
    if (webhook) {
        strcpy(r->webhook, webhook);
        r->webhook_status = 0;
    }
}

static int req(const esl_api_t *api, const char *method, const char *body, esl_sb_t *out)
{
    return esl_api_handle(api, method, "/api/remote", AUTH, body, body ? strlen(body) : 0, 5000, out);
}

static void test_remote_api(void)
{
    static char resp[2048];
    esl_sb_t out;
    esl_sb_init(&out, resp, sizeof(resp));
    esl_store_init(&st, 4, NULL, 0);
    st.channel = 25;
    esl_api_t api = {0};
    api.store = &st;
    api.token = "s3cret";
#define REQ(m, b) req(&api, m, b, &out)
    CHECK_INT(REQ("GET", NULL), 404); /* no remote in this build */
    api.remote = &remote;
    remote.boot = 7;
    CHECK_INT(REQ("GET", NULL), 200);
    CHECK_STR(resp, "{\"channel\":11,\"listen\":false,\"can_listen\":false,\"hearing\":false,\"webhook\":null,"
                    "\"webhook_status\":null,\"boot\":7,\"frames\":0,\"presses\":[]}");
    CHECK_INT(REQ("PUT", "{\"listen\":true}"), 422); /* the radio cannot */
    st.channel = 11;
    CHECK_INT(REQ("GET", NULL), 200);
    CHECK(strstr(resp, "\"hearing\":true"));
    st.channel = 25;
    remote.can_listen = true;
    remote.set = set_remote;
    CHECK_INT(REQ("PUT", "{}"), 422);
    CHECK_INT(REQ("PUT", "{\"listen\":1}"), 422);
    CHECK_INT(REQ("PUT", "{\"webhook\":\"ftp://x\"}"), 422);
    CHECK_INT(sets, 0);
    CHECK_INT(REQ("PUT", "{\"listen\":true,\"webhook\":\"http://192.168.1.5:8123/api/webhook/abc\"}"), 200);
    CHECK_INT(sets, 1);
    CHECK_STR(resp, "{\"channel\":11,\"listen\":true,\"can_listen\":true,\"hearing\":true,"
                    "\"webhook\":\"http://192.168.1.5:8123/api/webhook/abc\",\"webhook_status\":null,\"boot\":7,"
                    "\"frames\":0,\"presses\":[]}");
    /* listen alone keeps the webhook */
    CHECK_INT(REQ("PUT", "{\"listen\":false}"), 200);
    CHECK(strstr(resp, "\"listen\":false") && strstr(resp, "api/webhook/abc"));
    CHECK_INT(REQ("PUT", "{\"webhook\":null}"), 200);
    CHECK(strstr(resp, "\"webhook\":null"));
    /* presses, newest first */
    uint8_t f[14];
    frame(f, 1, ESL_REMOTE_W);
    esl_remote_frame(&remote, f, 14, -70, 1000, NULL);
    frame(f, 2, ESL_REMOTE_LEFT);
    esl_remote_frame(&remote, f, 14, -65, 3000, NULL);
    remote.webhook_status = 200;
    CHECK_INT(REQ("GET", NULL), 200);
    CHECK(strstr(resp, "\"webhook_status\":200,\"boot\":7,\"frames\":2,\"presses\":[{\"id\":2,\"button\":\"left\","
                       "\"s_ago\":2,\"rssi\":-65},{\"id\":1,\"button\":\"w\",\"s_ago\":4,\"rssi\":-70}]}"));
    CHECK_INT(REQ("DELETE", NULL), 405);
#undef REQ
    esl_store_free(&st);
}

void test_remote(void)
{
    test_parse();
    test_bursts();
    test_remote_api();
}
