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
 * anything to report (the previous state is "no problems"). */

static const char *TAG = "health";

static const esl_api_t *api;
static uint8_t *prev;

static void persist_webhook(void *ctx, const char *url)
{
    (void)ctx;
    cfg_set_str("webhook", url);
}

static esl_notify_t notify = {"", NULL, persist_webhook};

static void post(const char *url, const char *body, size_t len)
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
    esp_http_client_set_post_field(c, body, (int)len);
    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "webhook: POST %s -> %d (%u bytes)", url, esp_http_client_get_status_code(c), (unsigned)len);
    else
        ESP_LOGW(TAG, "webhook: POST %s failed: %s", url, esp_err_to_name(err));
    esp_http_client_cleanup(c);
}

static void health_task(void *arg)
{
    (void)arg;
    size_t cap = ESL_API_PAGE_SIZE(0) + 160 * api->store->cap; /* one row per tag with a problem */
    char *buf = malloc(cap);
    uint8_t *next = malloc(api->store->cap);
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
        memcpy(next, prev, api->store->cap);
        esl_sb_t out;
        esl_sb_init(&out, buf, cap);
        uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
        int status = api_handle_health(api, now_ms, next, &out);
        if (status != 200 || out.overflow) {
            ESP_LOGW(TAG, "report not built (%d)", status);
            continue;
        }
        if (memcmp(next, prev, api->store->cap) == 0)
            continue;
        char url[ESL_WEBHOOK_URL_MAX + 1];
        strcpy(url, notify.url);
        post(url, out.buf, out.len);
        memcpy(prev, next, api->store->cap);
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
    prev = calloc(api->store->cap, 1);
    if (!prev || xTaskCreate(health_task, "health", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "not started");
        return NULL;
    }
    if (notify.url[0])
        ESP_LOGI(TAG, "webhook %s, checking every %d s", notify.url, CONFIG_ESL_HEALTH_PERIOD_S);
    return &notify;
}
