/* radio_spimodem.c - --radio spi-modem:/dev/spidevB.C: an 802.15.4 modem behind a Linux SPI
 * driver that frames its traffic as records, as some ESL gateways have (docs/linux-ap.md).
 * The modem keeps its own firmware; this backend only sets it up and passes frames:
 *
 *   write  [cmd, 0xFF, len LE16, data...]        settings: 0x83 PAN, 0x85 channel, 0x86 short
 *                                                address, 0x87 IEEE address, 0x8C promiscuous,
 *                                                0x8B receive on/off
 *   write  [0x88, rates, n+1 LE16, frame (n bytes, no FCS), 0xFF ^ XOR(frame)]   transmit;
 *          the modem adds the FCS; rates 0x01 = 250 kbps O-QPSK
 *   write  [0x02] then read 2 bytes               the status of the last transmit (0 = sent) or of
 *                                                receive on, which the modem waits for
 *   read() blocks until frames came in and returns their count (or bytes); each is a
 *          132-byte record [0, len, frame (len bytes, no FCS), rssi, lqi]
 *
 * One file descriptor carries all of it, for the whole run: the driver sets up its receive
 * queue on every open(), so a second open (another program, or this one again) leaves a
 * read() that is waiting stuck for good. The read() cannot be interrupted either, so it runs
 * in a thread of its own that hands frames to recv() through a pipe, while send() and the
 * settings use the same descriptor from the radio loop. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "ap.h"
#include "esl_proto.h"

#define REC_LEN 132
#define REC_MAX 31
#define SPI_HZ 5000000
#define RATE_250K 0x01

static struct {
    int fd;
    int pipe[2];             /* RX thread -> recv(): esl_link_rx_t, one per write */
    char path[200];
    pthread_mutex_t spi;     /* one SPI transfer at a time */
    uint8_t channel;
    uint16_t pan, short_addr; /* as start() set them, for a change of channel */
    bool promisc;            /* ",promisc": hear every frame on the channel */
    bool start_promisc;
    unsigned failed;
} sm = {.fd = -1, .pipe = {-1, -1}, .spi = PTHREAD_MUTEX_INITIALIZER};

/* rx NULL: a write only, which the driver wants for commands and frames; reading back
 * during a transfer is only for the status */
static int xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    struct spi_ioc_transfer t;
    memset(&t, 0, sizeof(t));
    t.tx_buf = (uintptr_t)tx;
    t.rx_buf = (uintptr_t)rx;
    t.len = (uint32_t)n;
    return ioctl(sm.fd, SPI_IOC_MESSAGE(1), &t) < 0 ? -1 : 0;
}

static int cmd(uint8_t c, const uint8_t *data, size_t n)
{
    uint8_t b[16] = {c, 0xFF, (uint8_t)n, 0};
    if (n > sizeof(b) - 4)
        return -1;
    memcpy(b + 4, data, n);
    pthread_mutex_lock(&sm.spi);
    int err = xfer(b, NULL, 4 + n);
    pthread_mutex_unlock(&sm.spi);
    usleep(5000); /* the modem takes a setting before the next command */
    return err;
}

/* The modem's status, 0 = fine; 0xFFFF when it could not be read */
static uint16_t tx_status(void)
{
    uint8_t tx[3] = {0x02, 0, 0}, rx[3] = {0};
    pthread_mutex_lock(&sm.spi);
    int err = xfer(tx, rx, 1);
    usleep(100);
    if (!err)
        err = xfer(tx + 1, rx + 1, 2);
    pthread_mutex_unlock(&sm.spi);
    return err ? 0xFFFF : (uint16_t)(rx[1] | rx[2] << 8);
}

/* Receive on, and the status the modem waits for before it receives: after the set-up and
 * after a change of channel */
static int receive_on(void)
{
    uint8_t p[1] = {1};
    int err = cmd(0x8B, p, 1);
    usleep(100000);
    uint16_t st = tx_status();
    ap_debug("radio: receive on, status %04X", st);
    return err || st == 0xFFFF ? -1 : 0;
}

static int setup(uint16_t pan, uint16_t short_addr, bool promiscuous)
{
    uint8_t p[8] = {(uint8_t)pan, (uint8_t)(pan >> 8)};
    int err = cmd(0x83, p, 2);
    p[0] = sm.channel;
    err |= cmd(0x85, p, 1);
    p[0] = (uint8_t)short_addr, p[1] = (uint8_t)(short_addr >> 8);
    err |= cmd(0x86, p, 2);
    /* The modem wants an IEEE address, but the AP never uses it: tags send to the AP's short
       address and the AP answers with its short address too (docs/protocol.md), so a fixed
       one, locally administered (bit 1 of the first byte), does for every AP */
    static const uint8_t ieee[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0x02};
    err |= cmd(0x87, ieee, 8);
    p[0] = promiscuous || sm.promisc;
    err |= cmd(0x8C, p, 1);
    return err | receive_on();
}

/* ---- the framing, apart from the device so the tests can check it (test_spimodem.c) ---- */

/* The transmit command for a PSDU of len bytes (with its FCS, which the modem makes):
 * its length, 0 if it does not fit */
static size_t tx_frame(uint8_t *b, const uint8_t *psdu, size_t len)
{
    if (len <= ESL_FCS_LEN || len > ESL_PSDU_MAX)
        return 0;
    size_t n = len - ESL_FCS_LEN;
    uint8_t x = 0xFF;
    for (size_t i = 0; i < n; i++)
        x ^= psdu[i];
    b[0] = 0x88;
    b[1] = RATE_250K;
    b[2] = (uint8_t)(n + 1);
    b[3] = (uint8_t)((n + 1) >> 8);
    memcpy(b + 4, psdu, n);
    b[4 + n] = x;
    return 4 + n + 1;
}

/* The frames in what one read() returned (r: a count of records, or bytes): fills up to max
 * of f and returns how many */
static size_t rx_records(const uint8_t *buf, ssize_t r, esl_link_rx_t *f, size_t max)
{
    size_t got = 0, n = r <= REC_MAX ? (size_t)r : (size_t)r / REC_LEN;
    for (size_t k = 0; k < n && k < REC_MAX && got < max; k++) {
        const uint8_t *rec = buf + REC_LEN * k;
        uint8_t len = rec[1];
        if (len < ESL_MHR_LEN || len + ESL_FCS_LEN > ESL_PSDU_MAX || len + 4 > REC_LEN)
            continue;
        memset(&f[got], 0, sizeof(f[got]));
        memcpy(f[got].psdu, rec + 2, len);
        f[got].len = (uint8_t)(len + ESL_FCS_LEN); /* the modem checked and dropped the FCS */
        f[got].rssi = (int8_t)rec[2 + len];
        got++;
    }
    return got;
}

/* ---- receiving ---- */

static void *rx_thread(void *arg)
{
    (void)arg;
    static uint8_t buf[REC_LEN * REC_MAX];
    unsigned errors = 0;
    for (;;) {
        ssize_t r = read(sm.fd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errors++ % 600 == 0) /* the first, then one a minute */
                ap_log("radio: read from %s: %s (%u times)", sm.path, strerror(errno), errors);
            usleep(100000);
            continue;
        }
        errors = 0;
        int64_t now = ap_clock_us();
        static esl_link_rx_t f[REC_MAX];
        size_t n = rx_records(buf, r, f, REC_MAX);
        for (size_t k = 0; k < n; k++) {
            f[k].channel = sm.channel;
            f[k].rx_us = now;
            if (write(sm.pipe[1], &f[k], sizeof(f[k])) != (ssize_t)sizeof(f[k]))
                ap_log("radio: frame dropped");
        }
    }
    return NULL;
}

static int sm_recv(esl_link_t *l, esl_link_rx_t *f, int timeout_ms)
{
    (void)l;
    struct pollfd p = {sm.pipe[0], POLLIN, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (r == 0)
        return 0;
    return read(sm.pipe[0], f, sizeof(*f)) == (ssize_t)sizeof(*f) ? 1 : -1;
}

/* ---- esl_link_t ---- */

static int sm_send(esl_link_t *l, const uint8_t *psdu, size_t len, const esl_link_rx_t *req)
{
    uint8_t b[4 + ESL_PSDU_MAX + 1];
    size_t n = tx_frame(b, psdu, len);
    if (!n)
        return -1;
    esl_link_sending(l, req, ap_clock_us());
    pthread_mutex_lock(&sm.spi);
    int err = xfer(b, NULL, n);
    pthread_mutex_unlock(&sm.spi);
    if (err)
        return -1;
    /* the status only once the frame is out: reading it earlier gives a busy status, and an
       IMG block (about 110 bytes) takes almost 6 ms from the command to the end of its
       transmission (32 us a byte at 250 kbps, and the modem's start) */
    usleep(1500 + 40 * (useconds_t)len);
    uint16_t st = tx_status();
    if (st) {
        if (sm.failed++ < 10)
            ap_log("radio: transmit status %04X", st);
        return -1;
    }
    return 0;
}

static int sm_start(esl_link_t *l, uint8_t channel, uint16_t pan, uint16_t short_addr, bool promiscuous)
{
    (void)l;
    sm.channel = channel;
    sm.pan = pan;
    sm.short_addr = short_addr;
    sm.start_promisc = promiscuous;
    if (setup(pan, short_addr, promiscuous) != 0) {
        fprintf(stderr, "openesl-ap: %s: the modem takes no settings (SPI error, or no status after receive on)\n",
                sm.path);
        return -1;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, rx_thread, NULL) != 0)
        return -1;
    pthread_detach(t);
    return 0;
}

/* A new channel with receiving off and the whole set-up again, as at the start: set while
   receiving, the modem took the channel and heard the next frame, but stopped receiving after
   its first transmit there (tried on the gateway, 25 -> 15) */
static int sm_set_channel(esl_link_t *l, uint8_t channel)
{
    (void)l;
    uint8_t off[1] = {0};
    sm.channel = channel;
    int err = cmd(0x8B, off, 1);
    usleep(100000);
    tx_status(); /* receive off is answered with a status too */
    /* the store has already kept the new channel, so a set-up that fails here would leave the
       modem deaf until a restart: try it a few times */
    for (int tries = 1; tries <= 3; tries++) {
        if (!setup(sm.pan, sm.short_addr, sm.start_promisc))
            return err ? -1 : 0;
        ap_log("spi-modem: set-up on channel %u failed (try %d of 3)", channel, tries);
        usleep(200000);
    }
    return -1;
}

/* At a stop the receive thread may wait in read() until the modem hears something, and that
   read() cannot be interrupted: promiscuous makes any frame on the channel end it */
static void sm_close(esl_link_t *l)
{
    (void)l;
    uint8_t p[1] = {1};
    cmd(0x8C, p, 1);
}

static int sm_open(esl_link_t *l, const char *arg)
{
    if (!*arg) {
        fprintf(stderr, "openesl-ap: --radio spi-modem:/dev/spidevB.C\n");
        return -1;
    }
    snprintf(sm.path, sizeof(sm.path), "%s", arg);
    char *opt = strchr(sm.path, ',');
    if (opt) {
        *opt++ = 0;
        sm.promisc = strcmp(opt, "promisc") == 0;
    }
    sm.fd = open(sm.path, O_RDWR | O_CLOEXEC);
    if (sm.fd < 0) {
        fprintf(stderr, "openesl-ap: %s: %s\n", sm.path, strerror(errno));
        return -1;
    }
    uint8_t mode = SPI_MODE_0, bits = 8;
    uint32_t hz = SPI_HZ;
    if (ioctl(sm.fd, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(sm.fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(sm.fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
        fprintf(stderr, "openesl-ap: %s is not an SPI device: %s\n", sm.path, strerror(errno));
        close(sm.fd);
        return -1;
    }
    if (pipe(sm.pipe) != 0) {
        close(sm.fd);
        return -1;
    }
    l->name = "802.15.4 SPI modem";
    l->start = sm_start;
    l->send = sm_send;
    l->set_channel = sm_set_channel;
    l->ctx = &sm;
    return 0;
}

const radio_backend_t radio_spimodem = {
    "spi-modem", "spi-modem:DEVICE an 802.15.4 modem behind a record-framing SPI driver (docs/linux-ap.md)",
    sm_open, sm_recv, sm_close,
};
