#include <stdio.h>
#include "app.h"
#include "esl_link.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "radio.h"
#include "sdkconfig.h"

/* The radio task (docs/ap-firmware.md, Radio), the same for every link: the
 * driver (radio_ieee802154.c, ...) puts each received frame on a queue, and a
 * high-priority task hands it to esl_link_handle() (parse, look up the
 * ready-made reply, build the frame; tested on the PC), which sends the reply
 * with radio_link.send. Target: under 5 ms from received POLL to transmit
 * (krav A3); the worst time seen is logged.
 *
 * The task is registered with the task watchdog and wakes at least once a
 * second to feed it, so a radio task stuck in a lock or the driver restarts
 * the AP (sdkconfig.defaults: CONFIG_ESP_TASK_WDT_PANIC). */

static const char *TAG = "radio";

/* Bool Kconfig symbols are undefined when off, so give the sniffer choice a 0/1 value */
#ifdef CONFIG_ESL_RADIO_SNIFFER
#define SNIFFER 1
#else
#define SNIFFER 0
#endif

#if SNIFFER && CONFIG_ESL_SNIFFER_HOP_MS
#define HOP_MS CONFIG_ESL_SNIFFER_HOP_MS
#define HOP_HOLD_US (10 * 1000000LL) /* stay on a channel this long after a frame */
#else
#define HOP_MS 0
#endif

static QueueHandle_t rx_queue;
static esl_store_t *store;
static esl_layouts_t *layouts;
static uint16_t pan;

bool IRAM_ATTR radio_rx_from_isr(const esl_link_rx_t *f)
{
    BaseType_t woken = pdFALSE;
    bool ok = xQueueSendFromISR(rx_queue, f, &woken) == pdTRUE;
    if (!ok)
        radio_link.stats.dropped++;
    portYIELD_FROM_ISR(woken);
    return ok;
}

bool radio_rx(const esl_link_rx_t *f)
{
    bool ok = xQueueSend(rx_queue, f, 0) == pdTRUE;
    if (!ok)
        radio_link.stats.dropped++;
    return ok;
}

static int64_t clock_us(void)
{
    return esp_timer_get_time();
}

/* Only one of these is used, depending on the radio mode. */
static void __attribute__((unused)) log_frame(const esl_link_rx_t *f)
{
    char hex[3 * ESL_PSDU_MAX + 1];
    for (unsigned i = 0; i < f->len; i++)
        snprintf(&hex[3 * i], 4, "%02X ", f->psdu[i]);
    hex[3 * f->len] = '\0';
    ESP_LOGI(TAG, "rx ch %u %u B rssi %d: %s", f->channel, f->len, f->rssi, hex);
}

static void __attribute__((unused)) handle_frame(const esl_link_rx_t *f)
{
    static uint8_t tx[ESL_PSDU_MAX];
    esl_radio_event_t ev;
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;

    if (!esl_link_handle(&radio_link, store, layouts, pan, f, now_ms, tx, &ev, clock_us, NULL))
        return;
    int64_t us = radio_link.stats.last_us;
    if (us > ESL_LINK_SLOW_US)
        ESP_LOGW(TAG, "slow reply: %lld us", (long long)us);

    char m[17];
    esl_mac_format(ev.mac, m);
    if (ev.msg_type == ESL_MSG_FW_REQ) {
        /* hundreds per download: debug only; the stats line counts them as blocks */
        ESP_LOGD(TAG, "%s fw block -> %s in %lld us", m, ev.reply_type == ESL_MSG_FW ? "FW" : "IDLE", (long long)us);
        return;
    }
    if (ev.msg_type == ESL_MSG_IMG_REQ) {
        /* dozens per image: debug only, as fw blocks */
        ESP_LOGD(TAG, "%s img block -> %s in %lld us", m, ev.reply_type == ESL_MSG_IMG ? "IMG" : "IDLE", (long long)us);
        return;
    }
    const char *reply = ev.reply_type == ESL_MSG_CONFIG   ? "CONFIG"
                        : ev.reply_type == ESL_MSG_FW     ? "FW"
                        : ev.reply_type == ESL_MSG_IMG    ? "IMG"
                                                          : "IDLE";
    ESP_LOGI(TAG, "%s poll id=%08lX fw=%u bat=%u model=%u rssi=%d -> %s in %lld us", m,
             (unsigned long)ev.poll.content_id, ev.poll.fw_ver, ev.poll.battery_mv, ev.poll.model, f->rssi, reply,
             (long long)us);
}

#if HOP_MS
/* Move the sniffer to the next channel unless a frame was heard lately. */
static void hop(int64_t *next_hop_us, int64_t last_frame_us)
{
    static uint8_t ch = 11;
    int64_t now = esp_timer_get_time();
    if (!radio_link.set_channel || now < *next_hop_us || now - last_frame_us < HOP_HOLD_US)
        return;
    *next_hop_us = now + HOP_MS * 1000LL;
    ch = ch >= 26 ? 11 : ch + 1;
    radio_link.set_channel(&radio_link, ch);
    if (ch == 11)
        ESP_LOGI(TAG, "hopping: channels 11-26, %d ms each", HOP_MS);
}
#endif

#if !SNIFFER
/* A planned channel move (esl_store_move_channel), once the tags have it or the time is up */
static void move_channel(void)
{
    uint8_t ch = esl_store_channel_step(store, (uint64_t)esp_timer_get_time() / 1000);
    if (!ch)
        return;
    int err = radio_link.set_channel ? radio_link.set_channel(&radio_link, ch) : ESP_ERR_NOT_SUPPORTED;
    ESP_LOGI(TAG, "moved to channel %u: %s", ch, esp_err_to_name(err));
}
#endif

static void radio_task(void *arg)
{
    (void)arg;
    esl_link_rx_t f;
    int64_t last_stats = esp_timer_get_time();
#if HOP_MS
    int64_t next_hop_us = 0, last_frame_us = -HOP_HOLD_US;
    const TickType_t wait = pdMS_TO_TICKS(HOP_MS < 1000 ? HOP_MS : 1000);
#else
    const TickType_t wait = pdMS_TO_TICKS(1000);
#endif
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(rx_queue, &f, wait ? wait : 1) == pdTRUE) {
#if SNIFFER
            radio_link.stats.rx++;
            log_frame(&f);
#if HOP_MS
            last_frame_us = f.rx_us;
#endif
#else
            handle_frame(&f);
#endif
        }
#if HOP_MS
        hop(&next_hop_us, last_frame_us);
#endif
#if !SNIFFER
        move_channel();
#endif
        if (esp_timer_get_time() - last_stats > 60 * 1000000LL) {
            const esl_link_stats_t *s = &radio_link.stats;
            last_stats = esp_timer_get_time();
            ESP_LOGI(TAG, "heap: free %u, largest block %u, lowest free %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            ESP_LOGI(TAG, "stats: rx %lu polls %lu blocks %lu replies %lu tx_failed %lu tx_resent %lu dropped %lu worst %lld us",
                     (unsigned long)s->rx, (unsigned long)s->polls, (unsigned long)s->blocks,
                     (unsigned long)s->replies, (unsigned long)s->tx_failed, (unsigned long)s->tx_resent,
                     (unsigned long)s->dropped, (long long)s->worst_us);
        }
    }
}

esp_err_t radio_start(esl_store_t *st, esl_layouts_t *ls, uint8_t channel, uint16_t pan_id)
{
    store = st;
    layouts = ls;
    pan = pan_id;
    rx_queue = xQueueCreate(8, sizeof(esl_link_rx_t));
    if (!rx_queue)
        return ESP_ERR_NO_MEM;
    /* esl_radio_handle copies one model's layout names (about 0.5 KB) onto the stack, and
       drawing an image holds a tag's content and its NVS blob (about 0.6 KB since #182) */
    if (xTaskCreate(radio_task, "radio", 6656, NULL, configMAX_PRIORITIES - 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    esp_err_t err = radio_link.start(&radio_link, channel, pan_id, ESL_AP_SHORT_ADDR, SNIFFER);
    int8_t dbm = radio_tx_power()->dbm;
    if (err == ESP_OK && dbm != CONFIG_ESL_TX_POWER_DBM) /* the driver starts with menuconfig's */
        radio_set_tx_power(dbm);
    ESP_LOGI(TAG, "%s %s on channel %u, PAN 0x%04X, %d dBm: %s", radio_link.name, SNIFFER ? "sniffer" : "AP",
             channel, pan_id, dbm, esp_err_to_name(err));
    return err;
}

void radio_set_tx_power(int8_t dbm)
{
    if (radio_link.set_tx_power)
        radio_link.set_tx_power(&radio_link, dbm);
}
