#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esl_link.h"
#include "esl_nlink.h"
#include "esl_rcache.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <fcntl.h>
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "radio.h"
#include "sdkconfig.h"

/* The radio over WiFi (docs/ap-netradio.md): an ESP32-C6 on the same WiFi network as the
 * AP, which answers the tags itself from a cache (esl_rcache) and syncs with the AP over
 * TCP (esl_nlink) when no tag is mid-exchange.
 *
 *   radio task  every frame from the 802.15.4 driver (../main/radio_ieee802154.c, as on
 *               the AP) goes through esl_rcache_handle(), and the reply goes out at once
 *   net task    joins the AP (the address set in the console, or mDNS _openesl-ap._tcp),
 *               says HELLO, and syncs when esl_rcache_sync_due() says so: the SYNC and
 *               the log go out, the answer is applied to the cache
 *   console     on the USB port: ssid, pass, host, status, reboot
 *   web         port 80 has no page of its own: a browser that opens the radio is sent to
 *               the AP's dashboard, or gets a short page while the radio has not found the AP
 *
 * The cache is the two tasks' only shared state, under one mutex that is held only to
 * answer a frame or to apply what came from the AP: the radio task never waits for the
 * network. The first version is not secure: plain TCP, and the radio takes what any
 * device that answers as the AP sends (pairing and TLS-PSK are the next step). */

static const char *TAG = "netradio";

#define TAGS     CONFIG_ESL_NETRADIO_TAGS
#define IMAGES   CONFIG_ESL_NETRADIO_IMAGES
#define LOG      CONFIG_ESL_NETRADIO_LOG
#define ANSWER_TIMEOUT_MS 10000   /* a sync whose answer has not ended by then: the link is gone */
#define SYNC_TIMEOUT_S    5       /* send and connect */
#define WEB_REQUEST_US    3000000 /* a browser's request on port 80, whole */

static esl_rcache_t rc;
static esl_rtag_t *tags;
static esl_rimage_t images[IMAGES];
static esl_rlog_t *rlog;
static SemaphoreHandle_t lock;
static QueueHandle_t rx_queue;

static const esp_partition_t *fwpart;
static uint8_t channel;
static uint16_t pan;

static EventGroupHandle_t net_events;
#define GOT_IP BIT0

static char ssid[33], pass[65], host[64];
static char ap_ip[16];   /* the AP last connected to, for the web redirect; under the lock */

/* ---- settings in NVS ("cfg", as on the AP) ---- */

static void cfg_get(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK)
        return;
    size_t n = cap;
    if (nvs_get_str(h, key, out, &n) != ESP_OK)
        out[0] = '\0';
    nvs_close(h);
}

static void cfg_set(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK)
        return;
    if (val[0])
        nvs_set_str(h, key, val);
    else
        nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

static void cfg_get_radio(void)
{
    nvs_handle_t h;
    channel = CONFIG_ESL_CHANNEL;
    pan = CONFIG_ESL_PAN_ID;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK)
        return;
    nvs_get_u8(h, "channel", &channel);
    nvs_get_u16(h, "pan", &pan);
    nvs_close(h);
    if (channel < 11 || channel > 26)
        channel = CONFIG_ESL_CHANNEL; /* the driver asserts on a channel it does not have */
}

static void cfg_save_radio(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, "channel", channel);
    nvs_set_u16(h, "pan", pan);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- the radio ---- */

bool IRAM_ATTR radio_rx_from_isr(const esl_link_rx_t *f)
{
    BaseType_t woken = pdFALSE;
    bool ok = xQueueSendFromISR(rx_queue, f, &woken) == pdTRUE;
    if (!ok)
        radio_link.stats.dropped++;
    portYIELD_FROM_ISR(woken);
    return ok;
}

bool radio_rx(const esl_link_rx_t *f)
{
    return xQueueSend(rx_queue, f, 0) == pdTRUE;
}

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

/* A new channel or PAN from the AP: set by the net task, applied by the radio task between
   replies (retuning restarts reception, which would cut off a reply on its way) */
static volatile bool retune, save_radio;

static void radio_task(void *arg)
{
    (void)arg;
    esl_link_rx_t f;
    static uint8_t tx[ESL_PSDU_MAX];
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        esp_task_wdt_reset();
        if (retune) {
            retune = false;
            esp_ieee802154_set_panid(pan);
            radio_link.set_channel(&radio_link, channel);
            ESP_LOGI(TAG, "802.15.4 now channel %u, PAN 0x%04X", channel, pan);
        }
        if (xQueueReceive(rx_queue, &f, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        radio_link.stats.rx++;
        radio_link.stats.last_us = -1;
        xSemaphoreTake(lock, portMAX_DELAY);
        size_t n = esl_rcache_handle(&rc, f.psdu, f.len, f.rssi, now_ms(), tx);
        xSemaphoreGive(lock);
        if (!n)
            continue;
        if (radio_link.send(&radio_link, tx, n, &f) == 0)
            radio_link.stats.replies++;
        else
            radio_link.stats.tx_failed++;
        if (radio_link.stats.last_us > radio_link.stats.worst_us)
            radio_link.stats.worst_us = radio_link.stats.last_us;
        if (radio_link.stats.last_us > 5000)
            ESP_LOGW(TAG, "slow reply: %lld us", (long long)radio_link.stats.last_us);
    }
}

/* The AP's channel and PAN (CONFIG, checked by esl_nlink), kept for the next start: the
   radio task retunes, the net task saves them once the cache's lock is free */
static void on_config(void *ctx, uint8_t ch, uint16_t p)
{
    (void)ctx;
    if (ch == channel && p == pan)
        return;
    channel = ch;
    pan = p;
    retune = true;
    save_radio = true;
}

/* The AP's transmit power (in its CONFIG): the driver takes it at the next transmit. Not
   kept: until the AP is back after a restart, menuconfig's applies. */
static void on_tx_power(void *ctx, int8_t dbm)
{
    (void)ctx;
    static int8_t now = CONFIG_ESL_TX_POWER_DBM;
    if (dbm == now)
        return;
    now = dbm;
    radio_link.set_tx_power(&radio_link, dbm);
    ESP_LOGI(TAG, "802.15.4 transmit power now %d dBm", dbm);
}

/* Tag firmware in the tagfw partition, one ESL_TAGFW_SLOT per model: its bytes only (the
   AP sends no header). Each 4 KB sector is erased as the first part reaches it. */
static bool fw_slot(uint8_t model, size_t off, size_t n, size_t *at)
{
    size_t base = (size_t)(model - 1) * ESL_TAGFW_SLOT;
    if (!fwpart || !model || base + ESL_TAGFW_SLOT > fwpart->size || off + n > ESL_TAGFW_SLOT)
        return false;
    *at = base + off;
    return true;
}

/* Called by the net task with the cache's lock held (esl_nlink_feed): the lock is let go
   while the flash is written, so the radio task can answer tags in between. The slot is not
   read meanwhile: the cache offers this model's firmware only once all of it is in. */
static bool write_fw(void *ctx, uint8_t model, uint32_t off, const uint8_t *d, size_t n)
{
    (void)ctx;
    size_t at;
    if (!fw_slot(model, off, n, &at))
        return false;
    xSemaphoreGive(lock);
    bool ok = true;
    size_t sec = fwpart->erase_size;
    for (size_t s = (at + sec - 1) / sec * sec; ok && s < at + n; s += sec)
        ok = esp_partition_erase_range(fwpart, s, sec) == ESP_OK;
    ok = ok && esp_partition_write(fwpart, at, d, n) == ESP_OK;
    xSemaphoreTake(lock, portMAX_DELAY);
    return ok;
}

static bool read_fw(void *ctx, uint8_t model, size_t off, uint8_t *buf, size_t n)
{
    (void)ctx;
    size_t at;
    return fw_slot(model, off, n, &at) && esp_partition_read(fwpart, at, buf, n) == ESP_OK;
}

/* ---- the link to the AP ---- */

static esl_nlink_radio_t nl;
static esl_nlink_rx_t nrx;
static uint8_t *nrx_buf;
static int sock = -1;
static uint8_t *txb;     /* what goes to the AP, built under the lock and sent outside it */
static size_t txn, txcap;
static uint32_t connects, timeouts;
static int64_t sync_us_last, sync_us_worst;

static bool buf_write(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (txn + n > txcap)
        return false;
    memcpy(txb + txn, d, n);
    txn += n;
    return true;
}

static bool send_txb(void)
{
    for (size_t at = 0; at < txn;) {
        int k = send(sock, txb + at, txn - at, 0);
        if (k <= 0)
            return false;
        at += (size_t)k;
    }
    return true;
}

static bool on_frame(void *ctx, uint8_t type, const uint8_t *p, size_t len)
{
    (void)ctx;
    return esl_nlink_radio_on(&nl, type, p, len);
}

static void disconnect(const char *why)
{
    if (sock < 0)
        return;
    ESP_LOGW(TAG, "link to the AP closed: %s", why);
    close(sock);
    sock = -1;
}

/* The AP's address: the one set in the console (host[:port]) with by_host, else mDNS */
static bool find_ap(struct sockaddr_in *a, bool by_host)
{
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(CONFIG_ESL_NETRADIO_PORT);
    if (by_host) {
        char h[64];
        xSemaphoreTake(lock, portMAX_DELAY); /* the console may change it */
        strlcpy(h, host, sizeof(h));
        xSemaphoreGive(lock);
        char *colon = strchr(h, ':');
        if (colon) {
            *colon = '\0';
            a->sin_port = htons((uint16_t)atoi(colon + 1));
        }
        return inet_aton(h, &a->sin_addr) != 0;
    }
    /* asking for a unicast answer (QU): a multicast one goes out at the lowest rate and
       reaches a station in power save only at a DTIM beacon, and was missed on the C6 */
    mdns_result_t *res = NULL;
    if (mdns_query_generic(NULL, "_openesl-ap", "_tcp", MDNS_TYPE_PTR, MDNS_QUERY_UNICAST, 3000, 4, &res) != ESP_OK ||
        !res) {
        ESP_LOGW(TAG, "no AP found on mDNS (_openesl-ap._tcp)%s", host[0] ? "" : "; set one with: host <address>");
        return false;
    }
    bool found = false;
    for (mdns_result_t *r = res; r && !found; r = r->next)
        for (mdns_ip_addr_t *ip = r->addr; ip && !found; ip = ip->next)
            if (ip->addr.type == ESP_IPADDR_TYPE_V4) {
                a->sin_addr.s_addr = ip->addr.u_addr.ip4.addr;
                a->sin_port = htons(r->port);
                found = true;
                ESP_LOGI(TAG, "AP %s on mDNS", r->hostname ? r->hostname : "?");
            }
    if (!found)
        ESP_LOGW(TAG, "AP found on mDNS, but without an IPv4 address");
    mdns_query_results_free(res);
    return found;
}

static uint16_t version_number(void)
{
    unsigned major = 0, minor = 0, patch = 0;
    sscanf(esp_app_get_description()->version, "%u.%u.%u", &major, &minor, &patch);
    return (uint16_t)(major * 10000 + minor * 100 + patch);
}

/* A connection to the AP found by_host or on mDNS, -1 when there is none */
static int open_ap(bool by_host, struct sockaddr_in *a, char *ip)
{
    if (!find_ap(a, by_host))
        return -1;
    inet_ntoa_r(a->sin_addr, ip, 16);
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0)
        return -1;
    struct timeval tv = {SYNC_TIMEOUT_S, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* at most SYNC_TIMEOUT_S, not lwIP's SYN retries (half a minute and more): an address
       that does not answer gives way to mDNS soon */
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int err = 0;
    int rc_conn = connect(s, (struct sockaddr *)a, sizeof(*a));
    if (rc_conn && errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval ctv = {SYNC_TIMEOUT_S, 0};
        socklen_t el = sizeof(err);
        if (select(s + 1, NULL, &w, NULL, &ctv) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) || err)
            err = err ? err : ETIMEDOUT;
    } else if (rc_conn) {
        err = errno;
    }
    if (err) {
        ESP_LOGW(TAG, "AP %s:%u does not answer", ip, ntohs(a->sin_port));
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, flags);
    return s;
}

static bool connect_ap(void)
{
    struct sockaddr_in a;
    char ip[16];
    /* the address set by hand first; when it does not answer (the AP got another one from
       the router, say), mDNS */
    xSemaphoreTake(lock, portMAX_DELAY);
    bool by_host = host[0] != 0;
    xSemaphoreGive(lock);
    int s = by_host ? open_ap(true, &a, ip) : -1;
    if (s < 0)
        s = open_ap(false, &a, ip);
    if (s < 0)
        return false;
    sock = s;
    connects++;
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(ap_ip, ip, sizeof(ap_ip));
    xSemaphoreGive(lock);
    esl_nlink_rx_init(&nrx, nrx_buf, rc.image_cap + 64);
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    uint64_t id = 0;
    for (int i = 0; i < 6; i++)
        id = id << 8 | mac[i];
    unsigned slots = fwpart ? fwpart->size / ESL_TAGFW_SLOT : 0;
    esl_nlink_hello_t h = {rc.boot, version_number(), id, TAGS, IMAGES, (uint32_t)rc.image_cap,
                           (uint8_t)(slots < ESL_RCACHE_MODELS ? slots : ESL_RCACHE_MODELS),
                           ESL_NLINK_F_CHANNEL};
    xSemaphoreTake(lock, portMAX_DELAY);
    txn = 0;
    bool ok = esl_nlink_radio_hello(&nl, &h);
    xSemaphoreGive(lock);
    ok = ok && send_txb();
    ESP_LOGI(TAG, "connected to the AP at %s:%u%s", ip, ntohs(a.sin_port), ok ? "" : ", but HELLO failed");
    if (!ok)
        disconnect("HELLO not sent");
    return ok;
}

static void net_task(void *arg)
{
    (void)arg;
    static uint8_t buf[1460];
    uint64_t last_sync = 0, sync_start = 0;
    unsigned backoff_s = 2;
    for (;;) {
        xEventGroupWaitBits(net_events, GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
        if (sock < 0) {
            if (!connect_ap()) {
                vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
                backoff_s = backoff_s < 30 ? backoff_s * 2 : 30;
                continue;
            }
            backoff_s = 2;
            last_sync = now_ms() + 1; /* sync at once */
        }

        uint64_t now = now_ms();
        if (!nl.in_sync) {
            xSemaphoreTake(lock, portMAX_DELAY);
            bool due = esl_rcache_sync_due(&rc, now, last_sync);
            bool ok = true;
            if (due) {
                txn = 0;
                ok = esl_nlink_radio_sync(&nl, now);
            }
            xSemaphoreGive(lock);
            if (due) {
                sync_start = now;
                if (!ok || !send_txb()) {
                    disconnect(ok ? "send failed" : "sync too large for the buffer");
                    continue;
                }
            }
        } else if (now - sync_start > ANSWER_TIMEOUT_MS) {
            timeouts++;
            disconnect("no answer");
            continue;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        struct timeval tv = {0, nl.in_sync ? 5000 : 20000};
        if (select(sock + 1, &fds, NULL, NULL, &tv) <= 0)
            continue;
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            disconnect(n == 0 ? "closed by the AP" : "receive failed");
            continue;
        }
        uint32_t ends = nl.ends;
        xSemaphoreTake(lock, portMAX_DELAY);
        bool ok = esl_nlink_feed(&nrx, buf, (size_t)n, on_frame, NULL);
        xSemaphoreGive(lock);
        if (save_radio) {
            save_radio = false;
            cfg_save_radio();
        }
        if (!ok) {
            disconnect("bad frame from the AP");
            continue;
        }
        if (nl.ends != ends) {
            last_sync = now_ms();
            sync_us_last = (int64_t)(last_sync - sync_start) * 1000;
            if (sync_us_last > sync_us_worst)
                sync_us_worst = sync_us_last;
            ESP_LOGD(TAG, "sync %lu in %lld ms", (unsigned long)nl.syncs, (long long)sync_us_last / 1000);
        }
    }
}

/* ---- web: to the AP's dashboard ---- */

/* The AP's address for a browser: the AP last connected to, else the one set by hand
   (without its port: that is the link's, the dashboard is on 80); empty when neither */
static void dashboard_ip(char *out, size_t cap)
{
    char h[64];
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(h, ap_ip[0] ? ap_ip : host, sizeof(h));
    xSemaphoreGive(lock);
    char *colon = strchr(h, ':');
    if (colon)
        *colon = '\0';
    /* written back from the parsed address, not as typed: inet_aton takes trailing blanks
       and forms such as 0xc0a80301 */
    struct in_addr a;
    if (inet_aton(h, &a))
        inet_ntoa_r(a, out, cap);
    else
        out[0] = '\0';
}

static void web_send(int c, const char *s, size_t n)
{
    for (size_t at = 0; at < n;) {
        int k = send(c, s + at, n - at, 0);
        if (k <= 0)
            return;
        at += (size_t)k;
    }
}

/* The radio runs no web page of its own: the S3 is the AP, with the dashboard and the API. A
   request on port 80 gets a redirect there. Each request is WiFi airtime the tags lose
   (docs/ap-netradio.md, "Limits"), so it is one small answer and the connection closes. */
static void web_task(void *arg)
{
    (void)arg;
    static const char page[] =
        "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width\">"
        "<title>openESL radio</title><h1>openESL radio</h1>"
        "<p>This is a radio for an openESL access point. It has no page of its own: once it has found the "
        "access point on the network, it sends you to its dashboard. It has not found it yet."
        "<p>It looks for the access point on mDNS. To give it the address, use the Flash AP tab on "
        "<a href=\"https://mathiasbk.github.io/openESL/#ap\">mathiasbk.github.io/openESL</a>, or type "
        "<code>host &lt;address&gt;</code> on the radio's USB console.";
    xEventGroupWaitBits(net_events, GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(80), .sin_addr.s_addr = htonl(INADDR_ANY)};
    int one = 1;
    if (ls < 0 || setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 2)) {
        ESP_LOGW(TAG, "no web redirect: port 80 not open");
        if (ls >= 0)
            close(ls);
        vTaskDelete(NULL);
    }
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        struct timeval tv = {1, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        /* The request matters only for its path: read the whole head (data that comes after
           close() makes lwIP answer with a reset, which a browser may see before the answer),
           keeping its start, within WEB_REQUEST_US from the connection: a client that
           trickles bytes does not hold the port */
        char start[16] = {0}, buf[128];
        size_t n = 0;
        uint32_t last4 = 0;
        bool end = false;
        int64_t until = esp_timer_get_time() + WEB_REQUEST_US;
        while (!end && esp_timer_get_time() < until) {
            int k = recv(c, buf, sizeof(buf), 0);
            if (k <= 0)
                break;
            for (int i = 0; i < k && !end; i++, n++) {
                if (n < sizeof(start) - 1)
                    start[n] = buf[i];
                last4 = last4 << 8 | (uint8_t)buf[i];
                end = last4 == 0x0D0A0D0A; /* \r\n\r\n */
            }
        }
        if (!end) {
            close(c);
            continue;
        }
        char ip[16], head[192];
        dashboard_ip(ip, sizeof(ip));
        const char *sp = strchr(start, ' ');
        bool root = sp && sp[1] == '/' && (sp[2] == ' ' || sp[2] == '?');
        int hn;
        if (!ip[0] && !root) {
            /* the favicon and the like: no second page */
            static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            web_send(c, nf, sizeof(nf) - 1);
        } else if (ip[0]) {
            hn = snprintf(head, sizeof(head),
                          "HTTP/1.1 302 Found\r\nLocation: http://%s/\r\nContent-Length: 0\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\n\r\n", ip);
            web_send(c, head, (size_t)hn);
        } else {
            hn = snprintf(head, sizeof(head),
                          "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\n\r\n", (unsigned)(sizeof(page) - 1));
            web_send(c, head, (size_t)hn);
            web_send(c, page, sizeof(page) - 1);
        }
        shutdown(c, SHUT_WR);
        close(c);
    }
}

/* ---- WiFi ---- */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(net_events, GOT_IP);
        ESP_LOGW(TAG, "WiFi lost (reason %d), joining again", ((wifi_event_sta_disconnected_t *)data)->reason);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "WiFi up: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(net_events, GOT_IP);
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, "openesl-radio");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (!ssid[0]) {
        ESP_LOGW(TAG, "no WiFi set: type ssid <name>, then pass <password>");
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    /* MIN_MODEM: without power save about half the 802.15.4 frames were lost (docs) */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_ERROR_CHECK(mdns_init());
}

/* ---- console ---- */

static void status(void)
{
    const esl_link_stats_t *l = &radio_link.stats;
    wifi_ap_record_t ap;
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    unsigned n_tags = rc.n_tags, log_len = rc.log_len;
    esl_rcache_stats_t s = rc.stats;
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "802.15.4 channel %u PAN 0x%04X: rx %lu replies %lu tx_failed %lu tx_resent %lu dropped %lu worst %lld us",
             channel, pan, (unsigned long)l->rx, (unsigned long)l->replies, (unsigned long)l->tx_failed,
             (unsigned long)l->tx_resent, (unsigned long)l->dropped, (long long)l->worst_us);
    ESP_LOGI(TAG, "cache: tags %u/%u polls %lu blocks %lu waits %lu log %u (dropped %lu)", n_tags, TAGS,
             (unsigned long)s.polls, (unsigned long)s.blocks, (unsigned long)s.waits, log_len,
             (unsigned long)s.log_dropped);
    ESP_LOGI(TAG, "link: %s, WiFi rssi %d, connects %lu syncs %lu ends %lu timeouts %lu, last sync %lld ms, worst %lld ms",
             sock >= 0 ? "up" : "down", rssi, (unsigned long)connects, (unsigned long)nl.syncs,
             (unsigned long)nl.ends, (unsigned long)timeouts, (long long)sync_us_last / 1000,
             (long long)sync_us_worst / 1000);
    ESP_LOGI(TAG, "heap: free %u, lowest %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

static void command(char *line)
{
    char *arg = strchr(line, ' ');
    if (arg)
        *arg++ = '\0';
    else
        arg = line + strlen(line);
    if (!strcmp(line, "ssid")) {
        cfg_set("ssid", arg);
        ESP_LOGI(TAG, "ssid set to \"%s\"; now pass <password> (or reboot)", arg);
    } else if (!strcmp(line, "pass")) {
        cfg_set("pass", arg);
        ESP_LOGI(TAG, "password set; reboot to join");
    } else if (!strcmp(line, "host")) {
        cfg_set("host", arg);
        xSemaphoreTake(lock, portMAX_DELAY);
        strlcpy(host, arg, sizeof(host));
        xSemaphoreGive(lock);
        if (arg[0])
            ESP_LOGI(TAG, "AP address set to %s", arg);
        else
            ESP_LOGI(TAG, "AP found on mDNS");
    } else if (!strcmp(line, "status")) {
        status();
    } else if (!strcmp(line, "reboot")) {
        esp_restart();
    } else if (line[0]) {
        ESP_LOGI(TAG, "commands: ssid <name>, pass <password>, host <address[:port]> (empty: mDNS), status, reboot");
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[128];
    size_t n = 0;
    int64_t last_status = esp_timer_get_time();
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(1000)) == 1) {
            if (c == '\r' || c == '\n') {
                line[n] = '\0';
                if (n)
                    command(line);
                n = 0;
            } else if (n < sizeof(line) - 1) {
                line[n++] = (char)c;
            }
        }
        if (esp_timer_get_time() - last_status > 60 * 1000000LL) {
            last_status = esp_timer_get_time();
            status();
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&ucfg) == ESP_OK) {
        usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
        usb_serial_jtag_vfs_use_driver();
    }

    cfg_get("ssid", ssid, sizeof(ssid));
    cfg_get("pass", pass, sizeof(pass));
    cfg_get("host", host, sizeof(host));
    cfg_get_radio();
    ESP_LOGI(TAG, "OpenESL radio over WiFi %s: %u tags, %u images, log %u", esp_app_get_description()->version,
             TAGS, IMAGES, LOG);

    /* the cache: images as large as the AP draws (esl_image) */
    size_t cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    tags = calloc(TAGS, sizeof(*tags));
    rlog = calloc(LOG, sizeof(*rlog));
    uint8_t *idata = malloc(IMAGES * cap);
    nrx_buf = malloc(cap + 64);
    txcap = 1024 + LOG * (ESL_NLINK_HDR + 10 + ESL_PSDU_MAX);
    txb = malloc(txcap);
    lock = xSemaphoreCreateMutex();
    rx_queue = xQueueCreate(8, sizeof(esl_link_rx_t));
    net_events = xEventGroupCreate();
    if (!tags || !rlog || !idata || !nrx_buf || !txb || !lock || !rx_queue || !net_events) {
        ESP_LOGE(TAG, "out of memory");
        abort();
    }
    for (unsigned i = 0; i < IMAGES; i++)
        images[i].data = idata + i * cap;
    fwpart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "tagfw");
    static const esl_rcache_ops_t ops = {NULL, read_fw};
    esl_rcache_init(&rc, pan, tags, TAGS, images, IMAGES, cap, rlog, LOG, &ops);
    rc.boot = esp_random() | 1;
    static const esl_nlink_radio_ops_t lops = {NULL, on_config, write_fw, on_tx_power};
    esl_nlink_radio_init(&nl, &rc, &lops, buf_write, NULL);

    wifi_start();

    /* the same priority as the AP's radio task, above WiFi's */
    xTaskCreate(radio_task, "radio", 4096, NULL, configMAX_PRIORITIES - 2, NULL);
    err = radio_link.start(&radio_link, channel, pan, ESL_AP_SHORT_ADDR, false);
    ESP_LOGI(TAG, "802.15.4 on channel %u, PAN 0x%04X: %s", channel, pan, esp_err_to_name(err));
    xTaskCreate(net_task, "net", 6144, NULL, 5, NULL);
    xTaskCreate(console_task, "console", 4096, NULL, 2, NULL);
    xTaskCreate(web_task, "web", 3072, NULL, 2, NULL);
}
