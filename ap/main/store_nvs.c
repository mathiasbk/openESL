#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "esl_persist.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

/* Content per tag lives in NVS namespace "esl_tags", one blob per table slot
 * (key "t<slot>"), so a tag keeps its slot across reboots (krav A4, G4).
 * Layout files live in namespace "esl_layouts", one blob per model (key
 * "m<model>", ESL_MODEL_* from shared/proto.h). Both use the same mutex.
 *
 * Up to 2 x 8 KB of layouts plus one ~120-byte blob per tag needs more than the
 * default 24 KB NVS partition; see partitions.csv. */

static const char *TAG = "store";
static const char *NS_TAGS = "esl_tags";
static const char *NS_LAYOUTS = "esl_layouts";

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

static void blob_write(const char *ns, const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = data ? nvs_set_blob(h, key, data, len) : nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "%s %s/%s failed: %s", data ? "saving" : "erasing", ns, key, esp_err_to_name(err));
}

static void persist_save(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    (void)ctx;
    uint8_t blob[ESL_PERSIST_BLOB_MAX];
    size_t n = esl_persist_encode(mac, c, blob);
    char key[8];
    slot_key(slot, key);
    blob_write(NS_TAGS, key, blob, n);
}

static void persist_erase(void *ctx, unsigned slot)
{
    (void)ctx;
    char key[8];
    slot_key(slot, key);
    blob_write(NS_TAGS, key, NULL, 0);
}

static void layout_key(uint8_t model, char key[8])
{
    snprintf(key, 8, "m%u", (unsigned)model);
}

static void layout_save(void *ctx, uint8_t model, const char *data, size_t len)
{
    (void)ctx;
    char key[8];
    layout_key(model, key);
    blob_write(NS_LAYOUTS, key, data, len);
}

static void layout_erase(void *ctx, uint8_t model)
{
    (void)ctx;
    char key[8];
    layout_key(model, key);
    blob_write(NS_LAYOUTS, key, NULL, 0);
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

esp_err_t layouts_nvs_init(esl_layouts_t *ls)
{
    static const esl_layout_ops_t ops = {NULL, lock, unlock, layout_save, layout_erase};
    if (!mutex)
        return ESP_ERR_INVALID_STATE; /* store_nvs_init() creates it */
    esl_layouts_init(ls, &ops);

    nvs_handle_t h;
    if (nvs_open(NS_LAYOUTS, NVS_READONLY, &h) != ESP_OK)
        return ESP_OK;
    char *buf = malloc(ESL_LAYOUT_MAX_SIZE);
    if (!buf) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t model = 1; model < ESL_MODEL_COUNT; model++) {
        char key[8];
        size_t len = ESL_LAYOUT_MAX_SIZE;
        layout_key(model, key);
        if (nvs_get_blob(h, key, buf, &len) != ESP_OK)
            continue;
        if (esl_layouts_restore(ls, model, buf, len))
            ESP_LOGI(TAG, "layout for %s loaded (%u bytes)", esl_model_name(model), (unsigned)len);
        else
            ESP_LOGW(TAG, "ignoring bad stored layout for %s", esl_model_name(model));
    }
    free(buf);
    nvs_close(h);
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
