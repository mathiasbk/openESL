#include "app.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <stdio.h>

/* The store for tag firmware images (components/esl_core/esl_tagfw.h): the
 * "tagfw" data partition, ESL_TAGFW_SLOT bytes per tag model. An AP that was
 * flashed with an older partition table has no such partition until it is
 * flashed over USB once; the API then answers 404 for /api/tag-firmware.
 * Which tags an image is for is kept in NVS ("tagfw", key "to<model>"). */

static const char *TAG = "tagfw";
#define TAGFW_SUBTYPE 0x40

static const esp_partition_t *part;
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

static bool store_erase(void *ctx, uint8_t model)
{
    (void)ctx;
    return esp_partition_erase_range(part, (size_t)(model - 1) * ESL_TAGFW_SLOT, ESL_TAGFW_SLOT) == ESP_OK;
}

static bool store_write(void *ctx, uint8_t model, uint32_t off, const void *data, size_t len)
{
    (void)ctx;
    return off + len <= ESL_TAGFW_SLOT &&
           esp_partition_write(part, (size_t)(model - 1) * ESL_TAGFW_SLOT + off, data, len) == ESP_OK;
}

static bool store_read(void *ctx, uint8_t model, uint32_t off, void *data, size_t len)
{
    (void)ctx;
    return off + len <= ESL_TAGFW_SLOT &&
           esp_partition_read(part, (size_t)(model - 1) * ESL_TAGFW_SLOT + off, data, len) == ESP_OK;
}

static void save_to(void *ctx, uint8_t model, const void *to, size_t len)
{
    (void)ctx;
    char key[8];
    snprintf(key, sizeof(key), "to%u", (unsigned)model);
    nvs_handle_t h;
    esp_err_t err = nvs_open("tagfw", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = to ? nvs_set_blob(h, key, to, len) : nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "could not keep which tags get the %s firmware: %s", esl_model_name(model), esp_err_to_name(err));
}

static bool load_to(void *ctx, uint8_t model, void *to, size_t len)
{
    (void)ctx;
    char key[8];
    snprintf(key, sizeof(key), "to%u", (unsigned)model);
    nvs_handle_t h;
    if (nvs_open("tagfw", NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t n = len;
    bool ok = nvs_get_blob(h, key, to, &n) == ESP_OK && n == len;
    nvs_close(h);
    return ok;
}

esl_tagfw_t *tagfw_start(void)
{
    static const esl_tagfw_ops_t ops = {NULL, lock, unlock, store_erase, store_write, store_read, save_to, load_to};
    static esl_tagfw_t fw;

    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, TAGFW_SUBTYPE, "tagfw");
    if (!part || part->size < (ESL_MODEL_COUNT - 1) * ESL_TAGFW_SLOT) {
        ESP_LOGW(TAG, "no tagfw partition (older partition table): no firmware updates for the tags");
        return NULL;
    }
    mutex = xSemaphoreCreateMutex();
    if (!mutex)
        return NULL;
    esl_tagfw_init(&fw, &ops);
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
        esl_tagfw_img_t img;
        if (esl_tagfw_get(&fw, m, &img))
            ESP_LOGI(TAG, "%s: firmware %u.%u.%u (%u bytes) for the tags", esl_model_name(m), img.ver / 10000u,
                     img.ver / 100u % 100u, img.ver % 100u, (unsigned)img.size);
    }
    return &fw;
}
