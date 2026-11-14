/* netradio.c - network radios beside the AP's own (docs/linux-ap.md, "Network radios").
 *
 * The host's end of the link that ap/main/radio_net.c is on an S3 (docs/ap-netradio.md):
 * C6 boards running ap/netradio/ connect over TCP, each answers the tags from its own
 * cache and syncs with this AP (esl_nlink over esl_rhost). Here the AP has a radio of its
 * own as well, which stays in place 0 on the AP's channel and follows its moves; the
 * network radios take places 1..3, each with a channel of its own picked when it first
 * connects (esl_rhost_pick_channel), kept in <data>/radios as on the S3 (cfg/radios). The
 * radio that last heard a tag serves it (esl_rowners_t): the own radio counts its frames
 * through netradio_heard(), the others through the logs of their syncs.
 *
 * One thread serves every connection: it answers, it never sends on its own, as on the
 * S3. It alone changes the owners: the own radio's frames reach them through a queue, so
 * the radio loop never waits for a sync that draws images, and a radio forgotten through
 * the API is taken out of them by this thread too. The API reads a copy of the links,
 * published under its own lock (http_api_lock), so it never sees one half written. Not secure, as on the S3: plain TCP, and any device
 * that can reach the port may connect as a radio. */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "ap.h"
#include "esl_image.h"
#include "esl_nlink.h"
#include "esl_rhost.h"

#define RADIOS  ESL_STORE_RADIOS   /* places: the own radio's (0) and 3 more */
#define CONNS   RADIOS             /* one more than the network radios, so one that reconnects is not turned away */
#define RX_BUF  2048               /* the largest frame from a radio: a SYNC */
#define IDLE_S  120                /* a radio silent this long is gone (it syncs every 30 s at most) */
#define HELLO_S 10                 /* a connection without HELLO this long is not a radio */
#define HEARD_N 64                 /* frames of the own radio waiting for the owners */

typedef struct {
    uint64_t id;             /* the radio's MAC, 0 = a free place */
    uint8_t channel;         /* its own channel */
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
    uint64_t last_rx_ms;
    uint32_t ip;
} conn_t;

static bool started;
static place_t places[RADIOS];
static conn_t conns[CONNS];
static esl_radio_link_t links[RADIOS];  /* this thread's (and forget()'s, for a place not connected) */
static esl_radio_link_t pub[RADIOS];    /* what the API reads, under http_api_lock */
static esl_rowners_t owners;
static unsigned max_tags;
static size_t img_cap;
/* The places change in the netradio thread (a HELLO) and in the HTTP server's (DELETE
   /api/radios/{id}) */
static pthread_mutex_t places_mu = PTHREAD_MUTEX_INITIALIZER;
/* The own radio's frames: the radio loop adds, the netradio thread takes them to the owners */
static pthread_mutex_t heard_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t mac, at_ms; } heard[HEARD_N];
static unsigned heard_n;
/* Places forgotten through the API whose tags are still theirs in the owners (bit = place),
   under places_mu */
static unsigned forgotten;

/* ---- the places, kept in <data>/radios as "id:channel" per place, comma-separated ---- */

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
    store_files_put_text("radios", v);
}

static void load_places(void)
{
    char v[RADIOS * 21 + 1];
    store_files_get_text("radios", v, sizeof(v));
    const char *p = v;
    for (unsigned i = 0; i < RADIOS && *p; i++) {
        char *end;
        uint64_t id = strtoull(p, &end, 16);
        if (i && end != p && *end == ':') {
            unsigned long ch = strtoul(end + 1, &end, 10);
            if (ch >= ESL_CHANNEL_MIN && ch <= ESL_CHANNEL_MAX) {
                places[i].id = id;
                places[i].channel = (uint8_t)ch;
                esl_store_set_radio_channel(&ap.store, (uint8_t)i, places[i].channel);
            }
        }
        p = strchr(p, ',');
        if (!p)
            break;
        p++;
    }
}

/* The channels a new radio cannot have: the AP's, a move it plans, the other places' */
static uint32_t taken_channels(void)
{
    esl_channel_state_t cs;
    esl_store_channel_state(&ap.store, ap_now_ms(), &cs);
    uint32_t taken = 1u << cs.channel;
    if (cs.next)
        taken |= 1u << cs.next;
    for (unsigned i = 1; i < RADIOS; i++)
        if (places[i].id)
            taken |= 1u << places[i].channel;
    return taken;
}

/* ---- the own radio's frames ---- */

void netradio_heard(const uint8_t *psdu, size_t len, uint64_t now_ms)
{
    esl_rx_frame_t f;
    if (!started || !esl_frame_parse_from_tag(psdu, len, &f) || f.dst_pan != ap.pan)
        return;
    /* at once in the store, so the reply tells a tag that came over from another radio the
       AP's channel; the owners when the netradio thread gets to it. One entry per tag (a tag
       fetching an image sends a frame per block): its latest frame. */
    esl_store_set_tag_radio(&ap.store, f.src_mac, 0);
    pthread_mutex_lock(&heard_mu);
    unsigned i = 0;
    while (i < heard_n && heard[i].mac != f.src_mac)
        i++;
    if (i < HEARD_N) {
        heard[i].mac = f.src_mac;
        heard[i].at_ms = now_ms;
        if (i == heard_n)
            heard_n++;
    }
    pthread_mutex_unlock(&heard_mu);
}

/* The owners up to date: the places forgotten through the API, then the own radio's frames */
static void take_heard(void)
{
    pthread_mutex_lock(&places_mu);
    unsigned f = forgotten;
    forgotten = 0;
    pthread_mutex_unlock(&places_mu);
    for (unsigned i = 1; i < RADIOS; i++)
        if (f >> i & 1)
            esl_rowners_forget(&owners, (uint8_t)i);
    pthread_mutex_lock(&heard_mu);
    for (unsigned i = 0; i < heard_n; i++) {
        esl_rowners_heard(&owners, &ap.store, heard[i].mac, 0, heard[i].at_ms);
        if (heard[i].at_ms > links[0].seen_ms)
            links[0].seen_ms = heard[i].at_ms;
    }
    heard_n = 0;
    pthread_mutex_unlock(&heard_mu);
    links[0].tags = esl_rowners_count(&owners, 0);
}

/* The links for the API: copied under places_mu (forget() changes them too), then handed
   over under the API's lock; never both at once, as forget() runs under the API's lock */
static void publish(void)
{
    esl_radio_link_t copy[RADIOS];
    pthread_mutex_lock(&places_mu);
    memcpy(copy, links, sizeof(copy));
    pthread_mutex_unlock(&places_mu);
    http_api_lock();
    memcpy(pub, copy, sizeof(pub));
    http_api_unlock();
}

/* ---- connections ---- */

static uint64_t now_ms(void)
{
    return ap_now_ms();
}

static bool net_write(void *ctx, const uint8_t *d, size_t n)
{
    conn_t *c = ctx;
    while (n) {
        ssize_t k = send(c->fd, d, n, MSG_NOSIGNAL);
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

/* The radios connected now, the own one included */
static unsigned connected(void)
{
    unsigned n = 1;
    for (unsigned i = 1; i < RADIOS; i++)
        n += places[i].conn >= 0;
    return n;
}

static void close_conn(conn_t *c, const char *why)
{
    if (c->fd < 0)
        return;
    if (c->place >= 0) {
        place_t *p = &places[c->place];
        ap_log("radio %d (%016" PRIX64 ") connection closed: %s", c->place, p->id, why);
        p->conn = -1;
        links[c->place].up = false;
        /* gone since it was last heard: "radio_lost" ESL_RADIO_LOST_S after that */
        links[c->place].down_ms = links[c->place].seen_ms ? links[c->place].seen_ms : now_ms();
    } else {
        ap_log("radio connection closed before its HELLO: %s", why);
    }
    close(c->fd);
    c->fd = -1;
    c->place = -1;
    owners.radios = (uint8_t)connected();
}

static void log_stats(void)
{
    for (unsigned i = 1; i < RADIOS; i++) {
        if (!places[i].ready)
            continue;
        const esl_rhost_stats_t *s = &places[i].host.stats;
        ap_log("radio %u: syncs %" PRIu32 " replayed %" PRIu32 " skipped %" PRIu32 " resets %" PRIu32
               " tags %" PRIu32 " drops %" PRIu32 " images %" PRIu32 " fws %" PRIu32 " drawn %" PRIu32
               " refused %" PRIu32 " radio log dropped %" PRIu32 ", serves %u tags",
               i, s->syncs, s->replayed, s->skipped, s->resets, s->tags, s->drops, s->images, s->fws, s->drawn,
               s->refused, s->radio_log_dropped, links[i].tags);
    }
}

/* The place for a radio with this id: the one it had, else a free one, -1 when all are taken */
static int place_for(uint64_t id, bool *is_new)
{
    *is_new = false;
    for (unsigned i = 1; i < RADIOS; i++)
        if (places[i].id == id)
            return (int)i;
    for (unsigned i = 1; i < RADIOS; i++) {
        if (places[i].id)
            continue;
        uint8_t ch = esl_rhost_pick_channel(taken_channels());
        if (!ch)
            return -1;
        places[i].id = id;
        places[i].channel = ch;
        places[i].ready = false;
        *is_new = true;
        return (int)i;
    }
    return -1;
}

/* HELLO, under places_mu: the radio's place, its esl_rhost_t and its channel */
static bool hello_locked(conn_t *c, esl_nlink_host_t *n)
{
    /* a place forgotten since the last round goes to a new radio without the old one's tags */
    for (unsigned k = 1; k < RADIOS; k++)
        if (forgotten >> k & 1)
            esl_rowners_forget(&owners, (uint8_t)k);
    forgotten = 0;
    bool is_new;
    int i = n->radio.id ? place_for(n->radio.id, &is_new) : -1;
    if (i < 0) {
        ap_log("radio %016" PRIX64 " turned away: all %d places are taken (forget one: DELETE /api/radios/{id})",
               n->radio.id, RADIOS - 1);
        return false;
    }
    place_t *p = &places[i];
    if (!p->sent) {
        p->sent = calloc(max_tags, sizeof(*p->sent));
        p->img = malloc(img_cap);
        if (!p->sent || !p->img) {
            ap_log("no memory for radio %d", i);
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
        esl_rhost_init(&p->host, &ap.store, &ap.layouts, esl_radio_image(), esl_radio_tagfw(), ap.pan, p->sent,
                       max_tags, p->img, img_cap);
        esl_rhost_share(&p->host, &owners, (uint8_t)i);
        p->ready = true;
        p->full_warned = false;
    }
    if (is_new) {
        esl_store_set_radio_channel(&ap.store, (uint8_t)i, p->channel);
        save_places();
        ap_log("new radio %016" PRIX64 " in place %d, channel %u", n->radio.id, i, p->channel);
    }
    if (p->conn >= 0 && &conns[p->conn] != c)
        close_conn(&conns[p->conn], "the radio connected again");
    p->conn = (int)(c - conns);
    c->place = i;
    n->h = &p->host;
    n->channel = p->channel;
    links[i] = (esl_radio_link_t){.up = true, .ip = c->ip, .seen_ms = now_ms(), .ver = n->radio.radio_ver,
                                  .id = p->id, .channel = p->channel,
                                  .tags = esl_rowners_count(&owners, (uint8_t)i),
                                  .no_ap_id = !(n->radio.features & ESL_NLINK_F_APID)};
    owners.radios = (uint8_t)connected();
    return true;
}

static bool on_hello(void *ctx, esl_nlink_host_t *n)
{
    pthread_mutex_lock(&places_mu);
    bool ok = hello_locked(ctx, n);
    pthread_mutex_unlock(&places_mu);
    return ok;
}

/* The peer's IPv4 address, host order (0 for IPv6) */
static uint32_t peer_ip(const struct sockaddr_storage *from)
{
    if (from->ss_family == AF_INET)
        return ntohl(((const struct sockaddr_in *)from)->sin_addr.s_addr);
    if (from->ss_family == AF_INET6) {
        const struct in6_addr *a = &((const struct sockaddr_in6 *)from)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(a))
            return (uint32_t)a->s6_addr[12] << 24 | (uint32_t)a->s6_addr[13] << 16 | (uint32_t)a->s6_addr[14] << 8 |
                   a->s6_addr[15];
    }
    return 0;
}

static void take(int s, const struct sockaddr_storage *from)
{
    conn_t *c = NULL;
    for (unsigned i = 0; i < CONNS && !c; i++)
        if (conns[i].fd < 0)
            c = &conns[i];
    if (!c) {
        /* every slot busy: drop one that has not said HELLO, else the one silent longest,
           which is likely gone */
        for (unsigned i = 0; i < CONNS; i++)
            if (conns[i].place < 0 && (!c || conns[i].last_rx_ms < c->last_rx_ms))
                c = &conns[i];
        if (!c) {
            c = &conns[0];
            for (unsigned i = 1; i < CONNS; i++)
                if (conns[i].last_rx_ms < c->last_rx_ms)
                    c = &conns[i];
        }
        pthread_mutex_lock(&places_mu);
        close_conn(c, "a new connection replaces it");
        pthread_mutex_unlock(&places_mu);
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* a radio that stops reading mid-answer (it lost WiFi during an image) must not hold
       this thread, which also serves the other radios and takes new connections */
    struct timeval tv = {5, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    c->fd = s;
    c->place = -1;
    c->ip = peer_ip(from);
    c->last_rx_ms = now_ms();
    esl_nlink_rx_init(&c->rx, c->rx_buf, sizeof(c->rx_buf));
    esl_channel_state_t cs;
    esl_store_channel_state(&ap.store, now_ms(), &cs);
    esl_nlink_host_init(&c->hl, NULL, esl_radio_tagfw(), cs.channel, ap.pan, net_write, c, now_ms);
    c->hl.on_hello = on_hello;
    c->hl.hello_ctx = c;
    ap_log("radio connected from %u.%u.%u.%u", c->ip >> 24, c->ip >> 16 & 0xff, c->ip >> 8 & 0xff, c->ip & 0xff);
}

static void receive(conn_t *c)
{
    uint8_t buf[1460];
    ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            return;
        pthread_mutex_lock(&places_mu);
        close_conn(c, n == 0 ? "closed by the radio" : strerror(errno));
        pthread_mutex_unlock(&places_mu);
        return;
    }
    c->last_rx_ms = now_ms();
    int i = c->place;
    uint32_t syncs = i >= 0 ? places[i].host.stats.syncs : 0;
    uint64_t t0 = now_ms();
    take_heard(); /* the own radio's latest before the logs */
    if (!esl_nlink_feed(&c->rx, buf, (size_t)n, on_frame, c)) {
        pthread_mutex_lock(&places_mu);
        close_conn(c, c->hl.broken ? "write failed" : "bad frame");
        pthread_mutex_unlock(&places_mu);
        return;
    }
    i = c->place; /* set by the HELLO in this data */
    if (i < 0)
        return;
    place_t *p = &places[i];
    links[i].seen_ms = now_ms();
    if (p->host.stats.syncs != syncs) {
        /* a sync moves tags between radios: the counts of those connected (forget() may be
           clearing one that is not) */
        for (unsigned k = 0; k < RADIOS; k++)
            if (!k || places[k].conn >= 0)
                links[k].tags = esl_rowners_count(&owners, (uint8_t)k);
        ap_debug("radio %d: sync %" PRIu32 " answered in %" PRIu64 " ms (log %u)", i, p->host.stats.syncs,
                 now_ms() - t0, c->hl.req.log_n);
        unsigned n_tags = esl_store_count(&ap.store);
        if (!p->full_warned && n_tags > c->hl.radio.max_tags) {
            p->full_warned = true;
            ap_log("%u tags, but radio %d holds %u: tags beyond that get no content from it", n_tags, i,
                   c->hl.radio.max_tags);
        }
    }
}

/* DELETE /api/radios/{id}, in the HTTP server's thread under the API's lock: 0, 404 or 409 */
static int forget(void *ctx, uint64_t id)
{
    (void)ctx;
    if (id == pub[0].id)
        return 409; /* the AP's own radio */
    pthread_mutex_lock(&places_mu);
    int r = 404;
    for (unsigned i = 1; i < RADIOS && r == 404; i++) {
        place_t *p = &places[i];
        if (p->id != id)
            continue;
        if (p->conn >= 0) {
            r = 409;
            break;
        }
        ap_log("radio %016" PRIX64 " forgotten (place %u)", id, i);
        p->id = 0;
        p->channel = 0;
        p->ready = false;
        links[i] = (esl_radio_link_t){.down_ms = now_ms()};
        pub[i] = links[i]; /* the API's lock is held: this request's own reply shows it gone */
        forgotten |= 1u << i; /* its tags leave the owners in the netradio thread */
        esl_store_set_radio_channel(&ap.store, (uint8_t)i, 0);
        save_places();
        r = 0;
    }
    pthread_mutex_unlock(&places_mu);
    return r;
}

static void *net_thread(void *arg)
{
    int ls = (int)(intptr_t)arg;
    uint64_t last_stats = now_ms();
    for (;;) {
        struct pollfd pf[CONNS + 1];
        int idx[CONNS + 1];
        unsigned n = 0;
        pf[n] = (struct pollfd){.fd = ls, .events = POLLIN};
        idx[n++] = -1;
        for (unsigned i = 0; i < CONNS; i++)
            if (conns[i].fd >= 0) {
                pf[n] = (struct pollfd){.fd = conns[i].fd, .events = POLLIN};
                idx[n++] = (int)i;
            }
        int r = poll(pf, n, 1000);
        take_heard();
        if (r > 0 && pf[0].revents & POLLIN) {
            struct sockaddr_storage from;
            socklen_t fl = sizeof(from);
            int s = accept(ls, (struct sockaddr *)&from, &fl);
            if (s >= 0)
                take(s, &from);
        }
        for (unsigned k = 1; r > 0 && k < n; k++)
            if (pf[k].revents && conns[idx[k]].fd == pf[k].fd)
                receive(&conns[idx[k]]);
        uint64_t now = now_ms();
        pthread_mutex_lock(&places_mu);
        for (unsigned i = 0; i < CONNS; i++)
            if (conns[i].fd >= 0 && now - conns[i].last_rx_ms > (conns[i].place < 0 ? HELLO_S : IDLE_S) * 1000ull)
                close_conn(&conns[i], conns[i].place < 0 ? "no HELLO" : "silent");
        pthread_mutex_unlock(&places_mu);
        publish();
        if (now - last_stats > 600000) {
            last_stats = now;
            log_stats();
        }
    }
    return NULL;
}

bool netradio_start(const char *addr)
{
    /* a port alone: every address */
    char a[300];
    snprintf(a, sizeof(a), "%s%s", strchr(addr, ':') ? "" : ":", addr);
    int ls = ap_listen("netradio", a);
    if (ls < 0)
        return false;
    max_tags = esl_store_slots(&ap.store);
    if (max_tags < AP_MAX_TAGS)
        max_tags = AP_MAX_TAGS; /* sent[] covers every slot the store can grow to */
    img_cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    esl_rowner_t *by_slot = calloc(max_tags, sizeof(*by_slot));
    if (!by_slot) {
        close(ls);
        return false;
    }
    esl_rowners_init(&owners, by_slot, max_tags);
    for (unsigned i = 0; i < CONNS; i++) {
        conns[i].fd = -1;
        conns[i].place = -1;
    }
    for (unsigned i = 0; i < RADIOS; i++)
        places[i].conn = -1;
    load_places();
    /* place 0: the AP's own radio, always there; its id is the AP id, which tells it apart in
       GET /api/radios (DELETE answers 409) */
    links[0] = (esl_radio_link_t){.up = true, .local = true, .seen_ms = ap_now_ms(),
                                  .id = ap.store.ap_id ? ap.store.ap_id : 1};
    for (unsigned i = 1; i < RADIOS; i++) {
        links[i].id = places[i].id;
        links[i].channel = places[i].channel;
    }
    owners.radios = 1;
    memcpy(pub, links, sizeof(pub));
    static const esl_radio_ops_t ops = {NULL, forget};
    ap.api.link = &pub[0];
    ap.api.links = pub;
    ap.api.n_links = RADIOS;
    ap.api.radio_ops = &ops;
    started = true;
    pthread_t t;
    if (pthread_create(&t, NULL, net_thread, (void *)(intptr_t)ls) != 0) {
        started = false;
        close(ls);
        return false;
    }
    pthread_detach(t);
    unsigned known = 0;
    for (unsigned i = 1; i < RADIOS; i++)
        known += places[i].id != 0;
    ap_log("network radios on %s: %u of %d places known", a, known, RADIOS - 1);
    return true;
}
