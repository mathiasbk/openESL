#include <stdatomic.h>
#include <string.h>
#include "app.h"
#include "driver/uart.h"
#include "esl_coproc.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "radio.h"
#include "sdkconfig.h"

/* The link through a radio coprocessor on UART (docs/ap-radio-link.md): the
 * driver for radio.c on a chip without 802.15.4 (boards/s3-n16r8-c6). An
 * ESP32-C6 running ap/radio/ forwards every frame it receives as RX; this
 * driver's UART task hands it to radio.c's queue, and send() writes the reply
 * back as TX with the radio's ref. The radio sends it, again while the tag
 * still listens if it has to (it keeps the time from its own RX), and reports
 * TX_DONE. send() returns once the reply is on the UART; a TX_DONE that says it
 * did not go out is counted with esl_link_lost() at the next send().
 *
 * The radio says HELLO when it starts and after each CONFIG. A HELLO that is
 * not configured (the radio restarted) gets the CONFIG again; while no HELLO
 * comes, CONFIG goes out every couple of seconds.
 *
 * display.c sends the status screen through radio_screen(), for a radio with
 * an LCD (the C6 with a 0.96" screen as the radio).
 *
 * The UART task is registered with the task watchdog and wakes at least once
 * a second, like the radio task. */

static const char *TAG = "radio";

#define PORT CONFIG_ESL_COPROC_UART_NUM
#define PING_EVERY_US (2 * 1000000LL)

static esl_coproc_config_t cfg;
static QueueHandle_t uart_events;

static struct {
    bool up;              /* HELLO with our CONFIG seen */
    int64_t last_hello_us;
    int64_t last_ping_us;
} link;

/* From the UART task's TX_DONE to the radio task, which owns radio_link.stats */
static atomic_uint lost, resent;
static uint32_t late;      /* TX_DONE LATE, for the log */
static uint32_t worst_us;  /* the radio's: from its RX to the first transmit */

static void uart_send(const uint8_t *f, size_t n)
{
    if (n)
        uart_write_bytes(PORT, f, n);
}

static void send_config(void)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    uart_send(f, esl_coproc_encode_config(f, &cfg));
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
    uart_send(f, esl_coproc_encode_screen(f, &c));
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
        ESP_LOGI(TAG, "radio %u.%u up on channel %u, PAN 0x%04X", h->fw >> 8, h->fw & 0xFF,
                 cfg.channel, cfg.pan);
    link.up = true;
}

static void on_rx(const esl_coproc_rx_t *rx)
{
    esl_link_rx_t f;
    f.len = rx->len;
    memcpy(f.psdu, rx->psdu, rx->len);
    f.rssi = rx->rssi;
    f.channel = rx->channel;
    f.rx_us = esp_timer_get_time();
    f.ref = rx->ref;
    radio_rx(&f);
}

static void on_tx_done(const esl_coproc_tx_done_t *d)
{
    if (d->resends)
        atomic_fetch_add(&resent, d->resends);
    if (d->rx_to_tx_us > worst_us)
        worst_us = d->rx_to_tx_us;
    switch (d->status) {
    case ESL_COPROC_TX_OK:
        if (d->resends)
            ESP_LOGW(TAG, "reply sent after %u resends", d->resends);
        if (d->rx_to_tx_us > 5000)
            ESP_LOGW(TAG, "slow reply: %u us from the radio's RX to its transmit", d->rx_to_tx_us);
        break;
    case ESL_COPROC_TX_LATE:
        late++;
        atomic_fetch_add(&lost, 1);
        ESP_LOGW(TAG, "reply reached the radio after the tag stopped listening");
        break;
    default:
        atomic_fetch_add(&lost, 1);
        ESP_LOGW(TAG, "reply not sent (status %u) after %u resends", d->status, d->resends);
        break;
    }
}

static void on_stats(const esl_coproc_stats_t *s)
{
    ESP_LOGI(TAG, "radio: rx %lu tx %lu failed %lu resent %lu late %lu dropped %lu link errors %lu up %lu s; "
                  "late here %lu, worst RX to transmit %lu us",
             (unsigned long)s->rx, (unsigned long)s->tx, (unsigned long)s->tx_failed, (unsigned long)s->tx_resent,
             (unsigned long)s->tx_late, (unsigned long)s->dropped, (unsigned long)s->link_errors,
             (unsigned long)s->uptime_s, (unsigned long)late, (unsigned long)worst_us);
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
        static esl_coproc_rx_t rx; /* off the stack */
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

static void uart_task(void *arg)
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
            uart_send(f, esl_coproc_frame(f, ESL_COPROC_STATS_Q, NULL, 0));
            if (parser.bad_crc)
                ESP_LOGW(TAG, "UART link: %lu frames with a bad CRC", (unsigned long)parser.bad_crc);
        }
    }
}

/* ---- the link ---- */

static int link_send(esl_link_t *l, const uint8_t *psdu, size_t len, const esl_link_rx_t *req)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    for (unsigned n = atomic_exchange(&lost, 0); n; n--)
        esl_link_lost(l);
    l->stats.tx_resent += atomic_exchange(&resent, 0);
    if (!link.up)
        return ESP_ERR_INVALID_STATE;
    size_t n = esl_coproc_encode_tx(f, (uint8_t)req->ref, psdu, len);
    if (!n)
        return ESP_ERR_INVALID_SIZE;
    esl_link_sending(l, req, esp_timer_get_time());
    uart_send(f, n);
    return ESP_OK;
}

static int link_set_channel(esl_link_t *l, uint8_t channel)
{
    (void)l;
    cfg.channel = channel;
    send_config(); /* HELLO with the new channel follows */
    return ESP_OK;
}

static int link_start(esl_link_t *l, uint8_t channel, uint16_t pan, uint16_t short_addr, bool promiscuous)
{
    (void)l;
    cfg = (esl_coproc_config_t){.channel = channel, .pan = pan, .short_addr = short_addr,
                                .tx_power = CONFIG_ESL_TX_POWER_DBM,
                                .flags = promiscuous ? ESL_COPROC_CFG_SNIFFER : 0};

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
    /* above the radio task, which waits for the frames this one reads */
    if (xTaskCreate(uart_task, "radio_uart", 4096, NULL, configMAX_PRIORITIES - 1, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "radio coprocessor on UART%d (TX GPIO%d, RX GPIO%d, %d baud)", PORT, CONFIG_ESL_COPROC_TX_GPIO,
             CONFIG_ESL_COPROC_RX_GPIO, ESL_COPROC_BAUD);
    return ESP_OK;
}

esl_link_t radio_link = {
    .name = "802.15.4 coprocessor",
    .start = link_start,
    .send = link_send,
    .set_channel = link_set_channel,
};
