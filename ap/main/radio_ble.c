#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esl_image.h"
#include "esl_push.h"
#include "esl_radio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdkconfig.h"

/* Bluetooth tags (docs/ble-tags.md, experimental): no 802.15.4. NimBLE scans all the
 * time; each advertisement goes on a queue to the push task, which hands the ones from
 * paired tags to esl_push_seen() and runs esl_push_round() every PUSH_ROUND_MS.
 *
 * Not done yet: pairing (an API to add a tag, kept in NVS with the id it was last given)
 * and the protocol modules, so the transport answers AWAY. What this does today: the
 * firmware builds with NimBLE in the central role, and the scan logs every BLE device it
 * hears (once each, up to SEEN_LOG), which is what a tag's address and name are found
 * with. */

static const char *TAG = "radio_ble";

#define PUSH_ROUND_MS 2000
#define PUSH_SENDS    2     /* per round: each blocks the task for a connection */
#define SEEN_QUEUE    16
#define SEEN_LOG      64

typedef struct {
    uint64_t mac;
    int8_t rssi;
} seen_t;

static QueueHandle_t seen_q;
static esl_push_t push;
static esl_store_t *store;
static esl_layouts_t *layouts;
static uint8_t own_addr_type;
static uint64_t logged[SEEN_LOG];
static unsigned n_logged;

static uint64_t addr_mac(const ble_addr_t *a)
{
    uint64_t m = 0;
    for (int i = 5; i >= 0; i--) /* val[] is least significant first */
        m = (m << 8) | a->val[i];
    return m;
}

/* The scan's log: a device the first time it is heard, with its name if it gives one */
static void log_device(uint64_t mac, int8_t rssi, const uint8_t *data, uint8_t len)
{
    for (unsigned i = 0; i < n_logged; i++)
        if (logged[i] == mac)
            return;
    if (n_logged == SEEN_LOG)
        return;
    logged[n_logged++] = mac;
    struct ble_hs_adv_fields f;
    char name[32] = "";
    if (ble_hs_adv_parse_fields(&f, data, len) == 0 && f.name && f.name_len) {
        size_t n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
        memcpy(name, f.name, n);
        name[n] = 0;
    }
    ESP_LOGI(TAG, "heard %02X:%02X:%02X:%02X:%02X:%02X %d dBm %s", (unsigned)(mac >> 40) & 0xFF,
             (unsigned)(mac >> 32) & 0xFF, (unsigned)(mac >> 24) & 0xFF, (unsigned)(mac >> 16) & 0xFF,
             (unsigned)(mac >> 8) & 0xFF, (unsigned)mac & 0xFF, rssi, name);
}

static void scan_start(void);

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    if (ev->type == BLE_GAP_EVENT_DISC) {
        seen_t s = {addr_mac(&ev->disc.addr), ev->disc.rssi};
        log_device(s.mac, s.rssi, ev->disc.data, ev->disc.length_data);
        xQueueSend(seen_q, &s, 0); /* full: this report is dropped, the next one comes in a second */
    } else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        scan_start();
    }
    return 0;
}

static void scan_start(void)
{
    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof(p));
    p.passive = 1;           /* listen only: the tags' scan responses are not needed to find them */
    /* 40 ms of every 320 ms (units of 0.625 ms), not the default of all the time: the radio is
       shared with WiFi. A tag advertises about once a second, so it is still heard within a
       few seconds, which is all "last seen" and a woken retry need */
    p.itvl = 512;
    p.window = 64;
    p.filter_duplicates = 0; /* every advertisement: "last seen" and the signal come from them */
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc)
        ESP_LOGE(TAG, "scan did not start: %d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    scan_start();
    ESP_LOGI(TAG, "scanning; free heap %u, smallest since boot %u", (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* No protocol module yet: nothing can be delivered */
static esl_push_res_t send_none(void *ctx, uint64_t mac, uint8_t model, const uint8_t *img, size_t len,
                                esl_push_info_t *info)
{
    (void)ctx;
    (void)model;
    (void)img;
    (void)info;
    ESP_LOGW(TAG, "%016llX: no protocol module for its model yet (%u bytes not sent)", (unsigned long long)mac,
             (unsigned)len);
    return ESL_PUSH_AWAY;
}

static void push_task(void *arg)
{
    (void)arg;
    ESL_CHECK(esp_task_wdt_add(NULL));
    uint64_t next = 0;
    for (;;) {
        seen_t s;
        if (xQueueReceive(seen_q, &s, pdMS_TO_TICKS(500)) == pdTRUE)
            esl_push_seen(&push, store, layouts, s.mac, s.rssi, 0, (uint64_t)esp_timer_get_time() / 1000);
        uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
        esl_image_t *im = esl_radio_image(); /* app_main's buffers; NULL without the memory */
        if (now >= next && im) {
            esl_push_round(&push, store, layouts, im, now, PUSH_SENDS);
            next = now + PUSH_ROUND_MS;
        }
        esp_task_wdt_reset();
    }
}

esp_err_t radio_start(esl_store_t *st, esl_layouts_t *ls, uint8_t channel, uint16_t pan_id)
{
    (void)channel;
    (void)pan_id;
    store = st;
    layouts = ls;
    size_t heap0 = esp_get_free_heap_size();
    static const esl_push_out_t out = {NULL, send_none};
    /* esl_push draws into a buffer of its own; app_main's image buffers (esl_radio_image())
       only lend it their lock, as for radio_net.c */
    size_t cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_DEFAULT);
    seen_q = xQueueCreate(SEEN_QUEUE, sizeof(seen_t));
    if (!buf || !seen_q) {
        free(buf);
        if (seen_q)
            vQueueDelete(seen_q);
        seen_q = NULL;
        return ESP_ERR_NO_MEM;
    }
    esl_push_init(&push, &out, buf, cap);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE did not start: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    if (xTaskCreate(push_task, "ble_push", 6656, NULL, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGW(TAG, "Bluetooth tags (experimental): no 802.15.4; BLE and push took %u bytes of heap",
             (unsigned)(heap0 - esp_get_free_heap_size()));
    return ESP_OK;
}
