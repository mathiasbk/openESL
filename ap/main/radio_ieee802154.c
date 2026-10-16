#include <stdio.h>
#include <string.h>
#include "app.h"
#include "esl_radio.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
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

#if SNIFFER && CONFIG_ESL_SNIFF_HOP_MS > 0
#define HOP_MS CONFIG_ESL_SNIFF_HOP_MS
#else
#define HOP_MS 0
#endif

#if SNIFFER && defined(CONFIG_ESL_SNIFF_PROBE_REPLY)
#define PROBE_REPLY 1
#else
#define PROBE_REPLY 0
#endif

typedef struct {
    uint8_t len; /* PSDU length incl. FCS */
    uint8_t psdu[ESL_PSDU_MAX];
    int8_t rssi;
    uint8_t channel;
    uint8_t variant; /* guessed reply sent to this frame (0 = none) */
    int64_t rx_us;
} rx_item_t;

static QueueHandle_t rx_queue;
static SemaphoreHandle_t tx_idle;
static esl_store_t *store;
static esl_layouts_t *layouts;
static uint16_t pan;
static volatile uint8_t cur_channel;
static uint8_t tx_buf[1 + ESL_PSDU_MAX]; /* length byte + PSDU */

#if PROBE_REPLY
/* Search frame of an unpaired stock tag: FCF 41 88, seq, PAN 1234, dest FFFF, source 10AF, E0 <model> */
static bool is_stock_probe(const uint8_t *p, uint8_t len) /* len incl. 2 FCS bytes */
{
    static const uint8_t head[2] = { 0x41, 0x88 };
    static const uint8_t addr[6] = { 0x34, 0x12, 0xFF, 0xFF, 0xAF, 0x10 };
    return len == 13 && !memcmp(p, head, 2) && !memcmp(&p[3], addr, 6) && p[9] == 0xE0;
}

/* Guessed answer: data frame, PAN compression, short dest 10AF, long source (AP address),
 * then message type E1 and zeros. Variants differ in payload length only. */
static uint8_t build_probe_reply(uint8_t *out, const uint8_t *probe, uint8_t variant)
{
    static const uint8_t payload_len[] = { 26, 40, 70 };
    uint8_t n = 0;
    out[1 + n++] = 0x41;
    out[1 + n++] = 0xC8;
    out[1 + n++] = probe[2]; /* sequence number of the search frame */
    out[1 + n++] = 0x34;
    out[1 + n++] = 0x12;
    out[1 + n++] = 0xAF;
    out[1 + n++] = 0x10;
    static const uint8_t src[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };
    memcpy(&out[1 + n], src, 8);
    n += 8;
    out[1 + n++] = 0xE1;
    uint8_t zeros = payload_len[(variant - 1) % 3] - 1;
    memset(&out[1 + n], 0, zeros);
    n += zeros;
    out[0] = n + 2; /* PSDU length incl. 2 FCS bytes, which the hardware fills in */
    return n;
}
#endif

static struct {
    uint32_t rx, polls, blocks, replies, tx_failed, dropped;
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
    item.channel = cur_channel;
    item.variant = 0;
    item.rx_us = esp_timer_get_time();
#if PROBE_REPLY
    static uint8_t probe_count;
    static uint8_t reply_buf[1 + ESL_PSDU_MAX];
    bool probe = is_stock_probe(item.psdu, item.len);
    if (probe) {
        item.variant = 1 + probe_count++ % 3;
        build_probe_reply(reply_buf, item.psdu, item.variant);
    }
    esp_ieee802154_receive_handle_done(frame);
    if (probe)
        esp_ieee802154_transmit(reply_buf, false);
#else
    esp_ieee802154_receive_handle_done(frame);
#endif
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
    ESP_LOGI(TAG, "rx ch %u %u B rssi %d: %s", it->channel, it->len, it->rssi, hex);
    if (it->variant)
        ESP_LOGI(TAG, "reply variant %u sent on ch %u", it->variant, it->channel);
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
    const char *reply = ev.reply_type == ESL_MSG_CONTENT  ? "CONTENT"
                        : ev.reply_type == ESL_MSG_LAYOUT ? "LAYOUT"
                        : ev.reply_type == ESL_MSG_CONFIG ? "CONFIG"
                        : ev.reply_type == ESL_MSG_FW     ? "FW"
                                                          : "IDLE";
    ESP_LOGI(TAG, "%s poll id=%08lX fw=%u bat=%u model=%u layout=%08lX rssi=%d -> %s in %lld us", m,
             (unsigned long)ev.poll.content_id, ev.poll.fw_ver, ev.poll.battery_mv, ev.poll.model,
             (unsigned long)ev.poll.layout_ver,
             it->rssi, reply, (long long)us);
}

static void radio_task(void *arg)
{
    (void)arg;
    rx_item_t it;
    int64_t last_stats = esp_timer_get_time();
    int64_t last_hop = last_stats;
    TickType_t wait = pdMS_TO_TICKS(HOP_MS > 0 && HOP_MS < 1000 ? HOP_MS : 1000);
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        esp_task_wdt_reset();
        if (HOP_MS > 0 && esp_timer_get_time() - last_hop >= HOP_MS * 1000LL) {
            last_hop = esp_timer_get_time();
            uint8_t ch = cur_channel >= 26 ? 11 : cur_channel + 1;
            esp_ieee802154_set_channel(ch);
            cur_channel = ch;
            esp_ieee802154_receive();
            ESP_LOGD(TAG, "channel %u", ch);
        }
        if (xQueueReceive(rx_queue, &it, wait) == pdTRUE) {
            stats.rx++;
#if SNIFFER
            log_frame(&it);
#else
            handle_poll(&it);
#endif
        }
        if (esp_timer_get_time() - last_stats > 60 * 1000000LL) {
            last_stats = esp_timer_get_time();
            ESP_LOGI(TAG, "stats: rx %lu polls %lu blocks %lu replies %lu tx_failed %lu dropped %lu worst %lld us",
                     (unsigned long)stats.rx, (unsigned long)stats.polls, (unsigned long)stats.blocks,
                     (unsigned long)stats.replies,
                     (unsigned long)stats.tx_failed, (unsigned long)stats.dropped, (long long)stats.worst_us);
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
    esp_ieee802154_set_channel(channel);
    cur_channel = channel;
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
    if (HOP_MS > 0)
        ESP_LOGI(TAG, "sniffer steps through channels 11-26, %d ms each", HOP_MS);
    return err;
}
