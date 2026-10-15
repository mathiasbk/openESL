#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "app.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

/* Scheduled content (esl_sched.h) on the device: entries in NVS namespace
 * "esl_sched", one blob per slot (key "s<slot>"), a task that applies the due
 * ones every second, and the clock from NTP (CONFIG_ESL_NTP_SERVER) once the AP
 * is on WiFi. Without WiFi the time comes from PUT /api/time (esl-client sets
 * it). The schedule has its own mutex: esl_sched_tick() holds it while it sets
 * content in the store, which takes the store's. */

#ifndef CONFIG_ESL_NTP_SERVER
#define CONFIG_ESL_NTP_SERVER ""
#endif

static const char *TAG = "sched";
static const char *NS = "esl_sched";

static SemaphoreHandle_t mutex;
static esl_sched_t sched;

static void lock(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(mutex, portMAX_DELAY);
}

static void unlock(void *ctx)
{
    (void)ctx;
    xSemaphoreGive(mutex);
}

static void key_of(unsigned slot, char key[8])
{
    snprintf(key, 8, "s%u", slot);
}

static void save(void *ctx, unsigned slot, const uint8_t *blob, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    char key[8];
    key_of(slot, key);
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, blob, len);
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
}

static bool load(void *ctx, unsigned slot, uint8_t *blob, size_t *len)
{
    (void)ctx;
    nvs_handle_t h;
    char key[8];
    key_of(slot, key);
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    bool ok = nvs_get_blob(h, key, blob, len) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void erase(void *ctx, unsigned slot)
{
    (void)ctx;
    nvs_handle_t h;
    char key[8];
    key_of(slot, key);
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    if (nvs_erase_key(h, key) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
}

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

static void on_ntp(struct timeval *tv)
{
    bool first = esl_sched_now(&sched, now_ms(), NULL) == 0;
    esl_sched_set_time(&sched, (uint32_t)tv->tv_sec, now_ms(), ESL_TIME_NTP);
    if (first)
        ESP_LOGI(TAG, "time from NTP: %lld", (long long)tv->tv_sec);
}

static void tick_task(void *arg)
{
    esl_store_t *st = arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        unsigned n = esl_sched_tick(&sched, st, now_ms());
        if (n)
            ESP_LOGI(TAG, "scheduled content applied to %u tag(s)", n);
    }
}

esl_sched_t *sched_start(esl_store_t *st, unsigned cap)
{
    static const esl_sched_ops_t ops = {NULL, lock, unlock, save, load, erase};
    esl_sched_entry_t *e = calloc(cap, sizeof(*e));
    mutex = xSemaphoreCreateMutex();
    if (!e || !mutex) {
        ESP_LOGE(TAG, "out of memory: no scheduled content");
        return NULL;
    }
    esl_sched_init(&sched, e, cap, &ops);

    nvs_handle_t h;
    unsigned loaded = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t blob[ESL_SCHED_BLOB_MAX];
        for (unsigned slot = 0; slot < cap; slot++) {
            size_t len = sizeof(blob);
            char key[8];
            key_of(slot, key);
            if (nvs_get_blob(h, key, blob, &len) != ESP_OK)
                continue;
            if (esl_sched_restore(&sched, slot, blob, len))
                loaded++;
            else
                ESP_LOGW(TAG, "ignoring bad scheduled entry in slot %u", slot);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "loaded %u scheduled entries from NVS (room for %u)", loaded, cap);

    if (CONFIG_ESL_NTP_SERVER[0]) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_ESL_NTP_SERVER);
        cfg.sync_cb = on_ntp;
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "NTP: %s; set the time with PUT /api/time", esp_err_to_name(err));
    }
    if (xTaskCreate(tick_task, "sched", 6144, st, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no task: no scheduled content");
        return NULL;
    }
    return &sched;
}
