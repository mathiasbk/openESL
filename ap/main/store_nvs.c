#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "esl_persist.h"
#include "esp_heap_caps.h"
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
 * The content is not kept in RAM: the store reads the blob back when it draws a
 * tag's image or the API shows it (load_content). A picture a client sent (PUT
 * /api/tags/{mac}/image) is a third blob, "i<slot>", up to about 5 KB (less when it
 * run-length codes well), read back when a tag needs it.
 *
 * One layout file of up to 8 KB per tag model plus a few small blobs per tag
 * needs more than the default 24 KB NVS partition; see boards/<board>/partitions.csv. */

static const char *TAG = "store";
static const char *NS_TAGS = "esl_tags";
static const char *NS_LAYOUTS = "esl_layouts";

static SemaphoreHandle_t mutex;
static SemaphoreHandle_t save_mutex; /* esl_store_ops_t save_lock: content and settings writes in order */
static esl_store_t *store_ref;
static esl_sched_t *clock_ref;

/* What the AP last heard from each tag ("s<slot>", esl_persist.h) is saved
 * when a tag is new or changes model or firmware, every SEEN_SAVE_S for the
 * tags heard since, and before an OTA restart (store_nvs_flush). So a tag that
 * only polls is not forgotten, and one that has gone quiet still shows when it
 * was last heard. At most one small write per tag per hour. */
#define SEEN_SAVE_S 3600

/* The tag table grows while this much internal RAM stays free after it, for
 * WiFi, HTTP and the image buffers (TABLE_RESERVE now, TABLE_LOW_MARK at the
 * lowest it has been since boot, which counts the peaks of HTTP requests).
 * With PSRAM the table goes there instead. At boot (restoring) there is no
 * limit: the stored tags all had a slot before the restart, and WiFi has not
 * started yet. */
#define TABLE_RESERVE  (40 * 1024)
#define TABLE_LOW_MARK (16 * 1024)
static bool restoring;
static bool no_room; /* logged once until the table grows again */

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

static void save_lock(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(save_mutex, portMAX_DELAY);
}

static void save_unlock(void *ctx)
{
    (void)ctx;
    xSemaphoreGive(save_mutex);
}

static void slot_key(unsigned slot, char key[8])
{
    snprintf(key, 8, "t%u", slot);
}

static void cfg_key(unsigned slot, char key[8])
{
    snprintf(key, 8, "c%u", slot);
}

static bool blob_write(const char *ns, const char *key, const void *data, size_t len)
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
    return err == ESP_OK;
}

static bool persist_save(void *ctx, unsigned slot, uint64_t mac, const esl_content_t *c)
{
    (void)ctx;
    uint8_t blob[ESL_PERSIST_BLOB_MAX];
    size_t n = esl_persist_encode(mac, c, blob);
    char key[8];
    slot_key(slot, key);
    return blob_write(NS_TAGS, key, blob, n);
}

static bool load_content(void *ctx, unsigned slot, uint64_t mac, esl_content_t *c)
{
    (void)ctx;
    uint8_t blob[ESL_PERSIST_BLOB_MAX];
    size_t len = sizeof(blob);
    char key[8];
    uint64_t m;
    nvs_handle_t h;
    slot_key(slot, key);
    if (nvs_open(NS_TAGS, NVS_READONLY, &h) != ESP_OK)
        return false;
    bool ok = nvs_get_blob(h, key, blob, &len) == ESP_OK && esl_persist_decode(blob, len, &m, c) && m == mac;
    nvs_close(h);
    return ok;
}

static void *table_alloc(void *ctx, size_t n)
{
    (void)ctx;
#if CONFIG_SPIRAM
    void *ps = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    if (ps)
        return ps;
#endif
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t free_now = heap_caps_get_free_size(caps), low = heap_caps_get_minimum_free_size(caps);
    void *p = NULL;
    if (restoring || (free_now >= n + TABLE_RESERVE && low >= n + TABLE_LOW_MARK))
        p = heap_caps_calloc(1, n, caps);
    if (!p && !no_room)
        ESP_LOGW(TAG, "no room for more tags (free heap %u bytes, lowest %u): a new tag is not added",
                 (unsigned)free_now, (unsigned)low);
    no_room = !p;
    return p;
}

static void persist_erase(void *ctx, unsigned slot)
{
    (void)ctx;
    char key[8];
    slot_key(slot, key);
    blob_write(NS_TAGS, key, NULL, 0);
}

static void image_key(unsigned slot, char key[8])
{
    snprintf(key, 8, "i%u", slot);
}

static bool persist_image(void *ctx, unsigned slot, const uint8_t *data, size_t len)
{
    (void)ctx;
    char key[8];
    image_key(slot, key);
    return blob_write(NS_TAGS, key, data, len);
}

static size_t load_image(void *ctx, unsigned slot, uint8_t *buf, size_t cap)
{
    (void)ctx;
    char key[8];
    nvs_handle_t h;
    size_t len = 0;
    image_key(slot, key);
    if (nvs_open(NS_TAGS, NVS_READONLY, &h) != ESP_OK)
        return 0;
    if (nvs_get_blob(h, key, NULL, &len) != ESP_OK || len > cap || nvs_get_blob(h, key, buf, &len) != ESP_OK)
        len = 0;
    nvs_close(h);
    return len;
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

/* Calls fn for every blob in namespace esl_tags whose key is prefix + slot number. */
static void each_slot(nvs_handle_t h, char prefix, void (*fn)(nvs_handle_t h, const char *key, unsigned slot, void *arg),
                      void *arg)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NS_TAGS, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        char *end;
        unsigned long slot = info.key[0] == prefix ? strtoul(info.key + 1, &end, 10) : 0;
        if (info.key[0] == prefix && end != info.key + 1 && !*end && slot < ESL_STORE_MAX_TAGS)
            fn(h, info.key, (unsigned)slot, arg);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
}

typedef struct {
    esl_store_t *st;
    unsigned ok;
} restore_t;

static void restore_content(nvs_handle_t h, const char *key, unsigned slot, void *arg)
{
    restore_t *r = arg;
    uint8_t blob[ESL_PERSIST_BLOB_MAX];
    size_t len = sizeof(blob);
    uint64_t mac;
    esl_content_t c;
    if (nvs_get_blob(h, key, blob, &len) == ESP_OK && esl_persist_decode(blob, len, &mac, &c) &&
        esl_store_restore(r->st, slot, mac, &c))
        r->ok++;
    else
        ESP_LOGW(TAG, "ignoring stored entry in slot %u (bad, or no room)", slot);
}

static void restore_cfg(nvs_handle_t h, const char *key, unsigned slot, void *arg)
{
    restore_t *r = arg;
    uint8_t blob[ESL_PERSIST_CFG_LEN];
    size_t len = sizeof(blob);
    uint64_t mac;
    esl_tag_cfg_t cfg;
    if (nvs_get_blob(h, key, blob, &len) == ESP_OK && esl_persist_cfg_decode(blob, len, &mac, &cfg) &&
        esl_store_restore_cfg(r->st, slot, mac, &cfg))
        r->ok++;
    else
        ESP_LOGW(TAG, "ignoring stored settings in slot %u (bad, or no room)", slot);
}

/* A record with another MAC than the slot's content is an older tag that slot
   held, and is skipped */
static void restore_seen(nvs_handle_t h, const char *key, unsigned slot, void *arg)
{
    restore_t *r = arg;
    uint8_t blob[ESL_PERSIST_SEEN_LEN];
    size_t len = sizeof(blob);
    uint64_t mac;
    esl_seen_t rec;
    if (nvs_get_blob(h, key, blob, &len) == ESP_OK && esl_persist_seen_decode(blob, len, &mac, &rec) &&
        esl_store_restore_seen(r->st, slot, mac, &rec))
        r->ok++;
}

esp_err_t store_nvs_init(esl_store_t *st, unsigned max, uint16_t next_poll_s)
{
    static const esl_store_ops_t ops = {NULL, lock, unlock, persist_save, persist_erase, persist_cfg,
                                        persist_next_poll, persist_seen, unix_now, load_content, table_alloc,
                                        save_lock, save_unlock, persist_image, load_image};
    mutex = xSemaphoreCreateMutex();
    save_mutex = xSemaphoreCreateMutex();
    if (!mutex || !save_mutex)
        return ESP_ERR_NO_MEM;
    esl_store_init(st, max, &ops, next_poll_s);
    store_ref = st;
    if (xTaskCreate(seen_task, "store_seen", 4096, NULL, 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;

    nvs_handle_t h;
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
    /* Only the keys that are there (the table can be large): content, then
       settings, then what was last heard */
    restore_t loaded = {st, 0}, configs = {st, 0}, heard = {st, 0};
    restoring = true;
    each_slot(h, 't', restore_content, &loaded);
    each_slot(h, 'c', restore_cfg, &configs);
    each_slot(h, 's', restore_seen, &heard);
    restoring = false;
    nvs_close(h);
    ESP_LOGI(TAG, "loaded %u tags with content, %u tag settings and %u tags' last poll from NVS", loaded.ok,
             configs.ok, heard.ok);
    return ESP_OK;
}

esp_err_t layouts_nvs_init(esl_layouts_t *ls)
{
    static const esl_layout_ops_t ops = {NULL, lock, unlock, layout_save, layout_erase, NULL};
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
