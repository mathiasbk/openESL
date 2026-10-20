#include <stdio.h>
#include <string.h>
#include "app.h"
#include "esl_radio.h"
#include "esp_coexist.h"
#include "esp_ieee802154.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
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
 * The task is registered with the task watchdog and wakes at least once a
 * second to feed it, so a radio task stuck in a lock or the driver restarts
 * the AP (sdkconfig.defaults: CONFIG_ESP_TASK_WDT_PANIC).
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

#if SNIFFER && CONFIG_ESL_SNIFFER_HOP_MS
#define HOP_MS CONFIG_ESL_SNIFFER_HOP_MS
#define HOP_HOLD_US (10 * 1000000LL) /* stay on a channel this long after a frame */
#else
#define HOP_MS 0
#endif

typedef struct {
    uint8_t len; /* PSDU length incl. FCS */
    uint8_t psdu[ESL_PSDU_MAX];
    int8_t rssi;
    uint8_t channel;
    int64_t rx_us;
} rx_item_t;

static QueueHandle_t rx_queue;
static SemaphoreHandle_t tx_idle;
static esl_store_t *store;
static esl_layouts_t *layouts;
static uint16_t pan;
static uint8_t tx_buf[1 + ESL_PSDU_MAX]; /* length byte + PSDU */

/* esp_ieee802154_tx_error_t of the last transmit that failed, -1 = it went out */
static volatile int tx_err = -1;

/* A reply the radio refused (WiFi has the antenna under coexistence, say) is sent again
   while the tag still listens: it waits CFG_RX_TIMEOUT_MS (20 ms) after its POLL or
   request, and its own retry comes only after that. */
#define TX_RETRY_US      12000   /* from the request's arrival */
#define TX_RETRY_GAP_US  700     /* let a WiFi frame or another tag's frame pass */
#define TX_RETRY_MAX     6

static struct {
    uint32_t rx, polls, blocks, replies, tx_failed, tx_resent, dropped;
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
    item.channel = info->channel;
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
    BaseType_t woken = pdFALSE;
    stats.tx_failed++;
    tx_err = (int)error;
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
    ESP_LOGI(TAG, "rx ch %u %u B rssi %d: %s", it->channel, it->len, it->rssi, hex);
}

static void __attribute__((unused)) handle_poll(const rx_item_t *it)
{
    esl_radio_event_t ev;
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;

    /* The previous reply must be out of the radio before tx_buf is reused. */
    if (xSemaphoreTake(tx_idle, pdMS_TO_TICKS(20)) != pdTRUE)
        ESP_LOGW(TAG, "previous transmit did not finish");
    size_t len = esl_radio_handle(store, layouts, pan, it->psdu, it->len, it->rssi, now_ms, &tx_buf[1], &ev);
    if (!len) {
        xSemaphoreGive(tx_idle); /* not a POLL or LAYOUT_REQ for us */
        return;
    }
    if (ev.msg_type == ESL_MSG_POLL)
        stats.polls++;
    else
        stats.blocks++;
    tx_buf[0] = (uint8_t)len;
    tx_err = -1;
    esp_err_t err = esp_ieee802154_transmit(tx_buf, false);
    int64_t us = esp_timer_get_time() - it->rx_us;
    if (err != ESP_OK) {
        stats.tx_failed++;
        xSemaphoreGive(tx_idle);
        return;
    }
    /* Wait for each try to finish; tx_idle stays given for the next request */
    bool sent = true;
    int first_err = -1;
    for (int tries = 0;; tries++) {
        if (xSemaphoreTake(tx_idle, pdMS_TO_TICKS(20)) != pdTRUE)
            break;
        int e = tx_err;
        if (e < 0) {
            xSemaphoreGive(tx_idle);
            if (tries)
                ESP_LOGW(TAG, "reply sent after %d resends (tx error %d)", tries, first_err);
            break;
        }
        if (first_err < 0)
            first_err = e;
        if (tries >= TX_RETRY_MAX ||
            esp_timer_get_time() + TX_RETRY_GAP_US - it->rx_us > TX_RETRY_US) {
            xSemaphoreGive(tx_idle);
            ESP_LOGW(TAG, "reply not sent (tx error %d) after %d resends", e, tries);
            sent = false;
            break;
        }
        esp_rom_delay_us(TX_RETRY_GAP_US);
        tx_err = -1;
        if (esp_ieee802154_transmit(tx_buf, false) != ESP_OK) {
            stats.tx_failed++;
            xSemaphoreGive(tx_idle);
            sent = false;
            break;
        }
        stats.tx_resent++;
    }
    if (sent)
        stats.replies++;
    if (us > stats.worst_us)
        stats.worst_us = us;
    if (us > 5000)
        ESP_LOGW(TAG, "slow reply: %lld us", (long long)us);

    char m[17];
    esl_mac_format(ev.mac, m);
    if (ev.msg_type == ESL_MSG_LAYOUT_REQ) {
        ESP_LOGD(TAG, "%s layout block ver=%08lX pos=%u -> %lld us", m, (unsigned long)ev.req.layout_ver,
                 (unsigned)ev.req.pos, (long long)us);
        return;
    }
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
    const char *reply = ev.reply_type == ESL_MSG_CONTENT  ? "CONTENT"
                        : ev.reply_type == ESL_MSG_LAYOUT ? "LAYOUT"
                        : ev.reply_type == ESL_MSG_CONFIG ? "CONFIG"
                        : ev.reply_type == ESL_MSG_FW     ? "FW"
                        : ev.reply_type == ESL_MSG_IMG    ? "IMG"
                                                          : "IDLE";
    ESP_LOGI(TAG, "%s poll id=%08lX fw=%u bat=%u model=%u layout=%08lX rssi=%d -> %s in %lld us", m,
             (unsigned long)ev.poll.content_id, ev.poll.fw_ver, ev.poll.battery_mv, ev.poll.model,
             (unsigned long)ev.poll.layout_ver,
             it->rssi, reply, (long long)us);
}

#if HOP_MS
/* Move the sniffer to the next channel unless a frame was heard lately. */
static void hop(int64_t *next_hop_us, int64_t last_frame_us)
{
    static uint8_t ch = 11;
    int64_t now = esp_timer_get_time();
    if (now < *next_hop_us || now - last_frame_us < HOP_HOLD_US)
        return;
    *next_hop_us = now + HOP_MS * 1000LL;
    ch = ch >= 26 ? 11 : ch + 1;
    esp_ieee802154_set_channel(ch);
    esp_ieee802154_receive();
    if (ch == 11)
        ESP_LOGI(TAG, "hopping: channels 11-26, %d ms each", HOP_MS);
}
#endif

static void radio_task(void *arg)
{
    (void)arg;
    rx_item_t it;
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
        if (xQueueReceive(rx_queue, &it, wait ? wait : 1) == pdTRUE) {
            stats.rx++;
#if SNIFFER
            log_frame(&it);
#if HOP_MS
            last_frame_us = it.rx_us;
#endif
#else
            handle_poll(&it);
#endif
        }
#if HOP_MS
        hop(&next_hop_us, last_frame_us);
#endif
        if (esp_timer_get_time() - last_stats > 60 * 1000000LL) {
            last_stats = esp_timer_get_time();
            ESP_LOGI(TAG, "heap: free %u, largest block %u, lowest free %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            ESP_LOGI(TAG, "stats: rx %lu polls %lu blocks %lu replies %lu tx_failed %lu tx_resent %lu dropped %lu worst %lld us",
                     (unsigned long)stats.rx, (unsigned long)stats.polls, (unsigned long)stats.blocks,
                     (unsigned long)stats.replies, (unsigned long)stats.tx_failed, (unsigned long)stats.tx_resent,
                     (unsigned long)stats.dropped, (long long)stats.worst_us);
        }
    }
}

esp_err_t radio_start(esl_store_t *st, esl_layouts_t *ls, uint8_t channel, uint16_t pan_id)
{
    store = st;
    layouts = ls;
    pan = pan_id;
    rx_queue = xQueueCreate(8, sizeof(rx_item_t));
    tx_idle = xSemaphoreCreateBinary();
    if (!rx_queue || !tx_idle)
        return ESP_ERR_NO_MEM;
    xSemaphoreGive(tx_idle);

    esp_err_t err = esp_ieee802154_enable();
    if (err != ESP_OK)
        return err;
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    /* Without this the arbiter never shares the radio: 802.15.4 receiving
       keeps WiFi deaf (no scan results, "no AP found", no traffic), and the
       state outlives a USB or software reset. The sniffer runs without WiFi. */
    if (!SNIFFER) {
        err = esp_coex_wifi_i154_enable();
        if (err != ESP_OK)
            ESP_LOGE(TAG, "WiFi/802.15.4 coexistence: %s", esp_err_to_name(err));
    }
#endif
    esp_ieee802154_set_channel(channel);
    esp_ieee802154_set_txpower(CONFIG_ESL_TX_POWER_DBM);
    esp_ieee802154_set_panid(pan_id);
    esp_ieee802154_set_short_address(AP_SHORT_ADDR);
    esp_ieee802154_set_promiscuous(SNIFFER);
    esp_ieee802154_set_rx_when_idle(true);

    /* esl_radio_handle copies one model's layout names (about 0.5 KB) onto the stack */
    if (xTaskCreate(radio_task, "radio", 6144, NULL, configMAX_PRIORITIES - 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    err = esp_ieee802154_receive();
    ESP_LOGI(TAG, "802.15.4 %s on channel %u, PAN 0x%04X",
             SNIFFER ? "sniffer" : "AP", channel, pan_id);
    return err;
}
