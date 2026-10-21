#include "app.h"
#include "esl_radio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* Radio stub: no 802.15.4. CONFIG_ESL_STUB_TAGS simulated tags poll the AP
 * every CONFIG_ESL_STUB_INTERVAL_S seconds and "show" whatever image they get,
 * so the HTTP API (pending, shown_content_id, seen_s_ago, model) can be tested
 * with curl before any tag radio works.
 *
 * The simulated tags are esl_faketag (components/esl_core), and their frames go
 * through esl_radio_handle() exactly like frames from the real radio, image
 * downloads included (esl_faketag_round). Odd tags report model 2.9", even
 * tags 1.54".
 *
 * Simulated MACs: 00000000FAC70001, 00000000FAC70002, ...
 *
 * The task feeds the task watchdog every second while it waits, like the real
 * radio task. */

static const char *TAG = "radio_stub";

#define STUB_MAC_BASE 0x00000000FAC70000ull

static uint16_t stub_pan;
static esl_layouts_t *stub_layouts;

static void stub_task(void *arg)
{
    esl_store_t *st = arg;
    static esl_faketag_t tags[CONFIG_ESL_STUB_TAGS];

    for (int i = 0; i < CONFIG_ESL_STUB_TAGS; i++)
        esl_faketag_init(&tags[i], STUB_MAC_BASE + 1 + (uint64_t)i, 1, (uint16_t)(3000 - 10 * i),
                         i % 2 ? ESL_MODEL_154 : ESL_MODEL_29);

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        for (int i = 0; i < CONFIG_ESL_STUB_TAGS; i++) {
            esl_faketag_t *t = &tags[i];
            uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
            uint8_t type = esl_faketag_round(t, st, stub_layouts, stub_pan, (int8_t)(-50 - i), now_ms);
            char mac[17];
            esl_mac_format(t->mac, mac);
            if (type == ESL_MSG_IMG)
                ESP_LOGI(TAG, "%s got IMG, now showing %08lX", mac, (unsigned long)t->shown);
            else if (type == ESL_MSG_CONFIG)
                ESP_LOGI(TAG, "%s got CONFIG ver=%04X, interval now %u s", mac, (unsigned)t->cfg_ver,
                         (unsigned)t->interval_s);
        }
        for (int s = 0; s < CONFIG_ESL_STUB_INTERVAL_S; s++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

esp_err_t radio_start(esl_store_t *st, esl_layouts_t *ls, uint8_t channel, uint16_t pan_id)
{
    ESP_LOGW(TAG, "radio stub active (channel %u not used), %d simulated tags", channel, CONFIG_ESL_STUB_TAGS);
    stub_pan = pan_id;
    stub_layouts = ls;
    /* esl_faketag_round keeps two frames and esl_radio_handle a layout name list on the stack */
    return xTaskCreate(stub_task, "radio_stub", 6144, st, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
