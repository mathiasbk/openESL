#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "driver/uart.h"
#include "esl_coproc.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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
 * The radio says HELLO when it starts, after each CONFIG and PING, and when it
 * gets a TX before any CONFIG. A HELLO that is not configured (the radio
 * restarted) gets the CONFIG again; while no HELLO comes, CONFIG goes out every
 * couple of seconds. While the link is up a PING goes out every few seconds, and
 * a radio that has said nothing for LINK_DEAD_US is taken as gone (hung, or
 * restarted with its HELLO lost) and configured again.
 *
 * display.c sends the status screen through radio_screen(), for a radio with
 * an LCD (the C6 with a 0.96" screen as the radio).
 *
 * ota.c sends new radio firmware through radio_ota_*() (link v2): OTA_BEGIN,
 * the image in OTA_DATA parts, each after the radio's OTA_ACK for the one
 * before, and OTA_END, after which the radio restarts into it and says HELLO
 * unconfigured, so it gets its CONFIG like any radio that restarted. A part
 * whose OTA_ACK does not come is sent again; the radio answers a part it has
 * already written without writing it twice.
 *
 * The UART task is registered with the task watchdog and wakes at least once
 * a second, like the radio task. */

static const char *TAG = "radio";

#define PORT CONFIG_ESL_COPROC_UART_NUM
#define CONFIG_EVERY_US (2 * 1000000LL) /* while the link is down */
#define PING_EVERY_US   (5 * 1000000LL) /* while it is up */
#define LINK_DEAD_US    (15 * 1000000LL)

static esl_coproc_config_t cfg;
static QueueHandle_t uart_events;

static struct {
    bool up;              /* HELLO with our CONFIG seen */
    bool heard;           /* a HELLO since boot: hello holds it */
    esl_coproc_hello_t hello; /* the last one, for radio_info() */
    int64_t last_heard_us; /* any good frame from the radio */
    int64_t last_ping_us;  /* CONFIG while down, PING while up */
} link;

/* From the UART task's TX_DONE to the radio task, which owns radio_link.stats */
static atomic_uint lost, resent;
static uint32_t late;      /* TX_DONE LATE, for the log */
static uint32_t worst_us;  /* the radio's: from its RX to the first transmit */

/* The OTA_ACK the HTTP task waits for in radio_ota_*(), from the UART task */
static SemaphoreHandle_t ota_ack;
static uint8_t ota_ack_status;
static uint32_t ota_ack_offset;
static uint32_t ota_next; /* the image's next byte to send */
static portMUX_TYPE hello_mux = portMUX_INITIALIZER_UNLOCKED;

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
    taskENTER_CRITICAL(&hello_mux);
    link.hello = *h;
    link.heard = true;
    taskEXIT_CRITICAL(&hello_mux);
    bool ours = h->configured && h->cfg.channel == cfg.channel && h->cfg.pan == cfg.pan &&
                h->cfg.short_addr == cfg.short_addr && h->cfg.tx_power == cfg.tx_power && h->cfg.flags == cfg.flags;
    if (!ours) {
        if (h->version < ESL_COPROC_VERSION_OTA)
            ESP_LOGW(TAG, "radio speaks link v%u, this AP v%u: it works, but takes new firmware only over USB",
                     h->version, ESL_COPROC_VERSION);
        else if (h->version != ESL_COPROC_VERSION)
            ESP_LOGW(TAG, "radio speaks link v%u, this AP v%u", h->version, ESL_COPROC_VERSION);
        ESP_LOGI(TAG, "radio %u.%u %s(%016llX) %s: sending CONFIG", h->fw >> 8, h->fw & 0xFF, h->app_version,
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
    link.last_heard_us = esp_timer_get_time();
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
    case ESL_COPROC_OTA_ACK: {
        uint8_t st;
        uint32_t off;
        if (esl_coproc_decode_ota_ack(p->payload, p->len, &st, &off)) {
            ota_ack_status = st;
            ota_ack_offset = off;
            xSemaphoreGive(ota_ack);
        }
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
    int64_t last_stats = esp_timer_get_time(), last_read_us = 0;

    esl_coproc_parser_init(&parser);
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(uart_events, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
                /* bytes were lost: the frame in progress is broken, what is buffered is not */
                ESP_LOGW(TAG, "UART overflow");
                esl_coproc_parser_gap(&parser);
            }
            if (esp_timer_get_time() - last_read_us > ESL_COPROC_GAP_US)
                esl_coproc_parser_gap(&parser);
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
            last_read_us = esp_timer_get_time();
        }

        int64_t now = esp_timer_get_time();
        if (link.up && now - link.last_heard_us > LINK_DEAD_US) {
            ESP_LOGW(TAG, "the radio has said nothing for %lld s: configuring it again",
                     (long long)((now - link.last_heard_us) / 1000000));
            link.up = false;
            link.last_ping_us = 0;
        }
        if (link.up && now - link.last_ping_us > PING_EVERY_US) {
            uint8_t f[ESL_COPROC_FRAME_MAX];
            link.last_ping_us = now;
            uart_send(f, esl_coproc_frame(f, ESL_COPROC_PING, NULL, 0)); /* it answers with HELLO */
        }
        if (!link.up && now - link.last_ping_us > CONFIG_EVERY_US) {
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
            if (parser.bad_crc || parser.cut)
                ESP_LOGW(TAG, "UART link: %lu frames with a bad CRC, %lu cut off", (unsigned long)parser.bad_crc,
                         (unsigned long)parser.cut);
        }
    }
}

/* ---- new radio firmware ---- */

#define OTA_ACK_MS 500     /* a part; the radio erases a 4 KB sector now and then (~50 ms) */
#define OTA_END_ACK_MS 3000 /* the end: the radio checks the whole image's hash */
#define OTA_TRIES 4

/* Sends a frame and waits for the OTA_ACK, sending it again when none comes:
 * the status, or -1 when the radio did not answer. An OK for less than `expect`
 * bytes answers a part sent twice before this frame, and is skipped */
static int ota_send(const uint8_t *f, size_t n, uint32_t wait_ms, uint32_t expect, uint32_t *offset)
{
    for (int i = 0; i < OTA_TRIES; i++) {
        xSemaphoreTake(ota_ack, 0); /* a late one for the frame before */
        uart_send(f, n);
        int64_t until = esp_timer_get_time() + (int64_t)wait_ms * 1000;
        for (;;) {
            int64_t left_us = until - esp_timer_get_time();
            if (left_us <= 0 || xSemaphoreTake(ota_ack, pdMS_TO_TICKS(left_us / 1000) + 1) != pdTRUE)
                break;
            if (ota_ack_status == ESL_COPROC_OTA_OK && ota_ack_offset < expect)
                continue;
            *offset = ota_ack_offset;
            return ota_ack_status;
        }
        ESP_LOGW(TAG, "radio firmware: no answer from the radio, sending again");
    }
    return -1;
}

/* An OTA_ACK status as the HTTP status and message of the reply */
static int ota_error(int st, char *err, size_t errlen)
{
    static const struct {
        int st, http;
        const char *msg;
    } map[] = {
        {-1, 504, "the radio stopped answering"},
        {ESL_COPROC_OTA_NO_SLOT, 501, "the radio has no second app slot: flash it once over USB with this release's radio image"},
        {ESL_COPROC_OTA_TOO_BIG, 413, "the image does not fit the radio's app slot"},
        {ESL_COPROC_OTA_OFFSET, 409, "the radio lost the upload: send it again"},
        {ESL_COPROC_OTA_FLASH, 500, "writing the radio's flash failed"},
        {ESL_COPROC_OTA_INVALID, 422, "not a valid radio image for the radio's chip"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (map[i].st == st) {
            snprintf(err, errlen, "%s", map[i].msg);
            return map[i].http;
        }
    snprintf(err, errlen, "the radio answered %d", st);
    return 500;
}

int radio_ota_begin(uint32_t size, char *err, size_t errlen)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    uint32_t off;
    taskENTER_CRITICAL(&hello_mux);
    bool heard = link.heard;
    uint8_t version = link.hello.version;
    taskEXIT_CRITICAL(&hello_mux);
    if (!link.up || !ota_ack) {
        snprintf(err, errlen, "the radio is not answering on UART%d: check the wiring (TX GPIO%d, RX GPIO%d)", PORT,
                 CONFIG_ESL_COPROC_TX_GPIO, CONFIG_ESL_COPROC_RX_GPIO);
        return 503;
    }
    if (!heard || version < ESL_COPROC_VERSION_OTA) {
        snprintf(err, errlen, "the radio's firmware is too old to be updated over the link: flash it once over USB");
        return 501;
    }
    int st = ota_send(f, esl_coproc_encode_ota_begin(f, size), OTA_ACK_MS, 0, &off);
    if (st != ESL_COPROC_OTA_OK)
        return ota_error(st, err, errlen);
    ota_next = 0;
    ESP_LOGI(TAG, "radio firmware: sending %lu bytes", (unsigned long)size);
    return 0;
}

int radio_ota_write(const void *data, size_t len, char *err, size_t errlen)
{
    const uint8_t *d = data;
    uint8_t f[ESL_COPROC_FRAME_MAX];
    while (len) {
        size_t n = len < ESL_COPROC_OTA_CHUNK ? len : ESL_COPROC_OTA_CHUNK;
        uint32_t off;
        int st = ota_send(f, esl_coproc_encode_ota_data(f, ota_next, d, n), OTA_ACK_MS, ota_next + n, &off);
        if (st != ESL_COPROC_OTA_OK)
            return ota_error(st, err, errlen);
        if (off != ota_next + n)
            return ota_error(ESL_COPROC_OTA_OFFSET, err, errlen);
        ota_next = off;
        d += n;
        len -= n;
    }
    return 0;
}

int radio_ota_end(char *err, size_t errlen)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    uint32_t off;
    int st = ota_send(f, esl_coproc_frame(f, ESL_COPROC_OTA_END, NULL, 0), OTA_END_ACK_MS, ota_next, &off);
    if (st != ESL_COPROC_OTA_OK)
        return ota_error(st, err, errlen);
    /* The radio restarts: GET /api/ota says up again only on the new firmware's HELLO,
     * not the old one's for up to LINK_DEAD_US */
    link.up = false;
    ESP_LOGI(TAG, "radio firmware: %lu bytes written, the radio restarts into it", (unsigned long)off);
    return 0;
}

void radio_info(esl_sb_t *out)
{
    taskENTER_CRITICAL(&hello_mux);
    bool heard = link.heard;
    esl_coproc_hello_t h = link.hello;
    taskEXIT_CRITICAL(&hello_mux);
    esl_sb_printf(out, "{\"up\":%s", link.up ? "true" : "false");
    if (!heard) {
        esl_sb_puts(out, ",\"version\":null,\"fw\":null,\"link\":null,\"ota\":false,\"display\":null}");
        return;
    }
    esl_sb_puts(out, ",\"version\":");
    if (h.app_version[0])
        esl_sb_json_str(out, h.app_version);
    else
        esl_sb_puts(out, "null");
    bool v2 = h.version >= ESL_COPROC_VERSION_OTA;
    esl_sb_printf(out, ",\"fw\":\"%u.%u\",\"link\":%u,\"ota\":%s,\"display\":", h.fw >> 8, h.fw & 0xFF, h.version,
                  v2 ? "true" : "false");
    esl_sb_puts(out, !v2 ? "null}" : h.features & ESL_COPROC_FEAT_DISPLAY ? "true}" : "false}");
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
    ota_ack = xSemaphoreCreateBinary();
    if (!ota_ack)
        return ESP_ERR_NO_MEM;
    /* above the radio task, which waits for the frames this one reads */
    if (xTaskCreate(uart_task, "radio_uart", 4096, NULL, configMAX_PRIORITIES - 1, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "radio coprocessor on UART%d (TX GPIO%d, RX GPIO%d, %d baud)", PORT, CONFIG_ESL_COPROC_TX_GPIO,
             CONFIG_ESL_COPROC_RX_GPIO, ESL_COPROC_BAUD);
    return ESP_OK;
}

static int link_set_tx_power(esl_link_t *l, int8_t dbm)
{
    (void)l;
    cfg.tx_power = dbm;
    send_config(); /* HELLO with the new power follows */
    return ESP_OK;
}

esl_link_t radio_link = {
    .name = "802.15.4 coprocessor",
    .start = link_start,
    .send = link_send,
    .set_channel = link_set_channel,
    .set_tx_power = link_set_tx_power,
};
