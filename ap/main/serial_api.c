#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* The HTTP API over the USB serial port (USB-Serial-JTAG), for setups without
 * WiFi. The port also carries the log, so requests and replies are framed lines
 * that the host picks out of the stream:
 *
 *   host -> AP:  @ESL <seq> <METHOD> <uri> <len>\n<len bytes body>
 *   AP -> host:  @ESL <seq> <status> <len> <len bytes body>\n
 *
 * A reply that grows with the tag table (GET /api/tags) comes in pieces: every
 * piece but the last has a "+" after its length ("@ESL 7 200 22912+ {..."),
 * and the host joins them. Each piece is written with one fwrite under the
 * stdout lock, so log lines from other tasks cannot land inside it (they can
 * come between pieces). Anyone with the USB cable has the device anyway, so
 * serial requests need no token.
 *
 * The driver drops received bytes when its RX ring buffer is full, so the
 * buffer holds a whole request (the client waits for each reply before sending
 * the next). A body that stops arriving gets 408 after BODY_TIMEOUT_MS, and the
 * reader looks for the next request line.
 *
 * The task is registered with the task watchdog: it waits at most IDLE_WAIT_MS
 * for input before it feeds it again, and a request (body included) takes at
 * most BODY_TIMEOUT_MS plus the time the API needs. */

static const char *TAG = "serial_api";

#define SERIAL_TOKEN "serial"
#define REQ_LINE_MAX 256
#define BODY_TIMEOUT_MS 2000
#define IDLE_WAIT_MS 1000

static esl_api_t serial_api;

/* Bytes read from the driver but not used yet */
static uint8_t in_buf[256];
static size_t in_pos, in_len;

static int get_byte(TickType_t wait)
{
    if (in_pos == in_len) {
        int n = usb_serial_jtag_read_bytes(in_buf, sizeof(in_buf), wait);
        if (n <= 0)
            return -1;
        in_pos = 0;
        in_len = (size_t)n;
    }
    return in_buf[in_pos++];
}

/* Reads exactly n bytes into dst (NULL: discard). False on timeout. */
static bool read_body(char *dst, size_t n)
{
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(BODY_TIMEOUT_MS);
    for (size_t i = 0; i < n; i++) {
        TickType_t now = xTaskGetTickCount();
        int c = get_byte(now < end ? end - now : 0);
        if (c < 0)
            return false;
        if (dst)
            dst[i] = (char)c;
    }
    return true;
}

static void reply(const char *seq, int status, const char *body, size_t len, bool more)
{
    flockfile(stdout);
    printf("@ESL %s %d %u%s ", seq, status, (unsigned)len, more ? "+" : "");
    fwrite(body, 1, len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
}

/* The API's buffer is full: send it as one piece of the reply */
static bool serial_sink(esl_sb_t *sb)
{
    reply(sb->ctx, sb->status, sb->buf, sb->len, true);
    return true;
}

#if CONFIG_ESL_OTA
/* POST /api/ota?offset=N&total=T with one part of the image as the body
 * (ESL_API_MAX_BODY at most, which is what the driver's buffer holds), read
 * straight into flash in pieces of resp's size. GET /api/ota: the info. */
static void handle_ota(const char *seq, const char *method, const char *uri, unsigned len, char *resp, size_t cap)
{
    esl_sb_t out;
    char small[256];
    esl_sb_init(&out, small, sizeof(small));
    int status;
    if (strcmp(method, "GET") == 0) {
        read_body(NULL, len);
        ota_confirm_running();
        status = ota_info(&out);
    } else if (strcmp(method, "POST") != 0) {
        read_body(NULL, len);
        static const char msg[] = "{\"error\":\"method not allowed\"}";
        reply(seq, 405, msg, sizeof(msg) - 1, false);
        return;
    } else {
        size_t offset, total;
        if (!ota_parse_query(uri, &offset, &total) || !total) {
            read_body(NULL, len);
            static const char msg[] = "{\"error\":\"send the image in parts: ?offset=N&total=T\"}";
            reply(seq, 422, msg, sizeof(msg) - 1, false);
            return;
        }
        if (len > ESL_API_MAX_BODY) {
            read_body(NULL, len);
            static const char msg[] = "{\"error\":\"body too large\"}";
            reply(seq, 413, msg, sizeof(msg) - 1, false);
            return;
        }
        status = 200;
        size_t got = 0;
        while (status == 200 && got < len) {
            size_t n = len - got < cap ? len - got : cap;
            if (!read_body(resp, n)) {
                ESP_LOGW(TAG, "POST %s: body stopped arriving", uri);
                static const char msg[] = "{\"error\":\"body incomplete\"}";
                reply(seq, 408, msg, sizeof(msg) - 1, false);
                return;
            }
            esl_sb_init(&out, small, sizeof(small));
            status = ota_part(offset + got, total, resp, n, &out);
            got += n;
        }
        if (got < len)
            read_body(NULL, len - got);
    }
    ESP_LOGI(TAG, "%s %s -> %d", method, uri, status);
    reply(seq, status, out.buf, out.len, false);
}
#endif

static void handle_line(char *line, char *resp, size_t cap)
{
    char seq[16], method[8], uri[REQ_LINE_MAX];
    unsigned len;
    if (sscanf(line, "@ESL %15s %7s %255s %u", seq, method, uri, &len) != 4) {
        ESP_LOGW(TAG, "bad request line");
        return;
    }

#if CONFIG_ESL_OTA
    if (strncmp(uri, "/api/ota", 8) == 0 && (uri[8] == 0 || uri[8] == '?')) {
        handle_ota(seq, method, uri, len, resp, cap);
        return;
    }
#endif

    char *body = NULL;
    if (len > ESL_API_MAX_BODY) {
        /* Swallow the body so the next request line is found again */
        read_body(NULL, len);
        static const char msg[] = "{\"error\":\"body too large\"}";
        reply(seq, 413, msg, sizeof(msg) - 1, false);
        return;
    }
    if (len) {
        body = malloc(len);
        if (!body) {
            read_body(NULL, len);
            static const char msg[] = "{\"error\":\"out of memory\"}";
            reply(seq, 500, msg, sizeof(msg) - 1, false);
            return;
        }
        if (!read_body(body, len)) {
            free(body);
            ESP_LOGW(TAG, "%s %s: body stopped arriving", method, uri);
            static const char msg[] = "{\"error\":\"body incomplete\"}";
            reply(seq, 408, msg, sizeof(msg) - 1, false);
            return;
        }
    }

    esl_sb_t out;
    esl_sb_init(&out, resp, cap);
    esl_sb_set_sink(&out, serial_sink, seq);
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    int status = api_handle(&serial_api, method, uri, "Bearer " SERIAL_TOKEN, body, len, now_ms, &out);
    free(body);
    ESP_LOGI(TAG, "%s %s -> %d", method, uri, status);
    if (out.sent && (status != out.status || out.overflow)) {
        /* Pieces with out.status have gone out already (cannot happen with a
           buffer this size); the host sees a cut body */
        ESP_LOGE(TAG, "%s %s: streamed reply failed (%d)", method, uri, status);
        reply(seq, out.status, "", 0, false);
        return;
    }
    reply(seq, status, out.buf, out.len, false);
}

static void serial_task(void *arg)
{
    (void)arg;
    size_t cap = ESL_API_STREAM_BUF;
    char *resp = malloc(cap);
    static char line[REQ_LINE_MAX + 32];
    if (!resp) {
        ESP_LOGE(TAG, "out of memory");
        vTaskDelete(NULL);
    }
    size_t n = 0;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        esp_task_wdt_reset();
        int c = get_byte(pdMS_TO_TICKS(IDLE_WAIT_MS));
        if (c < 0)
            continue;
        if (c != '\n') {
            if (n < sizeof(line) - 1)
                line[n++] = (char)c;
            continue;
        }
        line[n] = 0;
        n = 0;
        if (strncmp(line, "@ESL ", 5) == 0)
            handle_line(line, resp, cap);
    }
}

esp_err_t serial_api_start(const esl_api_t *api)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = REQ_LINE_MAX + 32 + ESL_API_MAX_BODY;
    cfg.tx_buffer_size = 1024;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK)
        return err;
    /* Bodies are length-counted, so no CR/LF rewriting in either direction */
    /* stdout (log and replies) through the driver too; requests are read
     * from the driver directly, so stdin is not used. */
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    usb_serial_jtag_vfs_use_driver();

    serial_api = *api;
    serial_api.token = SERIAL_TOKEN;
    serial_api.lock = NULL; /* whoever has the cable has the device: no token, no lockout */
    if (xTaskCreate(serial_task, "serial_api", 6144, NULL, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "API on the USB serial port");
    return ESP_OK;
}
