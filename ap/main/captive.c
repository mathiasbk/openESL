#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

/* Captive portal for the setup network (ESL_WIFI_CAPTIVE): a DNS server on
 * 192.168.4.1 that answers every name with 192.168.4.1, so a phone that joins
 * "openesl-XXXX" finds its connectivity probe (connectivitycheck.gstatic.com,
 * captive.apple.com, msftconnecttest.com, ...) answered by the AP. http_api.c
 * then redirects the probe to the setup form, which makes the phone show its
 * "sign in to network" page with the form in it. The server runs only while
 * the setup network is open. */

static const char *TAG = "captive";

#define DNS_PORT 53
#define DNS_MAX 512
#define SETUP_IP "192.168.4.1"

static volatile bool running;
static TaskHandle_t task;

/* Length of the question section (name, type, class) at q, or 0 if malformed. */
static size_t question_len(const uint8_t *q, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t l = q[i];
        if (l == 0) {
            i++;
            break;
        }
        if (l & 0xC0) /* a compression pointer: two bytes, and the name ends */
            return i + 2 + 4 <= len ? i + 2 + 4 : 0;
        i += 1 + l;
    }
    return i + 4 <= len ? i + 4 : 0;
}

/* Builds the answer to one query in place; returns its length, 0 to ignore. */
static size_t answer(uint8_t *buf, size_t len)
{
    if (len < 12 || (buf[2] & 0x80))       /* too short, or itself a response */
        return 0;
    if (buf[4] != 0 || buf[5] != 1)        /* exactly one question */
        return 0;
    size_t ql = question_len(buf + 12, len - 12);
    if (!ql)
        return 0;
    uint16_t qtype = (uint16_t)((buf[12 + ql - 4] << 8) | buf[12 + ql - 3]);
    size_t out = 12 + ql;
    buf[2] = (uint8_t)(0x84 | (buf[2] & 0x01)); /* response, authoritative, RD copied */
    buf[3] = 0x80;                              /* RA, no error */
    buf[6] = buf[7] = 0;                        /* answers, filled in below */
    buf[8] = buf[9] = buf[10] = buf[11] = 0;    /* no authority, no additional */
    if (qtype == 1 && out + 16 <= DNS_MAX) {    /* A: name pointer, type, class, TTL, length, address */
        static const uint8_t rr[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
        memcpy(buf + out, rr, sizeof(rr));
        out += sizeof(rr);
        buf[7] = 1;
    }
    return out;
}

static void captive_task(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        ESP_LOGE(TAG, "no socket for the DNS server");
        goto out;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(DNS_PORT)};
    addr.sin_addr.s_addr = inet_addr(SETUP_IP);
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS server cannot bind " SETUP_IP ":%d", DNS_PORT);
        close(s);
        goto out;
    }
    ESP_LOGI(TAG, "DNS server for the setup network answers every name with " SETUP_IP);
    uint8_t *buf = malloc(DNS_MAX);
    if (!buf) {
        close(s);
        goto out;
    }
    while (running) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, DNS_MAX, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0)
            continue; /* a timeout: look at the flag again */
        size_t len = answer(buf, (size_t)n);
        if (len)
            sendto(s, buf, len, 0, (struct sockaddr *)&from, fl);
    }
    free(buf);
    close(s);
    ESP_LOGI(TAG, "DNS server stopped");
out:
    task = NULL;
    vTaskDelete(NULL);
}

void captive_dns_start(void)
{
    if (task)
        return;
    running = true;
    if (xTaskCreate(captive_task, "captive_dns", 3072, NULL, 4, &task) != pdPASS) {
        task = NULL;
        ESP_LOGE(TAG, "no memory for the DNS server task");
    }
}

void captive_dns_stop(void)
{
    running = false; /* the task closes its socket and ends within a second */
}
