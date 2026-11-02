#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esl_nlink.h"
#include "esl_rhost.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "sdkconfig.h"

/* The radio over WiFi (docs/ap-netradio.md): an ESP32-C6 running ap/netradio/ answers
 * the tags from its own cache and syncs with this AP over TCP. This file stands in for
 * radio.c and its drivers: it is the host's end of the link (esl_nlink over esl_rhost),
 * a TCP server announced on mDNS as _openesl-ap._tcp.
 *
 * Several radios (docs/ap-netradio.md, "Several radios"): up to RADIOS at once, each in
 * a place of its own, by the id (MAC) in its HELLO. Place 0 is on the AP's channel and
 * follows its moves (the only place when there is one radio, as before); a radio in
 * another place gets a channel of its own, picked when it first connects
 * (esl_rhost_pick_channel). The places and their channels are kept in NVS (cfg/radios),
 * so a radio gets its channel back after a restart. Each place has its own esl_rhost_t,
 * kept across connections; esl_rowners_t says which radio last heard each tag, and only
 * that one is told the tag's state and given its images.
 *
 * A radio starts every exchange; this side only answers (no keepalive, nothing sent on
 * its own), since every WiFi packet at a radio costs it tag replies. A radio that
 * connects again replaces its connection before it (it restarted, or lost the old one).
 *
 * Not secure yet: plain TCP, and any device on the network may connect as a radio
 * (pairing and TLS-PSK are the next step, docs/ap-netradio.md). */

static const char *TAG = "netradio";

#define RADIOS   ESL_STORE_RADIOS        /* places: the one on the AP's channel and 3 more */
#define CONNS    (RADIOS + 1)            /* one more, so a radio that reconnects is not turned away */
#define RX_BUF   2048                    /* the largest frame from a radio: a SYNC */
#define IDLE_S   120                     /* a radio silent this long is gone (it syncs every 30 s at most) */
#define HELLO_S  10                      /* a connection without HELLO this long is not a radio */

typedef struct {
    uint64_t id;             /* the radio's MAC, 0 = a free place */
    uint8_t channel;         /* its own channel, 0 in place 0 (the AP's) */
    bool ready;              /* host, sent and img are set up for this id */
    esl_rhost_t host;
    esl_rtag_state_t *sent;
    uint8_t *img;
    int conn;                /* its connection (conns[]), -1 = none */
    bool full_warned;
} place_t;

typedef struct {
    int fd;                  /* -1 = free */
    int place;               /* -1 until its HELLO */
    esl_nlink_host_t hl;
    esl_nlink_rx_t rx;
    uint8_t rx_buf[RX_BUF];
    int64_t last_rx_us;
    uint32_t ip;
} conn_t;

static esl_store_t *store;
static esl_layouts_t *layouts;
static place_t places[RADIOS];
static conn_t conns[CONNS];
static esl_radio_link_t links[RADIOS];
static esl_rowners_t owners;
static unsigned max_tags;
static size_t img_cap;
static uint8_t channel;      /* the AP's */
static uint16_t pan;
/* The transmit power for the radios: set by the HTTP task, given to the links by net_task */
static volatile int8_t tx_dbm;
static volatile bool tx_due;
/* The places change in net_task (a HELLO) and in the HTTP task (DELETE /api/radios/{id}) */
static SemaphoreHandle_t places_mu;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

/* ---- the places, kept in NVS as "id:channel" per place, comma-separated ---- */

static void save_places(void)
{
    char v[RADIOS * 21 + 1], *p = v;
    for (unsigned i = 0; i < RADIOS; i++) {
        if (places[i].id)
            p += sprintf(p, "%016" PRIX64 ":%u", places[i].id, (unsigned)places[i].channel);
        if (i + 1 < RADIOS)
            *p++ = ',';
    }
    *p = '\0';
    cfg_set_str("radios", v);
}

static void load_places(void)
{
    char v[RADIOS * 21 + 1];
    cfg_get_str("radios", "", v, sizeof(v));
    const char *p = v;
    for (unsigned i = 0; i < RADIOS && *p; i++) {
        char *end;
        uint64_t id = strtoull(p, &end, 16);
        if (end != p && *end == ':') {
            unsigned long ch = strtoul(end + 1, &end, 10);
            places[i].id = id;
            places[i].channel = i && ch >= ESL_CHANNEL_MIN && ch <= ESL_CHANNEL_MAX ? (uint8_t)ch : 0;
            if (i && !places[i].channel)
                places[i].id = 0; /* a place of its own without a channel: free it */
        }
        p = strchr(p, ',');
        if (!p)
            break;
        p++;
    }
    for (unsigned i = 1; i < RADIOS; i++)
        if (places[i].id)
            esl_store_set_radio_channel(store, (uint8_t)i, places[i].channel);
}

/* The channels a new radio cannot have: the AP's, a move it plans, the other places' */
static uint32_t taken_channels(void)
{
    esl_channel_state_t cs;
    esl_store_channel_state(store, now_ms(), &cs);
    uint32_t taken = 1u << cs.channel;
    if (cs.next)
        taken |= 1u << cs.next;
    for (unsigned i = 1; i < RADIOS; i++)
        if (places[i].id)
            taken |= 1u << places[i].channel;
    return taken;
}

/* ---- connections ---- */

static bool net_write(void *ctx, const uint8_t *d, size_t n)
{
    conn_t *c = ctx;
    while (n) {
        int k = send(c->fd, d, n, 0);
        if (k <= 0)
            return false;
        d += k;
        n -= (size_t)k;
    }
    return true;
}

static bool on_frame(void *ctx, uint8_t type, const uint8_t *p, size_t len)
{
    conn_t *c = ctx;
    return esl_nlink_host_on(&c->hl, type, p, len);
}

static unsigned connected(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < RADIOS; i++)
        n += places[i].conn >= 0;
    return n;
}

static void close_conn(conn_t *c, const char *why)
{
    if (c->fd < 0)
        return;
    if (c->place >= 0) {
        place_t *p = &places[c->place];
        ESP_LOGW(TAG, "radio %d (%016" PRIX64 ") connection closed: %s", c->place, p->id, why);
        p->conn = -1;
        links[c->place].up = false;
        /* gone since it was last heard: "radio_lost" in GET /api/health ESL_RADIO_LOST_S after that,
           at once when it took IDLE_S to tell */
        links[c->place].down_ms = links[c->place].seen_ms ? links[c->place].seen_ms : now_ms();
    } else {
        ESP_LOGW(TAG, "radio connection closed before its HELLO: %s", why);
    }
    close(c->fd);
    c->fd = -1;
    c->place = -1;
    owners.radios = (uint8_t)connected();
}

static void log_stats(void)
{
    for (unsigned i = 0; i < RADIOS; i++) {
        if (!places[i].ready)
            continue;
        const esl_rhost_stats_t *s = &places[i].host.stats;
        ESP_LOGI(TAG, "radio %u: syncs %lu replayed %lu skipped %lu resets %lu tags %lu drops %lu images %lu fws %lu "
                 "drawn %lu refused %lu radio log dropped %lu, serves %u tags",
                 i, (unsigned long)s->syncs, (unsigned long)s->replayed, (unsigned long)s->skipped,
                 (unsigned long)s->resets, (unsigned long)s->tags, (unsigned long)s->drops, (unsigned long)s->images,
                 (unsigned long)s->fws, (unsigned long)s->drawn, (unsigned long)s->refused,
                 (unsigned long)s->radio_log_dropped, links[i].tags);
    }
}

/* The place for a radio with this id: the one it had, else a free one (place 0 first,
   so a lone radio is on the AP's channel as before), -1 when all are taken. */
static int place_for(uint64_t id, bool *is_new)
{
    *is_new = false;
    unsigned known = 0;
    for (unsigned i = 0; i < RADIOS; i++)
        known += places[i].id != 0;
    for (unsigned i = 0; i < RADIOS; i++) {
        if (places[i].id != id)
            continue;
        if (i && known == 1 && places[i].conn < 0) {
            /* the only radio left, and place 0 is free (the radio there was forgotten, say a
               broken C6 this one replaced): it moves to the AP's channel, where tags from
               before 0.9.0, which cannot look on other channels, still find it */
            ESP_LOGI(TAG, "radio %016" PRIX64 " is the only one: it moves from place %u to place 0", id, i);
            esl_rowners_forget(&owners, (uint8_t)i);
            esl_store_set_radio_channel(store, (uint8_t)i, 0);
            places[i].id = 0;
            places[i].channel = 0;
            places[i].ready = false;
            links[i] = (esl_radio_link_t){.down_ms = now_ms()}; /* lost counts from now, not from boot */
            places[0].id = id;
            places[0].ready = false;
            *is_new = true;
            return 0;
        }
        return (int)i;
    }
    for (unsigned i = 0; i < RADIOS; i++) {
        if (places[i].id)
            continue;
        uint8_t ch = 0;
        if (i && !(ch = esl_rhost_pick_channel(taken_channels())))
            return -1;
        places[i].id = id;
        places[i].channel = ch;
        places[i].ready = false;
        *is_new = true;
        return (int)i;
    }
    return -1;
}

static bool hello_locked(conn_t *c, esl_nlink_host_t *n, bool *is_new_out);

/* HELLO: the radio's place, its esl_rhost_t and its channel */
static bool on_hello(void *ctx, esl_nlink_host_t *n)
{
    conn_t *c = ctx;
    bool is_new;
    xSemaphoreTake(places_mu, portMAX_DELAY);
    bool ok = hello_locked(c, n, &is_new);
    xSemaphoreGive(places_mu);
    return ok;
}

static bool hello_locked(conn_t *c, esl_nlink_host_t *n, bool *is_new_out)
{
    bool is_new;
    int i = place_for(n->radio.id, &is_new);
    *is_new_out = is_new;
    if (i < 0) {
        ESP_LOGW(TAG, "radio %016" PRIX64 " turned away: all %d places are taken (forget one: "
                 "DELETE /api/radios/{id})", n->radio.id, RADIOS);
        return false;
    }
    place_t *p = &places[i];
    if (!p->sent) {
        p->sent = heap_caps_calloc(max_tags, sizeof(*p->sent), MALLOC_CAP_8BIT);
        p->img = heap_caps_malloc(img_cap, MALLOC_CAP_8BIT);
        if (!p->sent || !p->img) {
            ESP_LOGE(TAG, "no memory for radio %d", i);
            free(p->sent);
            free(p->img);
            p->sent = NULL;
            p->img = NULL;
            if (is_new)
                p->id = 0;
            return false;
        }
    }
    if (!p->ready) {
        esl_rhost_init(&p->host, store, layouts, esl_radio_image(), esl_radio_tagfw(), pan, p->sent,
                       max_tags, p->img, img_cap);
        esl_rhost_share(&p->host, &owners, (uint8_t)i);
        p->ready = true;
        p->full_warned = false;
    }
    if (is_new) {
        if (i)
            esl_store_set_radio_channel(store, (uint8_t)i, p->channel);
        save_places();
        ESP_LOGI(TAG, "new radio %016" PRIX64 " in place %d, channel %u", n->radio.id, i,
                 i ? p->channel : channel);
    }
    if (p->conn >= 0 && &conns[p->conn] != c)
        close_conn(&conns[p->conn], "the radio connected again");
    p->conn = (int)(c - conns);
    c->place = i;
    n->h = &p->host;
    n->channel = i ? p->channel : channel;
    links[i] = (esl_radio_link_t){.up = true, .ip = c->ip, .seen_ms = now_ms(), .ver = n->radio.radio_ver,
                                  .id = p->id, .channel = p->channel,
                                  .tags = esl_rowners_count(&owners, (uint8_t)i)};
    owners.radios = (uint8_t)connected();
    if (i == 0)  /* a C6 from before 0.9.0 retunes but does not tell the tags */
        esl_store_set_radio_follows(store, n->radio.features & ESL_NLINK_F_CHANNEL);
    return true;
}

static void take(int s, const struct sockaddr_in *from)
{
    conn_t *c = NULL;
    for (unsigned i = 0; i < CONNS && !c; i++)
        if (conns[i].fd < 0)
            c = &conns[i];
    if (!c) {
        /* every slot busy: drop one that has not said HELLO, else the one silent longest,
           which is likely gone */
        for (unsigned i = 0; i < CONNS; i++)
            if (conns[i].place < 0 && (!c || conns[i].last_rx_us < c->last_rx_us))
                c = &conns[i];
        if (!c) {
            c = &conns[0];
            for (unsigned i = 1; i < CONNS; i++)
                if (conns[i].last_rx_us < c->last_rx_us)
                    c = &conns[i];
        }
        close_conn(c, "a new connection replaces it");
    }
    int one = 1, zero = 0;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &zero, sizeof(zero));
    /* a radio that stops reading mid-answer (it lost WiFi during an image) must not hold
       this task, which also serves the other radios and takes new connections */
    struct timeval tv = {5, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    c->fd = s;
    c->place = -1;
    c->ip = ntohl(from->sin_addr.s_addr);
    c->last_rx_us = esp_timer_get_time();
    esl_nlink_rx_init(&c->rx, c->rx_buf, sizeof(c->rx_buf));
    esl_nlink_host_init(&c->hl, NULL, esl_radio_tagfw(), channel, pan, net_write, c, now_ms);
    c->hl.on_hello = on_hello;
    c->hl.hello_ctx = c;
    esl_nlink_host_set_tx_power(&c->hl, tx_dbm);
    char ip[16];
    inet_ntoa_r(from->sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "radio connected from %s", ip);
}

static void receive(conn_t *c)
{
    static uint8_t buf[1460];
    int n = recv(c->fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        close_conn(c, n == 0 ? "closed by the radio" : strerror(errno));
        return;
    }
    c->last_rx_us = esp_timer_get_time();
    int i = c->place;
    uint32_t syncs = i >= 0 ? places[i].host.stats.syncs : 0;
    int64_t t0 = esp_timer_get_time();
    if (!esl_nlink_feed(&c->rx, buf, (size_t)n, on_frame, c)) {
        close_conn(c, c->hl.broken ? "write failed" : "bad frame");
        return;
    }
    i = c->place; /* set by the HELLO in this data */
    if (i < 0)
        return;
    place_t *p = &places[i];
    links[i].seen_ms = now_ms();
    if (p->host.stats.syncs != syncs) {
        links[i].tags = esl_rowners_count(&owners, (uint8_t)i);
        ESP_LOGI(TAG, "radio %d: sync %lu answered in %lld ms (log %u)", i, (unsigned long)p->host.stats.syncs,
                 (long long)(esp_timer_get_time() - t0) / 1000, c->hl.req.log_n);
        unsigned n_tags = esl_store_count(store);
        if (!p->full_warned && n_tags > c->hl.radio.max_tags) {
            p->full_warned = true;
            ESP_LOGW(TAG, "%u tags, but radio %d holds %u: tags beyond that get no content from it", n_tags, i,
                     c->hl.radio.max_tags);
        }
    }
}

/* DELETE /api/radios/{id}, under places_mu: 0, 404 or 409 */
static int forget_locked(uint64_t id)
{
    for (unsigned i = 0; i < RADIOS; i++) {
        place_t *p = &places[i];
        if (p->id != id)
            continue;
        if (p->conn >= 0)
            return 409;
        ESP_LOGI(TAG, "radio %016" PRIX64 " forgotten (place %u)", id, i);
        p->id = 0;
        p->channel = 0;
        p->ready = false;
        links[i] = (esl_radio_link_t){.down_ms = now_ms()}; /* lost counts from now, not from boot */
        esl_rowners_forget(&owners, (uint8_t)i);
        if (i)
            esl_store_set_radio_channel(store, (uint8_t)i, 0);
        save_places();
        return 0;
    }
    return 404;
}

static void net_task(void *arg)
{
    int ls = (int)(intptr_t)arg;
    int64_t last_stats = esp_timer_get_time();
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ls, &fds);
        int maxfd = ls;
        for (unsigned i = 0; i < CONNS; i++)
            if (conns[i].fd >= 0) {
                FD_SET(conns[i].fd, &fds);
                if (conns[i].fd > maxfd)
                    maxfd = conns[i].fd;
            }
        struct timeval tv = {1, 0};
        int r = select(maxfd + 1, &fds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(ls, &fds)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int s = accept(ls, (struct sockaddr *)&from, &fl);
            if (s >= 0)
                take(s, &from);
        }
        /* a planned channel move (esl_store_move_channel): the radio in place 0 gets it in
           the CONFIG with its next answer, or when it connects */
        uint8_t ch = esl_store_channel_step(store, now_ms());
        if (ch) {
            channel = ch;
            if (places[0].conn >= 0)
                esl_nlink_host_set_config(&conns[places[0].conn].hl, channel, pan);
            ESP_LOGI(TAG, "radio moves to channel %u", ch);
        }
        if (tx_due) {
            /* each radio gets it in the CONFIG with its next answer */
            tx_due = false;
            for (unsigned i = 0; i < CONNS; i++)
                if (conns[i].fd >= 0)
                    esl_nlink_host_set_tx_power(&conns[i].hl, tx_dbm);
        }
        for (unsigned i = 0; r > 0 && i < CONNS; i++)
            if (conns[i].fd >= 0 && FD_ISSET(conns[i].fd, &fds))
                receive(&conns[i]);
        for (unsigned i = 0; i < CONNS; i++)
            if (conns[i].fd >= 0 &&
                esp_timer_get_time() - conns[i].last_rx_us > (conns[i].place < 0 ? HELLO_S : IDLE_S) * 1000000LL)
                close_conn(&conns[i], conns[i].place < 0 ? "no HELLO" : "silent");
        if (esp_timer_get_time() - last_stats > 60 * 1000000LL) {
            last_stats = esp_timer_get_time();
            log_stats();
        }
    }
}

const esl_radio_link_t *radio_net_link(void)
{
    return &links[0];
}

const esl_radio_link_t *radio_net_links(unsigned *n)
{
    *n = RADIOS;
    return links;
}

static int forget(void *ctx, uint64_t id)
{
    (void)ctx;
    if (!places_mu)
        return 404;
    xSemaphoreTake(places_mu, portMAX_DELAY);
    int r = forget_locked(id);
    xSemaphoreGive(places_mu);
    return r;
}

const esl_radio_ops_t *radio_net_ops(void)
{
    static const esl_radio_ops_t ops = {NULL, forget};
    return &ops;
}

/* With each radio's next sync (it gets it in every CONFIG, and keeps its own until then) */
void radio_set_tx_power(int8_t dbm)
{
    tx_dbm = dbm;
    tx_due = true;
}

esp_err_t radio_start(esl_store_t *st, esl_layouts_t *ls, uint8_t ch, uint16_t pan_id)
{
    store = st;
    channel = ch;
    pan = pan_id;
    tx_dbm = radio_tx_power()->dbm;
    layouts = ls;
    max_tags = esl_store_slots(st);
    /* sent[] has to cover every slot the store can grow to */
    if (max_tags < CONFIG_ESL_MAX_TAGS)
        max_tags = CONFIG_ESL_MAX_TAGS;
    img_cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    places_mu = xSemaphoreCreateMutex();
    if (!places_mu)
        return ESP_ERR_NO_MEM;
    esl_rowner_t *by_slot = heap_caps_calloc(max_tags, sizeof(*by_slot), MALLOC_CAP_8BIT);
    if (!by_slot)
        return ESP_ERR_NO_MEM;
    esl_rowners_init(&owners, by_slot, max_tags);
    for (unsigned i = 0; i < CONNS; i++) {
        conns[i].fd = -1;
        conns[i].place = -1;
    }
    for (unsigned i = 0; i < RADIOS; i++)
        places[i].conn = -1;
    load_places();
    for (unsigned i = 0; i < RADIOS; i++) {
        links[i].id = places[i].id;
        links[i].channel = places[i].channel;
    }

    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0)
        return ESP_FAIL;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CONFIG_ESL_NETRADIO_PORT)};
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 2)) {
        close(s);
        return ESP_FAIL;
    }
    /* drawing images and replaying the logs: the stack of esl_radio_handle and more */
    if (xTaskCreate(net_task, "netradio", 8192, (void *)(intptr_t)s, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    esp_err_t err = mdns_service_add(NULL, "_openesl-ap", "_tcp", CONFIG_ESL_NETRADIO_PORT, NULL, 0);
    ESP_LOGI(TAG, "waiting for radios on TCP port %d (mDNS _openesl-ap._tcp: %s), channel %u, PAN 0x%04X, %d dBm, "
             "%d places", CONFIG_ESL_NETRADIO_PORT, esp_err_to_name(err), ch, pan_id, tx_dbm, RADIOS);
    return ESP_OK;
}
