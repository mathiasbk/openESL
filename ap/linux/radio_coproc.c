/* radio_coproc.c - --radio uart:/dev/ttyACM0: an ESP32-C6 running the radio firmware (ap/radio/)
 * on a serial port, over the link of docs/ap-radio-link.md, as ap/main/radio_uart.c drives it
 * from an S3. The c6-usb radio board speaks the link on the C6's own USB port (/dev/ttyACM*);
 * the c6 board on its UART pins, through a USB serial adapter (/dev/ttyUSB*) at 921600 baud.
 *
 * The radio forwards every frame it receives as RX (recv() hands it to the radio loop) and
 * sends the replies send() gives it as TX, again while the tag still listens if it has to,
 * and reports TX_DONE. It says HELLO when it starts and after each CONFIG and PING: a HELLO
 * with other settings (the radio restarted) gets the CONFIG again; while no HELLO comes,
 * CONFIG goes out every couple of seconds; while the link is up a PING goes out every few
 * seconds, and a radio that has said nothing for LINK_DEAD_US is configured again. A port
 * that goes away (the C6 unplugged) is opened again every REOPEN_US until it is back. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "ap.h"
#include "esl_coproc.h"

#define CONFIG_EVERY_US (2 * 1000000LL) /* while the link is down */
#define PING_EVERY_US   (5 * 1000000LL) /* while it is up */
#define LINK_DEAD_US    (15 * 1000000LL)
#define REOPEN_US       (2 * 1000000LL) /* while the port is gone */
#define WRITE_WAIT_MS   200             /* a frame that cannot be written in this time is dropped */
#define TX_POWER_DBM    10              /* as CONFIG_ESL_TX_POWER_DBM on the C6 */

static struct {
    int fd;
    char path[200];
    esl_coproc_config_t cfg;
    esl_coproc_parser_t parser;
    uint8_t buf[512];          /* read from the port, not yet fed to the parser */
    size_t pos, len;
    bool up;                   /* HELLO with our CONFIG seen */
    int64_t last_heard_us, last_ping_us, last_read_us, last_stats_us, last_open_us;
    unsigned lost, resent;     /* from TX_DONE, counted into the link's stats at the next send() */
} co = {.fd = -1};

static void port_send(const uint8_t *f, size_t n)
{
    int64_t until = ap_clock_us() + WRITE_WAIT_MS * 1000LL;
    if (co.fd < 0)
        return;
    while (n) {
        ssize_t w = write(co.fd, f, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno == EAGAIN) {
            int64_t left = until - ap_clock_us();
            if (left <= 0) {
                ap_debug("radio: %s takes nothing: frame dropped", co.path);
                return;
            }
            struct pollfd p = {co.fd, POLLOUT, 0};
            poll(&p, 1, (int)(left / 1000) + 1);
            continue;
        }
        if (w <= 0) {
            ap_log("radio: write to %s: %s", co.path, strerror(errno));
            return;
        }
        f += w;
        n -= (size_t)w;
    }
}

static void send_config(void)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    port_send(f, esl_coproc_encode_config(f, &co.cfg));
}

static void on_hello(const esl_coproc_hello_t *h)
{
    bool ours = h->configured && h->cfg.channel == co.cfg.channel && h->cfg.pan == co.cfg.pan &&
                h->cfg.short_addr == co.cfg.short_addr && h->cfg.tx_power == co.cfg.tx_power &&
                h->cfg.flags == co.cfg.flags;
    if (!ours) {
        if (h->version != ESL_COPROC_VERSION)
            ap_log("radio speaks link v%u, this AP v%u", h->version, ESL_COPROC_VERSION);
        ap_log("radio %u.%u %s (%016llX) %s: sending CONFIG", h->fw >> 8, h->fw & 0xFF, h->app_version,
               (unsigned long long)h->ieee_addr, h->configured ? "has other settings" : "started");
        co.up = false;
        send_config();
        return;
    }
    if (!co.up)
        ap_log("radio %u.%u up on channel %u, PAN 0x%04X, %d dBm", h->fw >> 8, h->fw & 0xFF, co.cfg.channel,
               co.cfg.pan, co.cfg.tx_power);
    co.up = true;
}

static void on_tx_done(const esl_coproc_tx_done_t *d)
{
    co.resent += d->resends;
    switch (d->status) {
    case ESL_COPROC_TX_OK:
        if (d->rx_to_tx_us > 5000)
            ap_log("radio: slow reply, %u us from its RX to its transmit", d->rx_to_tx_us);
        break;
    case ESL_COPROC_TX_LATE:
        co.lost++;
        ap_log("radio: reply reached the radio after the tag stopped listening");
        break;
    default:
        co.lost++;
        ap_debug("radio: reply not sent (status %u) after %u resends", d->status, d->resends);
        break;
    }
}

/* One message from the radio; true with f filled in for an RX */
static bool handle(esl_link_rx_t *f)
{
    const esl_coproc_parser_t *p = &co.parser;
    co.last_heard_us = ap_clock_us();
    switch (p->type) {
    case ESL_COPROC_HELLO: {
        esl_coproc_hello_t h;
        if (esl_coproc_decode_hello(p->payload, p->len, &h))
            on_hello(&h);
        return false;
    }
    case ESL_COPROC_RX: {
        esl_coproc_rx_t rx;
        /* not before a HELLO with our settings: what a C6 on USB kept in its buffer while no
           host read it is old, and may be from another channel */
        if (!co.up || !esl_coproc_decode_rx(p->payload, p->len, &rx))
            return false;
        f->len = rx.len;
        memcpy(f->psdu, rx.psdu, rx.len);
        f->rssi = rx.rssi;
        f->channel = rx.channel;
        f->rx_us = ap_clock_us();
        f->ref = rx.ref;
        return true;
    }
    case ESL_COPROC_TX_DONE: {
        esl_coproc_tx_done_t d;
        if (esl_coproc_decode_tx_done(p->payload, p->len, &d))
            on_tx_done(&d);
        return false;
    }
    case ESL_COPROC_STATS: {
        esl_coproc_stats_t s;
        if (esl_coproc_decode_stats(p->payload, p->len, &s))
            ap_log("radio: rx %lu tx %lu failed %lu resent %lu late %lu dropped %lu link errors %lu up %lu s",
                   (unsigned long)s.rx, (unsigned long)s.tx, (unsigned long)s.tx_failed, (unsigned long)s.tx_resent,
                   (unsigned long)s.tx_late, (unsigned long)s.dropped, (unsigned long)s.link_errors,
                   (unsigned long)s.uptime_s);
        return false;
    }
    default:
        return false;
    }
}

/* CONFIG while the link is down, PING while it is up, STATS now and then */
static void keep_link(void)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    int64_t now = ap_clock_us();
    if (co.up && now - co.last_heard_us > LINK_DEAD_US) {
        ap_log("the radio has said nothing for %lld s: configuring it again",
               (long long)((now - co.last_heard_us) / 1000000));
        co.up = false;
        co.last_ping_us = 0;
    }
    if (co.up && now - co.last_ping_us > PING_EVERY_US) {
        co.last_ping_us = now;
        port_send(f, esl_coproc_frame(f, ESL_COPROC_PING, NULL, 0)); /* it answers with HELLO */
    }
    if (!co.up && now - co.last_ping_us > CONFIG_EVERY_US) {
        if (co.last_ping_us)
            ap_log("no answer from the radio on %s", co.path);
        co.last_ping_us = now;
        send_config(); /* it answers with HELLO */
    }
    if (now - co.last_stats_us > 3600 * 1000000LL) {
        co.last_stats_us = now;
        port_send(f, esl_coproc_frame(f, ESL_COPROC_STATS_Q, NULL, 0));
    }
}

static int port_open(void)
{
    /* As it is opened, the system may change DTR and RTS, which can reset a C6 on its USB
       port: the radio then says HELLO and gets its CONFIG again, so nothing is lost but a
       second */
    int fd = open(co.path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        close(fd);
        errno = ENOTTY;
        return -1;
    }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CRTSCTS | HUPCL);
    cfsetispeed(&t, B921600); /* ESL_COPROC_BAUD; the USB port ignores it */
    cfsetospeed(&t, B921600);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        int e = errno; /* close() may change it */
        close(fd);
        errno = e;
        return -1;
    }
    tcflush(fd, TCIFLUSH);
    co.fd = fd;
    co.pos = co.len = 0;
    esl_coproc_parser_init(&co.parser);
    return 0;
}

/* The port went away (unplugged): closed, and opened again by reopen() */
static void port_lost(const char *why)
{
    ap_log("radio: %s %s: opening it again every %lld s", co.path, why, (long long)(REOPEN_US / 1000000));
    close(co.fd);
    co.fd = -1;
    co.up = false;
    co.last_open_us = ap_clock_us();
}

static void reopen(void)
{
    int64_t now = ap_clock_us();
    if (now - co.last_open_us < REOPEN_US)
        return;
    co.last_open_us = now;
    if (port_open() != 0)
        return;
    ap_log("radio: %s is back", co.path);
    co.last_ping_us = 0; /* CONFIG right away */
}

static int co_recv(esl_link_t *l, esl_link_rx_t *f, int timeout_ms)
{
    (void)l;
    int64_t until = ap_clock_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        if (co.fd < 0) {
            reopen();
            if (co.fd < 0) {
                int64_t left = until - ap_clock_us();
                if (left <= 0)
                    return 0;
                usleep((useconds_t)(left < 100000 ? left : 100000));
                continue;
            }
        }
        /* what is read already first: a read can hold several frames */
        while (co.pos < co.len)
            if (esl_coproc_parser_feed(&co.parser, co.buf[co.pos++]) && handle(f))
                return 1;
        keep_link();
        int64_t left = until - ap_clock_us();
        if (left <= 0)
            return 0;
        struct pollfd p = {co.fd, POLLIN, 0};
        int r = poll(&p, 1, (int)(left / 1000) + 1);
        if (r < 0 && errno != EINTR)
            return -1;
        if (r <= 0)
            continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL) && !(p.revents & POLLIN)) {
            port_lost("is gone");
            continue;
        }
        ssize_t n = read(co.fd, co.buf, sizeof(co.buf));
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (n <= 0) {
            port_lost(n ? strerror(errno) : "ended");
            continue;
        }
        int64_t now = ap_clock_us();
        if (now - co.last_read_us > ESL_COPROC_GAP_US)
            esl_coproc_parser_gap(&co.parser); /* the radio restarted mid-frame */
        co.last_read_us = now;
        co.pos = 0;
        co.len = (size_t)n;
    }
}

static int co_send(esl_link_t *l, const uint8_t *psdu, size_t len, const esl_link_rx_t *req)
{
    uint8_t f[ESL_COPROC_FRAME_MAX];
    for (; co.lost; co.lost--)
        esl_link_lost(l);
    l->stats.tx_resent += co.resent;
    co.resent = 0;
    if (!co.up)
        return -1;
    size_t n = esl_coproc_encode_tx(f, (uint8_t)req->ref, psdu, len);
    if (!n)
        return -1;
    esl_link_sending(l, req, ap_clock_us());
    port_send(f, n);
    return 0;
}

static int co_start(esl_link_t *l, uint8_t channel, uint16_t pan, uint16_t short_addr, bool promiscuous)
{
    (void)l;
    co.cfg = (esl_coproc_config_t){.channel = channel, .pan = pan, .short_addr = short_addr, .tx_power = TX_POWER_DBM,
                                   .flags = promiscuous ? ESL_COPROC_CFG_SNIFFER : 0};
    send_config();
    co.last_ping_us = ap_clock_us();
    return 0;
}

static int co_set_channel(esl_link_t *l, uint8_t channel)
{
    (void)l;
    co.cfg.channel = channel;
    send_config(); /* HELLO with the new channel follows */
    return 0;
}

static int co_set_tx_power(esl_link_t *l, int8_t dbm)
{
    (void)l;
    co.cfg.tx_power = dbm;
    send_config();
    return 0;
}

static int co_open(esl_link_t *l, const char *arg)
{
    if (!*arg) {
        fprintf(stderr, "openesl-ap: --radio uart:/dev/ttyACM0 (the serial port of the radio)\n");
        return -1;
    }
    snprintf(co.path, sizeof(co.path), "%s", arg);
    if (port_open() != 0) {
        fprintf(stderr, "openesl-ap: %s: %s\n", co.path, errno == ENOTTY ? "not a serial port" : strerror(errno));
        return -1;
    }
    co.last_stats_us = ap_clock_us();
    l->name = "802.15.4 coprocessor";
    l->start = co_start;
    l->send = co_send;
    l->set_channel = co_set_channel;
    l->set_tx_power = co_set_tx_power;
    l->ctx = &co;
    return 0;
}

static void co_close(esl_link_t *l)
{
    (void)l;
    if (co.fd >= 0)
        close(co.fd);
    co.fd = -1;
}

const radio_backend_t radio_coproc = {
    "uart", "uart:DEVICE      an ESP32-C6 with the radio firmware (ap/radio, board c6-usb) on a serial port",
    co_open, co_recv, co_close,
};
