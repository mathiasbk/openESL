#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* The webhook: every CONFIG_ESL_HEALTH_PERIOD_S the health report of every
 * tag (esl_api_health, the same as GET /api/health) is computed against the
 * last one, and when any tag's problems changed it is POSTed to the URL in
 * api->notify->url (PUT /api/config {"webhook": ...}, kept in NVS cfg/webhook).
 * A POST that fails is logged and tried again with the next change; the
 * report is not queued. The first report after boot goes out when there is
 * anything to report (the previous state is "no problems"). A radio coprocessor
 * that is lost or back (esl_api_radios_lost: the S3 has no contact with its C6)
 * sends the report too. */

static const char *TAG = "health";

static const esl_api_t *api;
static uint8_t *prev;
static uint32_t prev_lost; /* esl_api_radios_lost() of the last report */

static void persist_webhook(void *ctx, const char *url)
{
    (void)ctx;
    cfg_set_str("webhook", url);
}

static esl_notify_t notify = {"", NULL, persist_webhook};

/* The report is streamed (it grows with the tag table): first built into a sink
 * that throws it away, to see whether anything changed, then built again into the
 * POST as HTTP chunks. Only BUF bytes are held at a time. */
#define BUF 2048

static bool discard(esl_sb_t *sb)
{
    (void)sb;
    return true;
}

static bool chunk(esl_sb_t *sb)
{
    esp_http_client_handle_t c = sb->ctx;
    char head[12];
    if (!sb->len)
        return true; /* an empty chunk would end the body */
    int n = snprintf(head, sizeof(head), "%x\r\n", (unsigned)sb->len);
    return esp_http_client_write(c, head, n) == n && esp_http_client_write(c, sb->buf, (int)sb->len) == (int)sb->len &&
           esp_http_client_write(c, "\r\n", 2) == 2;
}

/* POSTs the report against flags (updated to the tags' new flags). Built
   without the API's mutex (esl_api_health reads only the tag table, which has
   its own lock, taken per tag), so a slow webhook holds up nothing else. */
static void post(const char *url, char *buf, uint8_t *flags)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "webhook: bad URL %s", url);
        return;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(c, -1); /* -1: chunked */
    if (err == ESP_OK) {
        esl_sb_t out;
        esl_sb_init(&out, buf, BUF);
        esl_sb_set_sink(&out, chunk, c);
        int status = esl_api_health(api, (uint64_t)esp_timer_get_time() / 1000, NULL, flags, &out);
        esl_sb_flush(&out);
        if (status != 200 || out.overflow || esp_http_client_write(c, "0\r\n\r\n", 5) != 5)
            err = ESP_FAIL;
        else if (esp_http_client_fetch_headers(c) < 0)
            err = ESP_ERR_HTTP_FETCH_HEADER;
        if (err == ESP_OK)
            ESP_LOGI(TAG, "webhook: POST %s -> %d (%u bytes)", url, esp_http_client_get_status_code(c),
                     (unsigned)out.sent);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "webhook: POST %s failed: %s", url, esp_err_to_name(err));
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

static void health_task(void *arg)
{
    (void)arg;
    unsigned max = api->store->max; /* slot numbers are below it */
    char *buf = malloc(BUF);
    uint8_t *next = malloc(max);
    if (!buf || !next) {
        ESP_LOGE(TAG, "out of memory");
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ESL_HEALTH_PERIOD_S * 1000));
        if (!notify.url[0])
            continue;
        /* Compare against a copy, so a report that cannot be built or sent
           is tried again on the next round */
        memcpy(next, prev, max);
        esl_sb_t out;
        esl_sb_init(&out, buf, BUF);
        esl_sb_set_sink(&out, discard, NULL);
        uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
        int status = esl_api_health(api, now_ms, NULL, next, &out);
        if (status != 200 || out.overflow) {
            ESP_LOGW(TAG, "report not built (%d)", status);
            continue;
        }
        uint32_t lost = esl_api_radios_lost(api, now_ms);
        if (memcmp(next, prev, max) == 0 && lost == prev_lost)
            continue;
        char url[ESL_WEBHOOK_URL_MAX + 1];
        strcpy(url, notify.url);
        memcpy(next, prev, max); /* built again, against the same last report */
        post(url, buf, next);
        memcpy(prev, next, max);
        prev_lost = lost;
    }
}

esl_notify_t *health_start(const esl_api_t *a)
{
    api = a;
    cfg_get_str("webhook", "", notify.url, sizeof(notify.url));
    if (notify.url[0] && !esl_webhook_url_ok(notify.url, strlen(notify.url))) {
        ESP_LOGW(TAG, "ignoring bad webhook URL in NVS");
        notify.url[0] = '\0';
    }
    prev = calloc(api->store->max, 1);
    if (!prev || xTaskCreate(health_task, "health", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "not started");
        return NULL;
    }
    if (notify.url[0])
        ESP_LOGI(TAG, "webhook %s, checking every %d s", notify.url, CONFIG_ESL_HEALTH_PERIOD_S);
    return &notify;
}
