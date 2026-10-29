#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esl_nlink.h"
#include "esl_rhost.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "sdkconfig.h"

/* The radio over WiFi (docs/ap-netradio.md): an ESP32-C6 running ap/netradio/ answers
 * the tags from its own cache and syncs with this AP over TCP. This file stands in for
 * radio.c and its drivers: it is the host's end of the link (esl_nlink over esl_rhost),
 * a TCP server announced on mDNS as _openesl-ap._tcp, for one radio at a time.
 *
 * The radio starts every exchange; this side only answers (no keepalive, nothing sent
 * on its own), since every WiFi packet at the radio costs it tag replies. A radio that
 * connects again replaces the connection before it (it restarted, or lost the old one).
 *
 * Not secure yet: plain TCP, and any device on the network may connect as the radio
 * (pairing and TLS-PSK are the next step, docs/ap-netradio.md). */

static const char *TAG = "netradio";

#define RX_BUF   2048                    /* the largest frame from the radio: a SYNC */
#define IDLE_S   120                     /* a radio silent this long is gone (it syncs every 30 s at most) */

static esl_rhost_t host;
static esl_nlink_host_t hl;
static esl_nlink_rx_t rx;
static uint8_t rx_buf[RX_BUF];
static int conn = -1;
static esl_radio_link_t net_link;
static bool full_warned;
static uint8_t channel;
static uint16_t pan;
/* The transmit power for the radio: set by the HTTP task, given to the link by net_task */
static volatile int8_t tx_dbm;
static volatile bool tx_due;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

static bool net_write(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    while (n) {
        int k = send(conn, d, n, 0);
        if (k <= 0)
            return false;
        d += k;
        n -= (size_t)k;
    }
    return true;
}

static bool on_frame(void *ctx, uint8_t type, const uint8_t *p, size_t len)
{
    (void)ctx;
    return esl_nlink_host_on(&hl, type, p, len);
}

static void log_stats(void)
{
    const esl_rhost_stats_t *s = &host.stats;
    ESP_LOGI(TAG, "syncs %lu replayed %lu skipped %lu resets %lu tags %lu drops %lu images %lu fws %lu drawn %lu "
             "refused %lu radio log dropped %lu",
             (unsigned long)s->syncs, (unsigned long)s->replayed, (unsigned long)s->skipped, (unsigned long)s->resets,
             (unsigned long)s->tags, (unsigned long)s->drops, (unsigned long)s->images, (unsigned long)s->fws,
             (unsigned long)s->drawn, (unsigned long)s->refused, (unsigned long)s->radio_log_dropped);
}

static void close_conn(const char *why)
{
    if (conn < 0)
        return;
    ESP_LOGW(TAG, "radio connection closed: %s", why);
    close(conn);
    conn = -1;
    net_link.up = false;
}

static void take(int s, const struct sockaddr_in *from)
{
    close_conn("a new connection replaces it");
    int one = 1, zero = 0;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &zero, sizeof(zero));
    /* a radio that stops reading mid-answer (it lost WiFi during an image) must not hold
       this task, which also takes the radio's next connection */
    struct timeval tv = {5, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    conn = s;
    net_link = (esl_radio_link_t){.up = true, .ip = ntohl(from->sin_addr.s_addr), .seen_ms = now_ms()};
    full_warned = false;
    esl_nlink_rx_init(&rx, rx_buf, sizeof(rx_buf));
    esl_nlink_host_init(&hl, &host, esl_radio_tagfw(), channel, pan, net_write, NULL, now_ms);
    esl_nlink_host_set_tx_power(&hl, tx_dbm);
    char ip[16];
    inet_ntoa_r(from->sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "radio connected from %s", ip);
}

static void net_task(void *arg)
{
    int ls = (int)(intptr_t)arg;
    static uint8_t buf[1460];
    int64_t last_rx = 0, last_stats = esp_timer_get_time();
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ls, &fds);
        if (conn >= 0)
            FD_SET(conn, &fds);
        struct timeval tv = {10, 0};
        int r = select((conn > ls ? conn : ls) + 1, &fds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(ls, &fds)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int s = accept(ls, (struct sockaddr *)&from, &fl);
            if (s >= 0) {
                take(s, &from);
                last_rx = esp_timer_get_time();
            }
        }
        /* a planned channel move (esl_store_move_channel): the radio gets it in the CONFIG
           with its next answer, or when it connects */
        uint8_t ch = esl_store_channel_step(host.st, now_ms());
        if (ch) {
            channel = ch;
            if (conn >= 0)
                esl_nlink_host_set_config(&hl, channel, pan);
            ESP_LOGI(TAG, "radio moves to channel %u", ch);
        }
        if (r > 0 && conn >= 0 && FD_ISSET(conn, &fds)) {
            int n = recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) {
                close_conn(n == 0 ? "closed by the radio" : strerror(errno));
            } else {
                last_rx = esp_timer_get_time();
                net_link.seen_ms = now_ms();
                if (tx_due) {
                    tx_due = false;
                    esl_nlink_host_set_tx_power(&hl, tx_dbm);
                }
                uint32_t syncs = host.stats.syncs;
                int64_t t0 = esp_timer_get_time();
                if (!esl_nlink_feed(&rx, buf, (size_t)n, on_frame, NULL))
                    close_conn(hl.broken ? "write failed" : "bad frame");
                else
                    net_link.ver = hl.hello ? hl.radio.radio_ver : 0;
                if (hl.hello)  /* a C6 from before 0.9.0 retunes but does not tell the tags */
                    esl_store_set_radio_follows(host.st, hl.radio.features & ESL_NLINK_F_CHANNEL);
                if (host.stats.syncs != syncs) {
                    ESP_LOGI(TAG, "sync %lu answered in %lld ms (log %u)", (unsigned long)host.stats.syncs,
                             (long long)(esp_timer_get_time() - t0) / 1000, hl.req.log_n);
                    unsigned n_tags = esl_store_count(host.st);
                    if (!full_warned && hl.hello && n_tags > hl.radio.max_tags) {
                        full_warned = true;
                        ESP_LOGW(TAG, "%u tags, but the radio holds %u: the rest get no content from it", n_tags,
                                 hl.radio.max_tags);
                    }
                }
            }
        }
        if (conn >= 0 && esp_timer_get_time() - last_rx > IDLE_S * 1000000LL)
            close_conn("silent");
        if (esp_timer_get_time() - last_stats > 60 * 1000000LL) {
            last_stats = esp_timer_get_time();
            log_stats();
        }
    }
}

const esl_radio_link_t *radio_net_link(void)
{
    return &net_link;
}

/* With the radio's next sync (it gets it in every CONFIG, and keeps its own until then) */
void radio_set_tx_power(int8_t dbm)
{
    tx_dbm = dbm;
    tx_due = true;
}

esp_err_t radio_start(esl_store_t *st, esl_layouts_t *ls, uint8_t ch, uint16_t pan_id)
{
    channel = ch;
    pan = pan_id;
    tx_dbm = radio_tx_power()->dbm;
    unsigned max = esl_store_slots(st);
    /* sent[] has to cover every slot the store can grow to */
    if (max < CONFIG_ESL_MAX_TAGS)
        max = CONFIG_ESL_MAX_TAGS;
    size_t cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    esl_rtag_state_t *sent = heap_caps_calloc(max, sizeof(*sent), MALLOC_CAP_8BIT);
    uint8_t *img = malloc(cap);
    if (!sent || !img)
        return ESP_ERR_NO_MEM;
    esl_rhost_init(&host, st, ls, esl_radio_image(), esl_radio_tagfw(), pan_id, sent, max, img, cap);

    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0)
        return ESP_FAIL;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CONFIG_ESL_NETRADIO_PORT)};
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 1)) {
        close(s);
        return ESP_FAIL;
    }
    /* drawing images and replaying the log: the stack of esl_radio_handle and more */
    if (xTaskCreate(net_task, "netradio", 8192, (void *)(intptr_t)s, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    esp_err_t err = mdns_service_add(NULL, "_openesl-ap", "_tcp", CONFIG_ESL_NETRADIO_PORT, NULL, 0);
    ESP_LOGI(TAG, "waiting for the radio on TCP port %d (mDNS _openesl-ap._tcp: %s), channel %u, PAN 0x%04X, %d dBm",
             CONFIG_ESL_NETRADIO_PORT, esp_err_to_name(err), ch, pan_id, tx_dbm);
    return ESP_OK;
}
