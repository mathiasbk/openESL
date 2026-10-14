#include <stdio.h>
#include <string.h>
#include "app.h"
#include "esl_radio.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* 802.15.4 side of the AP (docs/ap-firmware.md, Radio).
 *
 * The receive callback runs in interrupt context: it only copies the frame to
 * a queue. A high-priority task hands it to esl_radio_handle() (parse the
 * POLL, look up the ready-made reply, build the frame; tested on the PC) and
 * transmits the result. Target: under 5 ms from received POLL to
 * transmit (krav A3); the worst time seen is logged.
 *
 * Not yet built against ESP-IDF on real hardware. Check the driver API names
 * against the pinned ESP-IDF version on first build. */

static const char *TAG = "radio";

#define AP_SHORT_ADDR ESL_AP_SHORT_ADDR

/* Bool Kconfig symbols are undefined when off, so give the sniffer choice a 0/1 value */
#ifdef CONFIG_ESL_RADIO_SNIFFER
#define SNIFFER 1
#else
#define SNIFFER 0
#endif

typedef struct {
    uint8_t len; /* PSDU length incl. FCS */
    uint8_t psdu[ESL_PSDU_MAX];
    int8_t rssi;
    int64_t rx_us;
} rx_item_t;

static QueueHandle_t rx_queue;
static SemaphoreHandle_t tx_idle;
static esl_store_t *store;
static uint16_t pan;
static uint8_t tx_buf[1 + ESL_PSDU_MAX]; /* length byte + PSDU */

static struct {
    uint32_t rx, polls, replies, tx_failed, dropped;
    int64_t worst_us;
} stats;

/* ---- driver callbacks (weak symbols in esp_ieee802154, ISR context) ---- */

void IRAM_ATTR esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    rx_item_t item;
    BaseType_t woken = pdFALSE;
    item.len = frame[0];
    if (item.len > ESL_PSDU_MAX)
        item.len = ESL_PSDU_MAX;
    memcpy(item.psdu, &frame[1], item.len);
    item.rssi = info->rssi;
    item.rx_us = esp_timer_get_time();
    esp_ieee802154_receive_handle_done(frame);
    if (xQueueSendFromISR(rx_queue, &item, &woken) != pdTRUE)
        stats.dropped++;
    portYIELD_FROM_ISR(woken);
}

void IRAM_ATTR esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack, esp_ieee802154_frame_info_t *ack_info)
{
    (void)frame;
    (void)ack_info;
    BaseType_t woken = pdFALSE;
    if (ack)
        esp_ieee802154_receive_handle_done(ack);
    xSemaphoreGiveFromISR(tx_idle, &woken);
    portYIELD_FROM_ISR(woken);
}

void IRAM_ATTR esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    (void)frame;
    (void)error;
    BaseType_t woken = pdFALSE;
    stats.tx_failed++;
    xSemaphoreGiveFromISR(tx_idle, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---- task ---- */

/* Only one of these is used, depending on the radio mode. */
static void __attribute__((unused)) log_frame(const rx_item_t *it)
{
    char hex[3 * ESL_PSDU_MAX + 1];
    for (unsigned i = 0; i < it->len; i++)
        snprintf(&hex[3 * i], 4, "%02X ", it->psdu[i]);
    hex[3 * it->len] = '\0';
    ESP_LOGI(TAG, "rx %u B rssi %d: %s", it->len, it->rssi, hex);
}

static void __attribute__((unused)) handle_poll(const rx_item_t *it)
{
    esl_radio_event_t ev;
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;

    /* The previous reply must be out of the radio before tx_buf is reused. */
    if (xSemaphoreTake(tx_idle, pdMS_TO_TICKS(20)) != pdTRUE)
        ESP_LOGW(TAG, "previous transmit did not finish");
    size_t len = esl_radio_handle(store, pan, it->psdu, it->len, it->rssi, now_ms, &tx_buf[1], &ev);
    if (!len) {
        xSemaphoreGive(tx_idle); /* not a POLL for us */
        return;
    }
    stats.polls++;
    tx_buf[0] = (uint8_t)len;
    esp_err_t err = esp_ieee802154_transmit(tx_buf, false);
    int64_t us = esp_timer_get_time() - it->rx_us;
    if (err != ESP_OK) {
        stats.tx_failed++;
        xSemaphoreGive(tx_idle);
        return;
    }
    stats.replies++;
    if (us > stats.worst_us)
        stats.worst_us = us;
    if (us > 5000)
        ESP_LOGW(TAG, "slow reply: %lld us", (long long)us);

    char m[17];
    esl_mac_format(ev.mac, m);
    ESP_LOGI(TAG, "%s poll id=%08lX bat=%u model=%u rssi=%d -> %s in %lld us", m, (unsigned long)ev.poll.content_id,
             ev.poll.battery_mv, ev.poll.model, it->rssi, ev.reply_type == ESL_MSG_CONTENT ? "CONTENT" : "IDLE",
             (long long)us);
}

static void radio_task(void *arg)
{
    (void)arg;
    rx_item_t it;
    int64_t last_stats = esp_timer_get_time();
    for (;;) {
        if (xQueueReceive(rx_queue, &it, pdMS_TO_TICKS(1000)) == pdTRUE) {
            stats.rx++;
#if SNIFFER
            log_frame(&it);
#else
            handle_poll(&it);
#endif
        }
        if (esp_timer_get_time() - last_stats > 60 * 1000000LL) {
            last_stats = esp_timer_get_time();
            ESP_LOGI(TAG, "stats: rx %lu polls %lu replies %lu tx_failed %lu dropped %lu worst %lld us",
                     (unsigned long)stats.rx, (unsigned long)stats.polls, (unsigned long)stats.replies,
                     (unsigned long)stats.tx_failed, (unsigned long)stats.dropped, (long long)stats.worst_us);
        }
    }
}

esp_err_t radio_start(esl_store_t *st, uint8_t channel, uint16_t pan_id)
{
    store = st;
    pan = pan_id;
    rx_queue = xQueueCreate(8, sizeof(rx_item_t));
    tx_idle = xSemaphoreCreateBinary();
    if (!rx_queue || !tx_idle)
        return ESP_ERR_NO_MEM;
    xSemaphoreGive(tx_idle);

    esp_err_t err = esp_ieee802154_enable();
    if (err != ESP_OK)
        return err;
    esp_ieee802154_set_channel(channel);
    esp_ieee802154_set_txpower(CONFIG_ESL_TX_POWER_DBM);
    esp_ieee802154_set_panid(pan_id);
    esp_ieee802154_set_short_address(AP_SHORT_ADDR);
    esp_ieee802154_set_promiscuous(SNIFFER);
    esp_ieee802154_set_rx_when_idle(true);

    if (xTaskCreate(radio_task, "radio", 4096, NULL, configMAX_PRIORITIES - 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    err = esp_ieee802154_receive();
    ESP_LOGI(TAG, "802.15.4 %s on channel %u, PAN 0x%04X",
             SNIFFER ? "sniffer" : "AP", channel, pan_id);
    return err;
}
