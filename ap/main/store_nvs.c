#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "esl_persist.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs.h"

/* Content per tag lives in NVS namespace "esl_tags", one blob per table slot
 * (key "t<slot>"), so a tag keeps its slot across reboots (krav A4, G4). A
 * tag's own settings (poll_s etc.) are a second blob in the same slot (key
 * "c<slot>"), and the run-time default interval is "poll_s" (u16) in namespace
 * "cfg", next to ssid/pass/token; without it the menuconfig value is used.
 * Layout files live in namespace "esl_layouts", one blob per model (key
 * "m<model>", ESL_MODEL_* from shared/proto.h). Both use the same mutex.
 *
 * Up to 2 x 8 KB of layouts plus one ~120-byte blob per tag needs more than the
 * default 24 KB NVS partition; see partitions.csv. */

static const char *TAG = "store";
static const char *NS_TAGS = "esl_tags";
static const char *NS_LAYOUTS = "esl_layouts";

static SemaphoreHandle_t mutex;
static esl_store_t *store_ref;
static esl_sched_t *clock_ref;

/* What the AP last heard from each tag ("s<slot>", esl_persist.h) is saved
 * when a tag is new or changes model or firmware, every SEEN_SAVE_S for the
 * tags heard since, and before an OTA restart (store_nvs_flush). So a tag that
 * only polls is not forgotten, and one that has gone quiet still shows when it
 * was last heard. At most one small write per tag per hour. */
#define SEEN_SAVE_S 3600

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

static void cfg_key(unsigned slot, char key[8])
{
    snprintf(key, 8, "c%u", slot);
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

static void persist_cfg(void *ctx, unsigned slot, uint64_t mac, const esl_tag_cfg_t *cfg)
{
    (void)ctx;
    uint8_t blob[ESL_PERSIST_CFG_LEN];
    char key[8];
    cfg_key(slot, key);
    if (cfg)
        blob_write(NS_TAGS, key, blob, esl_persist_cfg_encode(mac, cfg, blob));
    else
        blob_write(NS_TAGS, key, NULL, 0);
}

static void persist_next_poll(void *ctx, const uint16_t *next_poll_s)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open("cfg", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = next_poll_s ? nvs_set_u16(h, "poll_s", *next_poll_s) : nvs_erase_key(h, "poll_s");
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "saving cfg/poll_s failed: %s", esp_err_to_name(err));
}

static void seen_key(unsigned slot, char key[8])
{
    snprintf(key, 8, "s%u", slot);
}

static void persist_seen(void *ctx, unsigned slot, uint64_t mac, const esl_seen_t *rec)
{
    (void)ctx;
    uint8_t blob[ESL_PERSIST_SEEN_LEN];
    char key[8];
    seen_key(slot, key);
    if (rec)
        blob_write(NS_TAGS, key, blob, esl_persist_seen_encode(mac, rec, blob));
    else
        blob_write(NS_TAGS, key, NULL, 0);
}

static uint32_t unix_now(void *ctx)
{
    (void)ctx;
    return clock_ref ? esl_sched_now(clock_ref, (uint64_t)esp_timer_get_time() / 1000, NULL) : 0;
}

static void seen_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SEEN_SAVE_S * 1000));
        unsigned n = esl_store_save_seen(store_ref);
        if (n)
            ESP_LOGI(TAG, "saved %u tags' last poll", n);
    }
}

void store_nvs_set_clock(esl_sched_t *sched)
{
    clock_ref = sched;
}

void store_nvs_flush(void)
{
    if (store_ref) {
        unsigned n = esl_store_save_seen(store_ref);
        ESP_LOGI(TAG, "saved %u tags' last poll before the restart", n);
    }
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
    static const esl_store_ops_t ops = {NULL, lock, unlock, persist_save, persist_erase, persist_cfg,
                                        persist_next_poll, persist_seen, unix_now};
    esl_tag_t *tags = calloc(cap, sizeof(esl_tag_t));
    mutex = xSemaphoreCreateMutex();
    if (!tags || !mutex)
        return ESP_ERR_NO_MEM;
    esl_store_init(st, tags, cap, &ops, next_poll_s);
    store_ref = st;
    if (xTaskCreate(seen_task, "store_seen", 4096, NULL, 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    unsigned loaded = 0, configs = 0, heard = 0;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        uint16_t v;
        if (nvs_get_u16(h, "poll_s", &v) == ESP_OK) {
            esl_store_restore_next_poll(st, v);
            ESP_LOGI(TAG, "default poll interval %u s (from NVS)", (unsigned)v);
        }
        nvs_close(h);
    }
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
        if (nvs_get_blob(h, key, blob, &len) == ESP_OK) {
            if (esl_persist_decode(blob, len, &mac, &c) && esl_store_restore(st, slot, mac, &c))
                loaded++;
            else
                ESP_LOGW(TAG, "ignoring bad stored entry in slot %u", slot);
        }

        esl_tag_cfg_t cfg;
        len = sizeof(blob);
        cfg_key(slot, key);
        if (nvs_get_blob(h, key, blob, &len) == ESP_OK) {
            if (esl_persist_cfg_decode(blob, len, &mac, &cfg) && esl_store_restore_cfg(st, slot, mac, &cfg))
                configs++;
            else
                ESP_LOGW(TAG, "ignoring bad stored settings in slot %u", slot);
        }

        /* after content and settings: a record with another MAC than the slot's
           content is an older tag that slot held, and is skipped */
        esl_seen_t rec;
        len = sizeof(blob);
        seen_key(slot, key);
        if (nvs_get_blob(h, key, blob, &len) == ESP_OK && esl_persist_seen_decode(blob, len, &mac, &rec) &&
            esl_store_restore_seen(st, slot, mac, &rec))
            heard++;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded %u tags with content, %u tag settings and %u tags' last poll from NVS", loaded, configs,
             heard);
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

void cfg_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("cfg", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = val[0] ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "saving cfg/%s failed: %s", key, esp_err_to_name(err));
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
