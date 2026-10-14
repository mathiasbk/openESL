#include "app.h"
#include "esl_radio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* Radio stub: no 802.15.4. CONFIG_ESL_STUB_TAGS simulated tags poll the AP
 * every CONFIG_ESL_STUB_INTERVAL_S seconds and "show" whatever CONTENT they
 * get, so the HTTP API (pending, shown_content_id, seen_s_ago, model) can be
 * tested with curl before any tag radio works.
 *
 * The simulated tags are esl_faketag (components/esl_core), and their frames go
 * through esl_radio_handle() exactly like frames from the real radio. Odd
 * tags report model 2.9", even tags 1.54".
 *
 * Simulated MACs: 00000000FAC70001, 00000000FAC70002, ... */

static const char *TAG = "radio_stub";

#define STUB_MAC_BASE 0x00000000FAC70000ull

static uint16_t stub_pan;

static void stub_task(void *arg)
{
    esl_store_t *st = arg;
    static esl_faketag_t tags[CONFIG_ESL_STUB_TAGS];
    uint8_t rx[ESL_PSDU_MAX], tx[ESL_PSDU_MAX];

    for (int i = 0; i < CONFIG_ESL_STUB_TAGS; i++)
        esl_faketag_init(&tags[i], STUB_MAC_BASE + 1 + (uint64_t)i, 1, (uint16_t)(3000 - 10 * i),
                         i % 2 ? ESL_MODEL_154 : ESL_MODEL_29);

    for (;;) {
        for (int i = 0; i < CONFIG_ESL_STUB_TAGS; i++) {
            esl_faketag_t *t = &tags[i];
            uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
            size_t n = esl_faketag_poll(t, stub_pan, rx);
            size_t m = esl_radio_handle(st, stub_pan, rx, n, (int8_t)(-50 - i), now_ms, tx, NULL);
            if (m && esl_faketag_receive(t, tx, m) == ESL_MSG_CONTENT) {
                char mac[17];
                esl_mac_format(t->mac, mac);
                ESP_LOGI(TAG, "%s got CONTENT id=%08lX (%u bytes), now showing it", mac, (unsigned long)t->shown,
                         (unsigned)m);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ESL_STUB_INTERVAL_S * 1000));
    }
}

esp_err_t radio_start(esl_store_t *st, uint8_t channel, uint16_t pan_id)
{
    ESP_LOGW(TAG, "radio stub active (channel %u not used), %d simulated tags", channel, CONFIG_ESL_STUB_TAGS);
    stub_pan = pan_id;
    return xTaskCreate(stub_task, "radio_stub", 4096, st, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
