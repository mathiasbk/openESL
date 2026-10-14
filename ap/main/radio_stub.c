#include "app.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* Radio stub: no 802.15.4. CONFIG_ESL_STUB_TAGS simulated tags poll the store
 * every CONFIG_ESL_STUB_INTERVAL_S seconds and "show" whatever CONTENT they
 * get, so the HTTP API (pending, shown_content_id, seen_s_ago) can be tested
 * with curl before any tag radio works.
 *
 * Simulated MACs: 00000000FAC70001, 00000000FAC70002, ... */

static const char *TAG = "radio_stub";

#define STUB_MAC_BASE 0x00000000FAC70000ull

static void stub_task(void *arg)
{
    esl_store_t *st = arg;
    uint32_t shown[CONFIG_ESL_STUB_TAGS] = {0};
    uint8_t reply[ESL_MAX_PAYLOAD];

    for (;;) {
        for (int i = 0; i < CONFIG_ESL_STUB_TAGS; i++) {
            uint64_t mac = STUB_MAC_BASE + 1 + (uint64_t)i;
            esl_poll_t poll = {.fw_ver = 1, .battery_mv = (uint16_t)(3000 - 10 * i), .content_id = shown[i]};
            uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
            size_t n = esl_store_on_poll(st, mac, &poll, (int8_t)(-50 - i), now_ms, reply);
            if (n > ESL_CONTENT_HDR_LEN && reply[0] == ESL_MSG_CONTENT) {
                shown[i] = (uint32_t)reply[4] | ((uint32_t)reply[5] << 8) | ((uint32_t)reply[6] << 16) |
                           ((uint32_t)reply[7] << 24);
                char m[17];
                esl_mac_format(mac, m);
                ESP_LOGI(TAG, "%s got CONTENT id=%08lX (%u bytes), now showing it", m, (unsigned long)shown[i],
                         (unsigned)n);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ESL_STUB_INTERVAL_S * 1000));
    }
}

esp_err_t radio_start(esl_store_t *st, uint8_t channel, uint16_t pan_id)
{
    ESP_LOGW(TAG, "radio stub active (channel %u, PAN 0x%04X not used), %d simulated tags",
             channel, pan_id, CONFIG_ESL_STUB_TAGS);
    return xTaskCreate(stub_task, "radio_stub", 4096, st, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
