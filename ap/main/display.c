#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esl_lcd.h"
#include "esl_screen.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* AP status screen: what esl_screen.c (host-tested) makes of the AP's state,
 * every ESL_LCD_REFRESH_MS. It goes to a small ST7789 LCD on this board
 * (ESL_DISPLAY, components/esl_lcd), and with a radio coprocessor on UART to
 * the coprocessor too, which shows it on its own LCD if it has one
 * (radio_screen(), docs/ap-radio-link.md). */

static const char *TAG = "display";

#if CONFIG_ESL_DISPLAY
#define REFRESH_MS CONFIG_ESL_LCD_REFRESH_MS
#else
#define REFRESH_MS 2000
#endif
#define SEEN_RECENT_MS (10 * 60 * 1000)

static const esl_api_t *api;

static void gather(esl_screen_info_t *in)
{
    memset(in, 0, sizeof(*in));
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    in->fw = api->fw_version;
    in->channel = api->channel;
    in->tags_max = api->store->cap;
    in->uptime_s = now_ms / 1000;
#if CONFIG_ESL_SERIAL_API
    in->serial_api = true;
#endif
    for (unsigned s = 0; s < api->store->cap; s++) {
        esl_tag_t t;
        if (!esl_store_get_slot(api->store, s, &t))
            continue;
        in->tags++;
        if (esl_tag_pending(&t))
            in->pending++;
        if (t.seen && now_ms - t.last_seen_ms < SEEN_RECENT_MS)
            in->seen++;
    }
    if (api->wifi)
        api->wifi->status(api->wifi->ctx, &in->wifi);
}

static void show(const esl_screen_t *s)
{
#if CONFIG_ESL_DISPLAY
    esl_lcd_show(s);
#endif
#if CONFIG_ESL_RADIO_UART
    radio_screen(s);
#endif
}

static void display_task(void *arg)
{
    (void)arg;
    esl_screen_info_t *in = malloc(sizeof(*in));
    esl_screen_t *scr = malloc(sizeof(*scr));
    if (!in || !scr) {
        ESP_LOGE(TAG, "out of memory");
        vTaskDelete(NULL);
    }
    vTaskDelay(pdMS_TO_TICKS(2000)); /* the start-up screen's frame, to check the gaps */
    for (;;) {
        gather(in);
        esl_screen_make(in, scr);
        show(scr);
        vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    }
}

esp_err_t display_start(const esl_api_t *a)
{
    api = a;
#if CONFIG_ESL_DISPLAY
    esl_screen_t s = {0};
    strcpy(s.text[0], "OpenESL AP");
    strncpy(s.text[1], api->fw_version ? api->fw_version : "", ESL_SCREEN_LINE_MAX - 1);
    strcpy(s.text[2], "starting");
    s.color[0] = ESL_SCREEN_TITLE;
    s.color[1] = s.color[2] = ESL_SCREEN_TEXT;
    esp_err_t err = esl_lcd_start(&s);
    if (err != ESP_OK)
        return err;
#endif
    if (xTaskCreate(display_task, "display", 4096, NULL, 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
