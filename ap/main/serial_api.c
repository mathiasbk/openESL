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
 * A reply is written with one fwrite under the stdout lock, so log lines from
 * other tasks cannot land inside it. Anyone with the USB cable has the device
 * anyway, so serial requests need no token.
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

static void reply(const char *seq, int status, const char *body, size_t len)
{
    flockfile(stdout);
    printf("@ESL %s %d %u ", seq, status, (unsigned)len);
    fwrite(body, 1, len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void handle_line(char *line, char *resp, size_t cap)
{
    char seq[16], method[8], uri[REQ_LINE_MAX];
    unsigned len;
    if (sscanf(line, "@ESL %15s %7s %255s %u", seq, method, uri, &len) != 4) {
        ESP_LOGW(TAG, "bad request line");
        return;
    }

    char *body = NULL;
    if (len > ESL_API_MAX_BODY) {
        /* Swallow the body so the next request line is found again */
        read_body(NULL, len);
        static const char msg[] = "{\"error\":\"body too large\"}";
        reply(seq, 413, msg, sizeof(msg) - 1);
        return;
    }
    if (len) {
        body = malloc(len);
        if (!body) {
            read_body(NULL, len);
            static const char msg[] = "{\"error\":\"out of memory\"}";
            reply(seq, 500, msg, sizeof(msg) - 1);
            return;
        }
        if (!read_body(body, len)) {
            free(body);
            ESP_LOGW(TAG, "%s %s: body stopped arriving", method, uri);
            static const char msg[] = "{\"error\":\"body incomplete\"}";
            reply(seq, 408, msg, sizeof(msg) - 1);
            return;
        }
    }

    esl_sb_t out;
    esl_sb_init(&out, resp, cap);
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    int status = api_handle(&serial_api, method, uri, "Bearer " SERIAL_TOKEN, body, len, now_ms, &out);
    free(body);
    ESP_LOGI(TAG, "%s %s -> %d", method, uri, status);
    reply(seq, status, out.buf, out.len);
}

static void serial_task(void *arg)
{
    (void)arg;
    size_t cap = ESL_API_RESP_SIZE(CONFIG_ESL_MAX_TAGS);
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
    if (xTaskCreate(serial_task, "serial_api", 6144, NULL, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "API on the USB serial port");
    return ESP_OK;
}
