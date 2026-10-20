#include <stdio.h>
#include <string.h>
#include "app.h"
#include "driver/uart.h"
#include "esl_coproc.h"
#include "esl_radio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* 802.15.4 through a radio coprocessor on UART (docs/ap-radio-link.md): an
 * ESP32-C6 running ap/radio/ forwards every frame it receives as RX, this
 * file answers it with esl_radio_handle() exactly as radio_ieee802154.c does
 * on a C6 alone, and sends the reply back as TX. The radio sends it, again
 * while the tag still listens if it has to, and reports TX_DONE.
 *
 * The radio says HELLO when it starts and after each CONFIG. A HELLO that is
 * not configured (the radio restarted) gets the CONFIG again; while no HELLO
 * comes, PING goes out every couple of seconds.
 *
 * display.c sends the status screen through radio_screen(), for a radio with
 * an LCD (the C6 with a 0.96" screen as the radio).
 *
 * The task is registered with the task watchdog and wakes at least once a
 * second, like the radio task on the C6. */

static const char *TAG = "radio";

#define PORT CONFIG_ESL_COPROC_UART_NUM
#define PING_EVERY_US (2 * 1000000LL)

static esl_store_t *store;
static esl_layouts_t *layouts;
static esl_coproc_config_t cfg;
static QueueHandle_t uart_events;

static struct {
    bool up;              /* HELLO with our CONFIG seen */
    int64_t last_hello_us;
    int64_t last_ping_us;
} link;

static struct {
    uint32_t rx, polls, blocks, replies, tx_failed, tx_late, tx_resent;
    int64_t worst_us; /* RX arrival on the radio to first transmit */
} stats;

static void send(const uint8_t *f, size_t n)
{
    if (n)
        uart_write_bytes(PORT, f, n);
}

static void send_config(void)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    send(f, esl_coproc_encode_config(f, &cfg));
}

_Static_assert(ESL_SCREEN_LINES == ESL_COPROC_SCREEN_LINES, "esl_screen_t and SCREEN have the same lines");

void radio_screen(const esl_screen_t *s)
{
    esl_coproc_screen_t c;
    uint8_t f[ESL_COPROC_FRAME_MAX];
    if (!link.up)
        return;
    for (int i = 0; i < ESL_COPROC_SCREEN_LINES; i++) {
        c.color[i] = s->color[i];
        strncpy(c.text[i], s->text[i], ESL_COPROC_SCREEN_TEXT_MAX);
        c.text[i][ESL_COPROC_SCREEN_TEXT_MAX] = '\0';
    }
    send(f, esl_coproc_encode_screen(f, &c));
}

static void on_hello(const esl_coproc_hello_t *h)
{
    link.last_hello_us = esp_timer_get_time();
    bool ours = h->configured && h->cfg.channel == cfg.channel && h->cfg.pan == cfg.pan &&
                h->cfg.short_addr == cfg.short_addr && h->cfg.tx_power == cfg.tx_power && h->cfg.flags == cfg.flags;
    if (!ours) {
        if (h->version != ESL_COPROC_VERSION)
            ESP_LOGW(TAG, "radio speaks link v%u, this AP v%u", h->version, ESL_COPROC_VERSION);
        ESP_LOGI(TAG, "radio %u.%u (%016llX) %s: sending CONFIG", h->fw >> 8, h->fw & 0xFF,
                 (unsigned long long)h->ieee_addr, h->configured ? "has other settings" : "started");
        link.up = false;
        send_config();
        return;
    }
    if (!link.up)
        ESP_LOGI(TAG, "radio %u.%u up: 802.15.4 AP on channel %u, PAN 0x%04X", h->fw >> 8, h->fw & 0xFF,
                 cfg.channel, cfg.pan);
    link.up = true;
}

static void on_rx(const esl_coproc_rx_t *rx)
{
    esl_radio_event_t ev;
    uint8_t tx[ESL_PSDU_MAX], f[ESL_COPROC_FRAME_MAX];
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;

    stats.rx++;
    size_t len = esl_radio_handle(store, layouts, cfg.pan, rx->psdu, rx->len, rx->rssi, now_ms, tx, &ev);
    if (!len)
        return; /* not a POLL or request for us */
    send(f, esl_coproc_encode_tx(f, rx->ref, tx, len));
    if (ev.msg_type == ESL_MSG_POLL)
        stats.polls++;
    else
        stats.blocks++;

    if (ev.msg_type != ESL_MSG_POLL)
        return; /* blocks: hundreds per download, counted only */
    char m[17];
    esl_mac_format(ev.mac, m);
    const char *reply = ev.reply_type == ESL_MSG_CONFIG   ? "CONFIG"
                        : ev.reply_type == ESL_MSG_FW     ? "FW"
                        : ev.reply_type == ESL_MSG_IMG    ? "IMG"
                                                          : "IDLE";
    ESP_LOGI(TAG, "%s poll id=%08lX fw=%u bat=%u model=%u rssi=%d -> %s", m, (unsigned long)ev.poll.content_id,
             ev.poll.fw_ver, ev.poll.battery_mv, ev.poll.model, rx->rssi, reply);
}

static void on_tx_done(const esl_coproc_tx_done_t *d)
{
    stats.tx_resent += d->resends;
    if (d->rx_to_tx_us > stats.worst_us)
        stats.worst_us = d->rx_to_tx_us;
    switch (d->status) {
    case ESL_COPROC_TX_OK:
        stats.replies++;
        if (d->resends)
            ESP_LOGW(TAG, "reply sent after %u resends", d->resends);
        if (d->rx_to_tx_us > 5000)
            ESP_LOGW(TAG, "slow reply: %u us", d->rx_to_tx_us);
        break;
    case ESL_COPROC_TX_LATE:
        stats.tx_late++;
        ESP_LOGW(TAG, "reply reached the radio after the tag stopped listening");
        break;
    default:
        stats.tx_failed++;
        ESP_LOGW(TAG, "reply not sent (status %u) after %u resends", d->status, d->resends);
        break;
    }
}

static void on_stats(const esl_coproc_stats_t *s)
{
    ESP_LOGI(TAG, "radio: rx %lu tx %lu failed %lu resent %lu late %lu dropped %lu link errors %lu up %lu s",
             (unsigned long)s->rx, (unsigned long)s->tx, (unsigned long)s->tx_failed, (unsigned long)s->tx_resent,
             (unsigned long)s->tx_late, (unsigned long)s->dropped, (unsigned long)s->link_errors,
             (unsigned long)s->uptime_s);
}

static void handle(const esl_coproc_parser_t *p)
{
    switch (p->type) {
    case ESL_COPROC_HELLO: {
        esl_coproc_hello_t h;
        if (esl_coproc_decode_hello(p->payload, p->len, &h))
            on_hello(&h);
        break;
    }
    case ESL_COPROC_RX: {
        static esl_coproc_rx_t rx; /* off the stack: esl_radio_handle needs it */
        if (esl_coproc_decode_rx(p->payload, p->len, &rx))
            on_rx(&rx);
        break;
    }
    case ESL_COPROC_TX_DONE: {
        esl_coproc_tx_done_t d;
        if (esl_coproc_decode_tx_done(p->payload, p->len, &d))
            on_tx_done(&d);
        break;
    }
    case ESL_COPROC_STATS: {
        esl_coproc_stats_t s;
        if (esl_coproc_decode_stats(p->payload, p->len, &s))
            on_stats(&s);
        break;
    }
    default:
        break;
    }
}

static void radio_task(void *arg)
{
    (void)arg;
    static esl_coproc_parser_t parser;
    static uint8_t buf[256];
    uart_event_t ev;
    int64_t last_stats = esp_timer_get_time();

    esl_coproc_parser_init(&parser);
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(uart_events, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
                ESP_LOGW(TAG, "UART overflow");
                uart_flush_input(PORT);
                xQueueReset(uart_events);
            }
            size_t n = 0;
            uart_get_buffered_data_len(PORT, &n);
            while (n) {
                int got = uart_read_bytes(PORT, buf, n < sizeof(buf) ? n : sizeof(buf), 0);
                if (got <= 0)
                    break;
                for (int i = 0; i < got; i++)
                    if (esl_coproc_parser_feed(&parser, buf[i]))
                        handle(&parser);
                n -= (size_t)got;
            }
        }

        int64_t now = esp_timer_get_time();
        if (!link.up && now - link.last_ping_us > PING_EVERY_US) {
            if (link.last_ping_us)
                ESP_LOGW(TAG, "no answer from the radio on UART%d (TX GPIO%d, RX GPIO%d)", PORT,
                         CONFIG_ESL_COPROC_TX_GPIO, CONFIG_ESL_COPROC_RX_GPIO);
            link.last_ping_us = now;
            send_config(); /* it answers with HELLO */
        }
        if (now - last_stats > 60 * 1000000LL) {
            uint8_t f[ESL_COPROC_FRAME_MAX];
            last_stats = now;
            send(f, esl_coproc_frame(f, ESL_COPROC_STATS_Q, NULL, 0));
            ESP_LOGI(TAG, "heap: free %u (internal %u), largest block %u, lowest free %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            ESP_LOGI(TAG, "stats: rx %lu polls %lu blocks %lu replies %lu tx_failed %lu late %lu resent %lu "
                          "worst %lld us, link crc errors %lu",
                     (unsigned long)stats.rx, (unsigned long)stats.polls, (unsigned long)stats.blocks,
                     (unsigned long)stats.replies, (unsigned long)stats.tx_failed, (unsigned long)stats.tx_late,
                     (unsigned long)stats.tx_resent, (long long)stats.worst_us, (unsigned long)parser.bad_crc);
        }
    }
}

esp_err_t radio_start(esl_store_t *st, esl_layouts_t *ls, uint8_t channel, uint16_t pan_id)
{
    store = st;
    layouts = ls;
    cfg = (esl_coproc_config_t){.channel = channel, .pan = pan_id, .short_addr = ESL_AP_SHORT_ADDR,
                              .tx_power = CONFIG_ESL_TX_POWER_DBM, .flags = 0};

    const uart_config_t uc = {
        .baud_rate = ESL_COPROC_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(PORT, 4096, 2048, 16, &uart_events, 0);
    if (err == ESP_OK)
        err = uart_param_config(PORT, &uc);
    if (err == ESP_OK)
        err = uart_set_pin(PORT, CONFIG_ESL_COPROC_TX_GPIO, CONFIG_ESL_COPROC_RX_GPIO, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
        return err;

    /* esl_radio_handle copies one model's layout names (about 0.5 KB) onto the stack */
    if (xTaskCreate(radio_task, "radio", 6144, NULL, configMAX_PRIORITIES - 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "802.15.4 through the radio on UART%d (TX GPIO%d, RX GPIO%d, %d baud), channel %u, PAN 0x%04X",
             PORT, CONFIG_ESL_COPROC_TX_GPIO, CONFIG_ESL_COPROC_RX_GPIO, ESL_COPROC_BAUD, channel, pan_id);
    return ESP_OK;
}
