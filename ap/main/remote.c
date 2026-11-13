#include <stdio.h>
#include <string.h>
#include "app.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* A SoluM remote as a wireless button (esl_remote.h, docs/solum-remote.md): the settings
 * in NVS (cfg/remote_listen, cfg/remote_hook), and a task that POSTs each press to the
 * remote's webhook. The radio task hands it the frames (remote_frame) and asks whether to
 * visit channel 11 (remote_listening). A press that cannot be posted is logged and dropped:
 * a button pressed minutes ago should not switch on a light now. */

static const char *TAG = "remote";

static esl_remote_t remote;
static SemaphoreHandle_t mtx;
static QueueHandle_t presses;

static void lock(esl_remote_t *r, bool take)
{
    (void)r;
    if (take)
        xSemaphoreTake(mtx, portMAX_DELAY);
    else
        xSemaphoreGive(mtx);
}

static void set(esl_remote_t *r, bool listen, const char *webhook)
{
    lock(r, true);
    r->listen = listen;
    if (webhook) {
        snprintf(r->webhook, sizeof(r->webhook), "%s", webhook);
        r->webhook_status = 0;
    }
    lock(r, false);
    cfg_set_str("remote_listen", listen ? "1" : "");
    if (webhook)
        cfg_set_str("remote_hook", webhook);
    ESP_LOGI(TAG, "listen %s, webhook %s", listen ? "on" : "off", webhook ? (webhook[0] ? webhook : "off") : "unchanged");
}

static int post(const char *url, const char *body)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c)
        return -1;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, (int)strlen(body));
    esp_err_t err = esp_http_client_perform(c);
    int code = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "webhook: POST %s failed: %s", url, esp_err_to_name(err));
    return code;
}

static void remote_task(void *arg)
{
    (void)arg;
    esl_remote_press_t p;
    char body[160], url[ESL_WEBHOOK_URL_MAX + 1];
    for (;;) {
        if (xQueueReceive(presses, &p, portMAX_DELAY) != pdTRUE)
            continue;
        esl_sb_t out;
        esl_sb_init(&out, body, sizeof(body));
        lock(&remote, true);
        snprintf(url, sizeof(url), "%s", remote.webhook);
        esl_remote_press_json(&remote, &p, &out);
        lock(&remote, false);
        ESP_LOGI(TAG, "press %lu: %s (rssi %d)", (unsigned long)p.id, esl_remote_button_name(p.button), p.rssi);
        if (!url[0])
            continue;
        int code = post(url, body);
        if (code >= 0)
            ESP_LOGI(TAG, "webhook: POST %s -> %d", url, code);
        lock(&remote, true);
        if (!strcmp(url, remote.webhook))
            remote.webhook_status = code;
        lock(&remote, false);
    }
}

esl_remote_t *remote_start(bool can_listen)
{
    char v[4];
    mtx = xSemaphoreCreateMutex();
    presses = xQueueCreate(4, sizeof(esl_remote_press_t));
    if (!mtx || !presses || xTaskCreate(remote_task, "remote", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "not started");
        return NULL;
    }
    remote.lock = lock;
    remote.set = set;
    remote.can_listen = can_listen;
    remote.boot = esp_random();
    cfg_get_str("remote_listen", "", v, sizeof(v));
    remote.listen = v[0] == '1';
    cfg_get_str("remote_hook", "", remote.webhook, sizeof(remote.webhook));
    if (remote.webhook[0] && !esl_webhook_url_ok(remote.webhook, strlen(remote.webhook))) {
        ESP_LOGW(TAG, "ignoring bad webhook URL in NVS");
        remote.webhook[0] = '\0';
    }
    if (remote.listen || remote.webhook[0])
        ESP_LOGI(TAG, "listen %s, webhook %s", remote.listen ? "on" : "off", remote.webhook[0] ? remote.webhook : "off");
    return &remote;
}

void remote_frame(const esl_link_rx_t *f)
{
    esl_remote_press_t p;
    if (presses && esl_remote_frame(&remote, f->psdu, f->len, f->rssi, (uint64_t)esp_timer_get_time() / 1000, &p) &&
        xQueueSend(presses, &p, 0) != pdTRUE)
        ESP_LOGW(TAG, "press %lu not posted: the webhook is behind", (unsigned long)p.id);
}

bool remote_listening(void)
{
    return presses && remote.listen && remote.can_listen; /* a bool: read without the lock */
}
