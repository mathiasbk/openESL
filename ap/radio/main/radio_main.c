#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "esl_coproc.h"
#include "esl_lcd.h"
#include "esl_screen.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* 802.15.4 radio coprocessor for an AP host on another chip (an ESP32-S3,
 * boards/s3-n16r8-c6). It knows nothing about tags: it forwards every frame
 * it receives to the host over UART and transmits the frames the host sends
 * back (docs/ap-radio-link.md, components/esl_coproc).
 *
 * The timing that the tag needs stays here: a reply the radio refuses is sent
 * again while the tag still listens (as main/radio_ieee802154.c does on the
 * C6 alone), measured from when this radio received the tag's frame. A reply
 * that comes too late is not sent at all.
 *
 * There is no WiFi on this chip, so 802.15.4 has the radio to itself.
 *
 * On a C6 with a small LCD (boards/c6-lcd, ESL_DISPLAY) the screen shows the
 * AP's status, which the host sends as SCREEN every couple of seconds. */

static const char *TAG = "radio";

#define RADIO_FW_VERSION 0x0100 /* 1.0 */

#define TX_RETRY_US     12000 /* from the request's arrival; the tag listens CFG_RX_TIMEOUT_MS (20 ms) */
#define TX_RETRY_GAP_US 700
#define TX_RETRY_MAX    6

typedef struct {
    uint8_t len;
    uint8_t psdu[ESL_COPROC_PSDU_MAX];
    int8_t rssi;
    uint8_t channel;
    int64_t rx_us;
} rx_item_t;

static QueueHandle_t rx_queue;
static SemaphoreHandle_t tx_idle;
static SemaphoreHandle_t uart_lock;
static volatile int tx_err = -1;
static uint8_t tx_buf[1 + ESL_COPROC_PSDU_MAX];

static esl_coproc_config_t cfg;
static bool configured;
static int64_t rx_time[256]; /* arrival of the RX frame with each ref */
static uint8_t next_ref;
static esl_coproc_stats_t stats;

#if CONFIG_ESL_DISPLAY
#define SCREEN_STALE_MS 10000 /* no SCREEN for this long: say the AP is gone */
static QueueHandle_t screen_box; /* one esl_coproc_screen_t, the newest */
#endif

/* ---- driver callbacks (ISR) ---- */

void IRAM_ATTR esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    rx_item_t it;
    BaseType_t woken = pdFALSE;
    it.len = frame[0] > ESL_COPROC_PSDU_MAX ? ESL_COPROC_PSDU_MAX : frame[0];
    memcpy(it.psdu, &frame[1], it.len);
    it.rssi = info->rssi;
    it.channel = info->channel;
    it.rx_us = esp_timer_get_time();
    esp_ieee802154_receive_handle_done(frame);
    if (xQueueSendFromISR(rx_queue, &it, &woken) != pdTRUE)
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
    tx_err = (int)error;
    xSemaphoreGiveFromISR(tx_idle, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---- link ---- */

static void link_send(const uint8_t *f, size_t n)
{
    if (!n)
        return;
    xSemaphoreTake(uart_lock, portMAX_DELAY);
    uart_write_bytes(CONFIG_ESL_COPROC_UART_NUM, f, n);
    xSemaphoreGive(uart_lock);
}

static void send_hello(void)
{
    uint8_t f[ESL_COPROC_FRAME_MAX], mac[8] = {0};
    esl_coproc_hello_t h = {.version = ESL_COPROC_VERSION, .configured = configured, .fw = RADIO_FW_VERSION, .cfg = cfg};
    esp_read_mac(mac, ESP_MAC_IEEE802154);
    for (int i = 0; i < 8; i++)
        h.ieee_addr = h.ieee_addr << 8 | mac[i];
    link_send(f, esl_coproc_encode_hello(f, &h));
}

static void apply_config(const esl_coproc_config_t *c)
{
    esp_ieee802154_set_channel(c->channel);
    esp_ieee802154_set_txpower(c->tx_power);
    esp_ieee802154_set_panid(c->pan);
    esp_ieee802154_set_short_address(c->short_addr);
    esp_ieee802154_set_promiscuous(c->flags & ESL_COPROC_CFG_SNIFFER);
    esp_ieee802154_set_rx_when_idle(true);
    esp_ieee802154_receive();
    cfg = *c;
    configured = true;
    ESP_LOGI(TAG, "channel %u, PAN 0x%04X, short 0x%04X, %d dBm%s", c->channel, c->pan, c->short_addr,
             c->tx_power, c->flags & ESL_COPROC_CFG_SNIFFER ? ", sniffer" : "");
}

/* Sends psdu, again while the tag listens if the radio refuses it. */
static void transmit(uint8_t ref, const uint8_t *psdu, size_t len)
{
    esl_coproc_tx_done_t d = {.ref = ref, .status = ESL_COPROC_TX_OK, .rx_to_tx_us = 0xFFFF};
    int64_t rx_us = ref == ESL_COPROC_REF_NONE ? esp_timer_get_time() : rx_time[ref];
    int64_t age = esp_timer_get_time() - rx_us;
    uint8_t f[ESL_COPROC_FRAME_MAX];

    if (age > TX_RETRY_US) {
        d.status = ESL_COPROC_TX_LATE;
        stats.tx_late++;
        link_send(f, esl_coproc_encode_tx_done(f, &d));
        return;
    }
    if (age < 0xFFFF)
        d.rx_to_tx_us = (uint16_t)age;
    if (xSemaphoreTake(tx_idle, pdMS_TO_TICKS(20)) != pdTRUE) {
        d.status = ESL_COPROC_TX_BUSY;
        stats.tx_failed++;
        link_send(f, esl_coproc_encode_tx_done(f, &d));
        return;
    }
    tx_buf[0] = (uint8_t)len;
    memcpy(&tx_buf[1], psdu, len);
    for (;;) {
        tx_err = -1;
        if (esp_ieee802154_transmit(tx_buf, false) != ESP_OK) {
            d.status = ESL_COPROC_TX_FAILED;
            xSemaphoreGive(tx_idle);
            break;
        }
        if (xSemaphoreTake(tx_idle, pdMS_TO_TICKS(20)) != pdTRUE) {
            d.status = ESL_COPROC_TX_FAILED;
            xSemaphoreGive(tx_idle);
            break;
        }
        if (tx_err < 0) {
            xSemaphoreGive(tx_idle);
            break;
        }
        if (d.resends >= TX_RETRY_MAX || esp_timer_get_time() + TX_RETRY_GAP_US - rx_us > TX_RETRY_US) {
            d.status = ESL_COPROC_TX_FAILED;
            xSemaphoreGive(tx_idle);
            break;
        }
        esp_rom_delay_us(TX_RETRY_GAP_US);
        d.resends++;
        stats.tx_resent++;
    }
    if (d.status == ESL_COPROC_TX_OK)
        stats.tx++;
    else
        stats.tx_failed++;
    link_send(f, esl_coproc_encode_tx_done(f, &d));
}

static void handle_message(const esl_coproc_parser_t *p)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    switch (p->type) {
    case ESL_COPROC_PING:
        send_hello();
        break;
    case ESL_COPROC_CONFIG: {
        esl_coproc_config_t c;
        if (esl_coproc_decode_config(p->payload, p->len, &c))
            apply_config(&c);
        else
            stats.link_errors++;
        send_hello();
        break;
    }
    case ESL_COPROC_TX: {
        uint8_t ref;
        const uint8_t *psdu;
        size_t len;
        if (!esl_coproc_decode_tx(p->payload, p->len, &ref, &psdu, &len)) {
            stats.link_errors++;
            break;
        }
        if (!configured) {
            /* the host thinks the link is up, but this radio restarted and its
               HELLO was lost: say so, and the host sends CONFIG */
            stats.link_errors++;
            send_hello();
            break;
        }
        transmit(ref, psdu, len);
        break;
    }
    case ESL_COPROC_SCREEN: {
#if CONFIG_ESL_DISPLAY
        static esl_coproc_screen_t sc;
        if (esl_coproc_decode_screen(p->payload, p->len, &sc))
            xQueueOverwrite(screen_box, &sc); /* drawn by screen_task, not in the way of TX */
        else
            stats.link_errors++;
#endif
        break;
    }
    case ESL_COPROC_STATS_Q:
        stats.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        link_send(f, esl_coproc_encode_stats(f, &stats));
        break;
    default:
        stats.link_errors++;
        break;
    }
}

/* Host -> radio: commands, TX first of all */
static void uart_task(void *arg)
{
    (void)arg;
    static esl_coproc_parser_t parser;
    uint8_t buf[128];
    int64_t last_read_us = 0;
    esl_coproc_parser_init(&parser);
    for (;;) {
        int n = uart_read_bytes(CONFIG_ESL_COPROC_UART_NUM, buf, 1, portMAX_DELAY);
        if (n <= 0)
            continue;
        if (esp_timer_get_time() - last_read_us > ESL_COPROC_GAP_US)
            esl_coproc_parser_gap(&parser); /* the host restarted mid-frame */
        size_t more = 0;
        uart_get_buffered_data_len(CONFIG_ESL_COPROC_UART_NUM, &more);
        if (more > sizeof(buf) - 1)
            more = sizeof(buf) - 1;
        if (more)
            n += uart_read_bytes(CONFIG_ESL_COPROC_UART_NUM, &buf[1], more, 0);
        for (int i = 0; i < n; i++) {
            uint32_t bad = parser.bad_crc + parser.too_long;
            if (esl_coproc_parser_feed(&parser, buf[i]))
                handle_message(&parser);
            else if (parser.bad_crc + parser.too_long != bad)
                stats.link_errors++;
        }
        last_read_us = esp_timer_get_time();
    }
}

/* Radio -> host: every received frame */
static void rx_task(void *arg)
{
    (void)arg;
    rx_item_t it;
    uint8_t f[ESL_COPROC_FRAME_MAX];
    for (;;) {
        if (xQueueReceive(rx_queue, &it, portMAX_DELAY) != pdTRUE)
            continue;
        stats.rx++;
        esl_coproc_rx_t rx = {.ref = next_ref, .rssi = it.rssi, .channel = it.channel, .len = it.len};
        rx_time[next_ref] = it.rx_us;
        next_ref = next_ref + 1 == ESL_COPROC_REF_NONE ? 0 : next_ref + 1;
        memcpy(rx.psdu, it.psdu, it.len);
        link_send(f, esl_coproc_encode_rx(f, &rx));
    }
}

#if CONFIG_ESL_DISPLAY
static void screen_lines(esl_screen_t *s, const char *a, const char *b, const char *c)
{
    memset(s, 0, sizeof(*s));
    strncpy(s->text[0], "OpenESL radio", ESL_SCREEN_LINE_MAX - 1);
    strncpy(s->text[1], a, ESL_SCREEN_LINE_MAX - 1);
    strncpy(s->text[2], b, ESL_SCREEN_LINE_MAX - 1);
    strncpy(s->text[3], c, ESL_SCREEN_LINE_MAX - 1);
    s->color[0] = ESL_SCREEN_TITLE;
    s->color[1] = ESL_SCREEN_TEXT;
    s->color[2] = ESL_SCREEN_WARN;
    s->color[3] = ESL_SCREEN_DIM;
}

/* Shows the host's SCREEN, or, while none comes, that the radio waits for it */
static void screen_task(void *arg)
{
    (void)arg;
    static esl_coproc_screen_t sc;
    static esl_screen_t s;
    char pins[ESL_SCREEN_LINE_MAX];
    snprintf(pins, sizeof(pins), "UART TX %d RX %d", CONFIG_ESL_COPROC_TX_GPIO, CONFIG_ESL_COPROC_RX_GPIO);
    bool shown = false;
    for (;;) {
        if (xQueueReceive(screen_box, &sc, pdMS_TO_TICKS(SCREEN_STALE_MS)) == pdTRUE) {
            memset(&s, 0, sizeof(s));
            for (int i = 0; i < ESL_SCREEN_LINES; i++) {
                strncpy(s.text[i], sc.text[i], ESL_SCREEN_LINE_MAX - 1);
                s.color[i] = sc.color[i];
            }
            shown = true;
        } else {
            screen_lines(&s, configured ? "AP silent" : "waiting for the AP", shown ? "no status lately" : "",
                         pins);
        }
        esl_lcd_show(&s);
    }
}

static void screen_start(void)
{
    static esl_screen_t s;
    screen_box = xQueueCreate(1, sizeof(esl_coproc_screen_t));
    screen_lines(&s, "802.15.4 radio", "waiting for the AP", "");
    esp_err_t err = screen_box ? esl_lcd_start(&s) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "screen: %s", esp_err_to_name(err)); /* the radio works without it */
        return;
    }
    xTaskCreate(screen_task, "screen", 3072, NULL, 2, NULL);
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "OpenESL 802.15.4 radio %d.%d, link v%d on UART%d (TX GPIO%d, RX GPIO%d, %d baud)",
             RADIO_FW_VERSION >> 8, RADIO_FW_VERSION & 0xFF, ESL_COPROC_VERSION, CONFIG_ESL_COPROC_UART_NUM,
             CONFIG_ESL_COPROC_TX_GPIO, CONFIG_ESL_COPROC_RX_GPIO, ESL_COPROC_BAUD);

    const uart_config_t uc = {
        .baud_rate = ESL_COPROC_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESL_COPROC_UART_NUM, 2048, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_ESL_COPROC_UART_NUM, &uc));
    ESP_ERROR_CHECK(uart_set_pin(CONFIG_ESL_COPROC_UART_NUM, CONFIG_ESL_COPROC_TX_GPIO, CONFIG_ESL_COPROC_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    rx_queue = xQueueCreate(16, sizeof(rx_item_t));
    tx_idle = xSemaphoreCreateBinary();
    uart_lock = xSemaphoreCreateMutex();
    if (!rx_queue || !tx_idle || !uart_lock)
        abort();
    xSemaphoreGive(tx_idle);

    /* Receiving starts with the host's CONFIG, which says the channel and PAN */
    ESP_ERROR_CHECK(esp_ieee802154_enable());

#if CONFIG_ESL_DISPLAY
    screen_start(); /* before the link task, which hands it SCREEN */
#endif
    /* the host's TX before more RX: the tag that sent the last frame listens only briefly */
    xTaskCreate(uart_task, "link", 4096, NULL, configMAX_PRIORITIES - 2, NULL);
    xTaskCreate(rx_task, "rx", 3072, NULL, configMAX_PRIORITIES - 3, NULL);
    send_hello(); /* tells a running host that the radio (re)started and needs CONFIG */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGI(TAG, "rx %lu tx %lu failed %lu resent %lu late %lu dropped %lu link errors %lu",
                 (unsigned long)stats.rx, (unsigned long)stats.tx, (unsigned long)stats.tx_failed,
                 (unsigned long)stats.tx_resent, (unsigned long)stats.tx_late, (unsigned long)stats.dropped,
                 (unsigned long)stats.link_errors);
    }
}
