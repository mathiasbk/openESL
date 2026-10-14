#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "esl_persist.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

/* Content per tag lives in NVS namespace "esl_tags", one blob per table slot
 * (key "t<slot>"), so a tag keeps its slot across reboots (krav A4, G4). */

static const char *TAG = "store";
static const char *NS_TAGS = "esl_tags";

static SemaphoreHandle_t mutex;

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

static void slot_key(unsigned slot, char key[8])
{
    snprintf(key, 8, "t%u", slot);
}

static void persist_save(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    (void)ctx;
    uint8_t blob[ESL_PERSIST_BLOB_MAX];
    size_t n = esl_persist_encode(mac, c, blob);
    char key[8];
    nvs_handle_t h;
    slot_key(slot, key);

    esp_err_t err = nvs_open(NS_TAGS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, blob, n);
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "saving slot %u failed: %s", slot, esp_err_to_name(err));
}

static void persist_erase(void *ctx, unsigned slot)
{
    (void)ctx;
    char key[8];
    nvs_handle_t h;
    slot_key(slot, key);

    esp_err_t err = nvs_open(NS_TAGS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "erasing slot %u failed: %s", slot, esp_err_to_name(err));
}

esp_err_t store_nvs_init(esl_store_t *st, unsigned cap, uint16_t next_poll_s)
{
    static const esl_store_ops_t ops = {NULL, lock, unlock, persist_save, persist_erase};
    esl_tag_t *tags = calloc(cap, sizeof(esl_tag_t));
    mutex = xSemaphoreCreateMutex();
    if (!tags || !mutex)
        return ESP_ERR_NO_MEM;
    esl_store_init(st, tags, cap, &ops, next_poll_s);

    nvs_handle_t h;
    unsigned loaded = 0;
    if (nvs_open(NS_TAGS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored tags yet");
        return ESP_OK;
    }
    for (unsigned slot = 0; slot < cap; slot++) {
        uint8_t blob[ESL_PERSIST_BLOB_MAX];
        size_t len = sizeof(blob);
        char key[8];
        uint64_t mac;
        esl_content_t c;
        slot_key(slot, key);
        if (nvs_get_blob(h, key, blob, &len) != ESP_OK)
            continue;
        if (!esl_persist_decode(blob, len, &mac, &c) || !esl_store_restore(st, slot, mac, &c)) {
            ESP_LOGW(TAG, "ignoring bad stored entry in slot %u", slot);
            continue;
        }
        loaded++;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded %u tags from NVS", loaded);
    return ESP_OK;
}

void cfg_get_str(const char *key, const char *def, char *out, size_t out_len)
{
    nvs_handle_t h;
    snprintf(out, out_len, "%s", def);
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK)
        return;
    size_t len = out_len;
    if (nvs_get_str(h, key, out, &len) != ESP_OK)
        snprintf(out, out_len, "%s", def);
    nvs_close(h);
}
