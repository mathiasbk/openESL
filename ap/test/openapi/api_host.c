/* The AP's HTTP API on the host, for check_openapi.py: reads commands on stdin and
 * runs them through the same esl_api_handle() the AP uses.
 *
 *   REQ <method> <uri> <auth|-> <len>\n<len bytes body>   ->  <status> <len>\n<body>\n
 *   POLL <mac> <model> <layout_ver|-> <shown_content_id>  ->  OK\n
 *   TIME <ms since boot>                                  ->  OK\n
 *
 * <auth> is the whole Authorization value with spaces as '+', '-' for none. POLL makes
 * the tag poll as firmware with settings and (unless '-') layout files would. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esl_api.h"

#define CAP 8

static esl_tag_t tags[CAP];
static esl_store_t st;
static esl_layouts_t layouts;
static esl_notify_t notify;

/* A WiFi side that only remembers what it was given. */
static esl_wifi_status_t wifi = {.mode = ESL_WIFI_SETUP, .setup_ssid = "openesl-1A2B", .setup_ip = "192.168.4.1",
                                 .scan_count = 1, .scan = {"Butikken"}, .scan_rssi = {-58}};

static void wifi_status(void *ctx, esl_wifi_status_t *out)
{
    (void)ctx;
    *out = wifi;
}

static int wifi_set(void *ctx, const char *ssid, const char *pass)
{
    (void)ctx;
    (void)pass;
    strcpy(wifi.ssid, ssid);
    wifi.mode = ssid[0] ? ESL_WIFI_STA_SETUP : ESL_WIFI_SETUP;
    return 0;
}

static const esl_wifi_ops_t wifi_ops = {NULL, wifi_status, wifi_set, NULL, NULL};

/* Scheduled content kept in RAM */
static esl_sched_entry_t sched_e[CAP];
static esl_sched_t sched;
static uint8_t blobs[CAP][ESL_SCHED_BLOB_MAX];
static size_t blob_len[CAP];

static void blob_save(void *ctx, unsigned slot, const uint8_t *b, size_t len)
{
    (void)ctx;
    memcpy(blobs[slot], b, len);
    blob_len[slot] = len;
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
}

/* Tag firmware store in RAM */
static uint8_t fw_mem[ESL_MODEL_COUNT - 1][ESL_TAGFW_SLOT];
static esl_tagfw_t tagfw;

static bool fw_erase(void *ctx, uint8_t model)
{
    (void)ctx;
    memset(fw_mem[model - 1], 0xFF, ESL_TAGFW_SLOT);
    return true;
}

static bool fw_write(void *ctx, uint8_t model, uint32_t off, const void *d, size_t n)
{
    (void)ctx;
    if (off + n > ESL_TAGFW_SLOT)
        return false;
    memcpy(fw_mem[model - 1] + off, d, n);
    return true;
}

static bool fw_read(void *ctx, uint8_t model, uint32_t off, void *d, size_t n)
{
    (void)ctx;
    if (off + n > ESL_TAGFW_SLOT)
        return false;
    memcpy(d, fw_mem[model - 1] + off, n);
    return true;
}

static const esl_tagfw_ops_t fw_ops = {NULL, NULL, NULL, fw_erase, fw_write, fw_read};

static const esl_sched_ops_t sched_ops = {NULL, NULL, NULL, blob_save, blob_load, blob_erase};

int main(void)
{
    static char resp[ESL_API_RESP_SIZE(CAP)];
    static char body[ESL_API_MAX_BODY + 64];
    esl_api_t api = {&st, "s3cret", "0.1.0", 25, &layouts, "poweron", &wifi_ops, &sched, &notify, NULL, &tagfw};
    uint64_t now_ms = 1000;
    char line[512];

    esl_store_init(&st, tags, CAP, NULL, 0);
    esl_layouts_init(&layouts, NULL);
    memset(fw_mem, 0xFF, sizeof(fw_mem));
    esl_tagfw_init(&tagfw, &fw_ops);
    esl_sched_init(&sched, sched_e, CAP, &sched_ops);
    /* 2026-10-15 12:00 UTC, so the examples with "at" (later times) are scheduled */
    esl_sched_set_time(&sched, 1792065600u, now_ms, ESL_TIME_NTP);

    while (fgets(line, sizeof(line), stdin)) {
        char method[16], uri[256], auth[128], mac_s[32], ver_s[16];
        unsigned long len;
        unsigned model;
        uint32_t shown;
        unsigned long long t;

        if (sscanf(line, "REQ %15s %255s %127s %lu", method, uri, auth, &len) == 4) {
            if (len > sizeof(body) || fread(body, 1, len, stdin) != len)
                return 1;
            for (char *p = auth; *p; p++)
                if (*p == '+')
                    *p = ' ';
            esl_sb_t out;
            esl_sb_init(&out, resp, sizeof(resp));
            int status = esl_api_handle(&api, method, uri, strcmp(auth, "-") ? auth : NULL, len ? body : NULL,
                                        len, now_ms, &out);
            printf("%d %u\n", status, (unsigned)out.len);
            fwrite(out.buf, 1, out.len, stdout);
            putchar('\n');
        } else if (sscanf(line, "POLL %31s %u %15s %" SCNu32, mac_s, &model, ver_s, &shown) == 4) {
            uint64_t mac;
            if (!esl_mac_parse(mac_s, strlen(mac_s), &mac))
                return 1;
            uint8_t reply[ESL_MAX_PAYLOAD];
            esl_layout_env_t env;
            esl_poll_t p = {2, 2950, shown, 0, (uint8_t)model, true, 0, strcmp(ver_s, "-") != 0,
                            (uint32_t)strtoul(ver_s, NULL, 10)};
            esl_layouts_env(&layouts, (uint8_t)model, &env);
            esl_store_on_poll(&st, mac, &p, -61, now_ms, &env, reply);
            puts("OK");
        } else if (sscanf(line, "TIME %llu", &t) == 1) {
            now_ms = t;
            puts("OK");
        } else {
            return 1;
        }
        fflush(stdout);
    }
    return 0;
}
