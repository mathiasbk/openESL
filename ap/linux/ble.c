/* ble.c - --ble hci0: Bluetooth tags with their maker's firmware (docs/ble-tags.md) from the
 * Linux AP, as ap/main/ble.c does with NimBLE on an ESP32. The kernel's Bluetooth stack
 * (BlueZ) is used through its sockets, without libbluetooth or bluetoothd:
 *
 *   scan     a raw HCI socket on the adapter: LE scan parameters and enable, and the
 *            advertising reports, in which a Gicisky tag names its maker and model
 *   connect  an L2CAP socket on the ATT channel (CID 4) to the tag's address: the kernel
 *            makes the LE connection, and this file speaks ATT over it (MTU, discovery of
 *            FEF0/FEF1/FEF2 and the CCCDs, writes with response, notifications)
 *
 * The transfer itself is esl_ble_send / esl_gicisky_send in esl_core, as on the ESP32. One
 * thread runs the scan and the push rounds (esl_push), so the push table needs no lock;
 * the store and the images take their own. The scan is off while a tag is connected, since
 * a controller may refuse a connection while it scans.
 *
 * The raw HCI socket needs CAP_NET_RAW (root, or setcap cap_net_raw,cap_net_admin+eip). It
 * shares the adapter with bluetoothd: both can scan, but a desktop that scans itself may
 * change the scan parameters under it. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "ap.h"
#include "esl_ble.h"
#include "esl_persist.h"
#include "esl_push.h"

/* ---- BlueZ's socket ABI (include/net/bluetooth in the kernel), spelled out ---- */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_L2CAP     0
#define BTPROTO_HCI       1
#define SOL_HCI           0
#define HCI_FILTER        2
#define HCI_CHANNEL_RAW   0
#define HCI_COMMAND_PKT   0x01
#define HCI_EVENT_PKT     0x04
#define BDADDR_LE_PUBLIC  1
#define BDADDR_LE_RANDOM  2
#define ATT_CID           4

struct bt_sockaddr_hci {
    sa_family_t hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

struct bt_hci_filter {
    uint32_t type_mask;
    uint32_t event_mask[2];
    uint16_t opcode;
};

struct bt_sockaddr_l2 {
    sa_family_t l2_family;
    unsigned short l2_psm;
    uint8_t l2_bdaddr[6];     /* little-endian: the last byte of the MAC first */
    unsigned short l2_cid;
    uint8_t l2_bdaddr_type;
};

/* HCI events and LE commands */
#define EVT_CMD_COMPLETE  0x0E
#define EVT_CMD_STATUS    0x0F
#define EVT_LE_META       0x3E
#define LE_ADV_REPORT     0x02
#define OP_LE_SCAN_PARAMS 0x200B
#define OP_LE_SCAN_ENABLE 0x200C

/* ATT (Bluetooth Core, Vol 3 Part F) */
#define ATT_ERROR         0x01
#define ATT_MTU_REQ       0x02
#define ATT_MTU_RSP       0x03
#define ATT_FIND_INFO_REQ 0x04
#define ATT_FIND_INFO_RSP 0x05
#define ATT_FIND_TYPE_REQ 0x06
#define ATT_FIND_TYPE_RSP 0x07
#define ATT_READ_TYPE_REQ 0x08
#define ATT_READ_TYPE_RSP 0x09
#define ATT_WRITE_REQ     0x12
#define ATT_WRITE_RSP     0x13
#define ATT_NOTIFY        0x1B
#define ATT_INDICATE      0x1D
#define ATT_CONFIRM       0x1E
#define ATT_ERR_NOT_FOUND 0x0A

/* as ap/main/ble.c */
#define BLE_CONNECT_TIMEOUT_MS 15000
#define BLE_REQ_TIMEOUT_MS     5000
#define BLE_NOTIFY_TIMEOUT_MS  5000
#define BLE_MTU_MIN            247 /* 3 bytes of ATT header + the 244-byte chunk */
#define GICISKY_COMPANY_ID     0x5053
#define GICISKY_MODEL_BYTE     0x2E
#define BLE_NOTES              16

typedef struct {
    uint16_t len;
    uint8_t data[16];
} note_t;

/* What the scan last heard from a tag: its address type (a MAC alone does not carry it) */
typedef struct {
    uint64_t mac;
    uint8_t addr_type; /* BDADDR_LE_* */
    int8_t rssi;
} heard_t;

typedef struct {
    int att;                  /* the ATT socket of the connected tag, -1 = none */
    uint16_t mtu;
    uint16_t fef1_val, fef2_val, fef1_cccd, fef2_cccd;
    uint64_t cur_mac;
    note_t notes[BLE_NOTES];  /* FEF1 notifications that came while waiting for something else */
    unsigned note_head, note_n;
} att_t;

static struct {
    int hci;                  /* the raw HCI socket, -1 = none */
    int dev;
    bool scanning;
    unsigned round_ms;
    bool auto_add;
    char push_file[300];
    att_t att;
    heard_t heard[ESL_PUSH_TAGS];
    unsigned heard_next;
    esl_ble_t ble;
    esl_push_t push;
    uint8_t fb[ESL_GICISKY_FB_SIZE];
    volatile sig_atomic_t *stop; /* the program's: the thread ends when it is set */
    pthread_t thread;
    bool running;
} lb = {.hci = -1, .att = {.att = -1}};

/* ---- addresses ---- */

static uint64_t mac_of(const uint8_t a[6])
{
    uint64_t m = 0;
    for (int i = 5; i >= 0; i--)
        m = (m << 8) | a[i];
    return m;
}

static void addr_of(uint64_t mac, uint8_t a[6])
{
    for (int i = 0; i < 6; i++) {
        a[i] = (uint8_t)mac;
        mac >>= 8;
    }
}

static uint64_t parse_mac(const char *s)
{
    unsigned v[6];
    if (!s || !*s || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return 0;
    uint64_t m = 0;
    for (int i = 0; i < 6; i++)
        m = (m << 8) | (v[i] & 0xffu);
    return m;
}

static heard_t *heard_get(uint64_t mac)
{
    for (unsigned i = 0; i < ESL_PUSH_TAGS; i++)
        if (lb.heard[i].mac == mac)
            return &lb.heard[i];
    return NULL;
}

static void heard_put(uint64_t mac, uint8_t addr_type, int8_t rssi)
{
    heard_t *h = heard_get(mac);
    for (unsigned i = 0; i < ESL_PUSH_TAGS && !h; i++)
        if (!lb.heard[i].mac)
            h = &lb.heard[i];
    if (!h)
        h = &lb.heard[lb.heard_next++ % ESL_PUSH_TAGS];
    h->mac = mac;
    h->addr_type = addr_type;
    h->rssi = rssi;
}

/* The model a Gicisky advertisement names (its manufacturer data: the maker's company id,
 * then the model byte), 0 when it is not one */
static uint8_t adv_model(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i + 1 < n;) {
        uint8_t l = d[i];
        if (l == 0 || i + 1 + l > n)
            break;
        const uint8_t *p = d + i + 2;
        if (d[i + 1] == 0xFF && l >= 4 && (p[0] | p[1] << 8) == GICISKY_COMPANY_ID && p[2] == GICISKY_MODEL_BYTE)
            return ESL_MODEL_294;
        i += (size_t)l + 1;
    }
    return 0;
}

/* ---- ATT over an L2CAP socket ---- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static int64_t ms_left(int64_t deadline_us)
{
    int64_t left = (deadline_us - ap_clock_us()) / 1000;
    return left > 0 ? left : 0;
}

/* A notification or indication: FEF1's are kept for att_wait (an indication is confirmed) */
static void att_note(att_t *a, const uint8_t *pdu, size_t n)
{
    if (pdu[0] == ATT_INDICATE) {
        uint8_t c = ATT_CONFIRM;
        if (write(a->att, &c, 1) != 1)
            ap_debug("ble: indication not confirmed");
    }
    if (n < 3 || get16(pdu + 1) != a->fef1_val)
        return;
    note_t *t = &a->notes[(a->note_head + a->note_n) % BLE_NOTES];
    if (a->note_n == BLE_NOTES) {
        a->note_head = (a->note_head + 1) % BLE_NOTES; /* the oldest goes */
        a->note_n--;
    }
    t->len = (uint16_t)(n - 3 > sizeof(t->data) ? sizeof(t->data) : n - 3);
    memcpy(t->data, pdu + 3, t->len);
    a->note_n++;
}

/* The next PDU from the tag, until deadline: its length, 0 on a time-out, -1 when the link is
 * gone. A notification is kept (att_note) and returned too, so a waiter sees it at once. */
static int att_recv(att_t *a, uint8_t *pdu, size_t cap, int64_t deadline_us)
{
    for (;;) {
        struct pollfd p = {a->att, POLLIN, 0};
        int r = poll(&p, 1, (int)ms_left(deadline_us));
        if (r == 0)
            return 0;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        ssize_t n = read(a->att, pdu, cap);
        if (n <= 0)
            return -1;
        if (pdu[0] == ATT_NOTIFY || pdu[0] == ATT_INDICATE)
            att_note(a, pdu, (size_t)n);
        return (int)n;
    }
}

/* A request and its answer: the answer's length, -(ATT error code) for an error response to
 * it, or 0 on no answer or a broken link */
static int att_request(att_t *a, const uint8_t *req, size_t n, uint8_t *rsp, size_t cap, uint8_t want)
{
    if (a->att < 0 || write(a->att, req, n) != (ssize_t)n)
        return 0;
    int64_t deadline = ap_clock_us() + BLE_REQ_TIMEOUT_MS * 1000LL;
    for (;;) {
        int r = att_recv(a, rsp, cap, deadline);
        if (r <= 0)
            return 0;
        if (rsp[0] == want)
            return r;
        if (rsp[0] == ATT_NOTIFY || rsp[0] == ATT_INDICATE)
            continue;
        if (rsp[0] == ATT_ERROR && r >= 5 && rsp[1] == req[0])
            return -(int)rsp[4];
        ap_debug("ble: unexpected ATT pdu %02X while waiting for %02X", rsp[0], want);
    }
}

/* The FEF0 service's handles, FEF1/FEF2 value handles and their CCCDs; CCCDs written */
static bool att_setup(att_t *a)
{
    uint8_t q[8], r[600];
    q[0] = ATT_MTU_REQ;
    put16(q + 1, 517);
    int n = att_request(a, q, 3, r, sizeof(r), ATT_MTU_RSP);
    uint16_t server = n >= 3 ? get16(r + 1) : 23;
    a->mtu = server < 517 ? server : 517;
    if (a->mtu < BLE_MTU_MIN) {
        ap_log("ble: %012llX: MTU %u is too small for the image chunks", (unsigned long long)a->cur_mac, a->mtu);
        return false;
    }

    q[0] = ATT_FIND_TYPE_REQ;
    put16(q + 1, 1);
    put16(q + 3, 0xFFFF);
    put16(q + 5, 0x2800); /* primary service */
    uint8_t q2[9];
    memcpy(q2, q, 7);
    put16(q2 + 7, 0xFEF0);
    n = att_request(a, q2, 9, r, sizeof(r), ATT_FIND_TYPE_RSP);
    if (n < 5)
        return false;
    uint16_t start = get16(r + 1), end = get16(r + 3);

    /* characteristics: handle, properties, value handle, 16-bit UUID (7-byte entries) */
    uint16_t chr_h[8] = {0}, chr_v[8] = {0}, chr_u[8] = {0};
    unsigned chrs = 0;
    for (uint16_t from = start; from <= end && chrs < 8;) {
        q[0] = ATT_READ_TYPE_REQ;
        put16(q + 1, from);
        put16(q + 3, end);
        put16(q + 5, 0x2803);
        n = att_request(a, q, 7, r, sizeof(r), ATT_READ_TYPE_RSP);
        if (n < 2 || r[1] < 7)
            break;
        uint16_t last = from;
        for (int i = 2; i + r[1] <= n && chrs < 8; i += r[1]) {
            chr_h[chrs] = get16(r + i);
            chr_v[chrs] = get16(r + i + 3);
            chr_u[chrs] = r[1] == 7 ? get16(r + i + 5) : 0;
            last = chr_v[chrs];
            chrs++;
        }
        if (last >= end)
            break;
        from = (uint16_t)(last + 1);
    }
    for (unsigned i = 0; i < chrs; i++) {
        uint16_t *val = chr_u[i] == 0xFEF1 ? &a->fef1_val : chr_u[i] == 0xFEF2 ? &a->fef2_val : NULL;
        uint16_t *cccd = val == &a->fef1_val ? &a->fef1_cccd : &a->fef2_cccd;
        if (!val)
            continue;
        *val = chr_v[i];
        uint16_t last = i + 1 < chrs ? (uint16_t)(chr_h[i + 1] - 1) : end;
        if (chr_v[i] >= last)
            continue;
        q[0] = ATT_FIND_INFO_REQ;
        put16(q + 1, (uint16_t)(chr_v[i] + 1));
        put16(q + 3, last);
        n = att_request(a, q, 5, r, sizeof(r), ATT_FIND_INFO_RSP);
        for (int j = 2; n >= 2 && r[1] == 1 && j + 4 <= n; j += 4)
            if (get16(r + j + 2) == 0x2902)
                *cccd = get16(r + j);
    }
    if (!a->fef1_val || !a->fef2_val || !a->fef1_cccd) {
        ap_log("ble: %012llX: setup failed: FEF1 %u, FEF2 %u, CCCD %u", (unsigned long long)a->cur_mac, a->fef1_val,
               a->fef2_val, a->fef1_cccd);
        return false;
    }
    /* notifications on FEF1, then FEF2 (when it has a CCCD) as the maker's app does */
    uint16_t cccds[2] = {a->fef1_cccd, a->fef2_cccd};
    for (int i = 0; i < 2; i++) {
        if (!cccds[i])
            continue;
        q[0] = ATT_WRITE_REQ;
        put16(q + 1, cccds[i]);
        put16(q + 3, 0x0001);
        n = att_request(a, q, 5, r, sizeof(r), ATT_WRITE_RSP);
        if (n < 1 && i == 0)
            return false; /* the transfer needs FEF1's; FEF2's is not needed */
    }
    return true;
}

static bool att_write(att_t *a, esl_gicisky_chan_t chan, const uint8_t *data, size_t len)
{
    uint16_t h = chan == ESL_GICISKY_CMD ? a->fef1_val : a->fef2_val;
    uint8_t q[520], r[64];
    if (a->att < 0 || !h || len + 3 > a->mtu || len + 3 > sizeof(q))
        return false;
    q[0] = ATT_WRITE_REQ;
    put16(q + 1, h);
    memcpy(q + 3, data, len);
    return att_request(a, q, len + 3, r, sizeof(r), ATT_WRITE_RSP) > 0;
}

static bool att_wait(att_t *a, const uint8_t *prefix, size_t len)
{
    if (!prefix || len > sizeof(((note_t *)0)->data))
        return false;
    int64_t deadline = ap_clock_us() + BLE_NOTIFY_TIMEOUT_MS * 1000LL;
    for (;;) {
        while (a->note_n) {
            note_t *t = &a->notes[a->note_head];
            a->note_head = (a->note_head + 1) % BLE_NOTES;
            a->note_n--;
            if (t->len >= len && memcmp(t->data, prefix, len) == 0)
                return true;
            ap_log("ble: %012llX: unexpected notification starting %02x", (unsigned long long)a->cur_mac,
                   t->len ? t->data[0] : 0);
        }
        uint8_t pdu[600];
        int r = att_recv(a, pdu, sizeof(pdu), deadline);
        if (r <= 0) {
            ap_log("ble: %012llX: no answer starting %02x", (unsigned long long)a->cur_mac, prefix[0]);
            return false;
        }
    }
}

static void att_reset(att_t *a, int fd, uint64_t mac)
{
    memset(a, 0, sizeof(*a));
    a->att = fd;
    a->cur_mac = mac;
}

/* ---- the scan, on a raw HCI socket ---- */

static bool hci_cmd(uint16_t op, const uint8_t *params, uint8_t n)
{
    uint8_t b[4 + 32];
    b[0] = HCI_COMMAND_PKT;
    put16(b + 1, op);
    b[3] = n;
    memcpy(b + 4, params, n);
    if (lb.hci < 0 || write(lb.hci, b, 4u + n) != 4 + n)
        return false;
    /* its Command Complete (or Status), with the reports that come meanwhile dropped */
    int64_t deadline = ap_clock_us() + 1000000;
    for (;;) {
        struct pollfd p = {lb.hci, POLLIN, 0};
        if (poll(&p, 1, (int)ms_left(deadline)) <= 0)
            return false;
        uint8_t e[260];
        ssize_t r = read(lb.hci, e, sizeof(e));
        if (r < 7 || e[0] != HCI_EVENT_PKT)
            continue;
        if (e[1] == EVT_CMD_COMPLETE && get16(e + 4) == op)
            return e[6] == 0;
        if (e[1] == EVT_CMD_STATUS && r >= 7 && get16(e + 5) == op)
            return e[3] == 0;
    }
}

static void scan_set(bool on)
{
    if (on == lb.scanning || lb.hci < 0)
        return;
    if (on) {
        /* passive, 30 ms of every 100 ms as on the S3 next to 802.15.4 (units of 0.625 ms) */
        uint8_t p[7] = {0x00, 0, 0, 0, 0, 0x00, 0x00};
        put16(p + 1, 160);
        put16(p + 3, 48);
        uint8_t off[2] = {0, 0};
        hci_cmd(OP_LE_SCAN_ENABLE, off, 2);
        if (!hci_cmd(OP_LE_SCAN_PARAMS, p, 7))
            ap_debug("ble: the adapter refused the scan parameters (it keeps its own)");
    }
    uint8_t en[2] = {on ? 1 : 0, 0x00};
    if (hci_cmd(OP_LE_SCAN_ENABLE, en, 2) || !on)
        lb.scanning = on;
    else
        ap_log("ble: the adapter did not start scanning");
}

/* An LE advertising report event: each Gicisky tag in it to the push table */
static void scan_event(const uint8_t *e, size_t n, uint64_t now)
{
    if (n < 5 || e[1] != EVT_LE_META || e[3] != LE_ADV_REPORT)
        return;
    unsigned reports = e[4];
    size_t i = 5;
    for (unsigned k = 0; k < reports && i + 9 <= n; k++) {
        uint8_t addr_type = e[i + 1] == 1 ? BDADDR_LE_RANDOM : BDADDR_LE_PUBLIC;
        uint64_t mac = mac_of(e + i + 2);
        uint8_t len = e[i + 8];
        if (i + 9 + len + 1 > n)
            return;
        int8_t rssi = (int8_t)e[i + 9 + len];
        uint8_t model = adv_model(e + i + 9, len);
        i += 9u + len + 1u;
        if (model != ESL_MODEL_294)
            continue;
        heard_put(mac, addr_type, rssi);
        if (lb.auto_add && !esl_push_get(&lb.push, mac) && esl_push_add(&lb.push, mac, model, 0))
            ap_log("ble: tag %012llX added (rssi %d)", (unsigned long long)mac, rssi);
        esl_push_seen(&lb.push, &ap.store, &ap.layouts, mac, rssi, 0, now);
    }
}

/* ---- the esl_ble_t backend ---- */

static bool lb_connect(void *ctx, uint64_t mac)
{
    (void)ctx;
    scan_set(false);
    const heard_t *h = heard_get(mac);
    uint8_t types[2] = {h ? h->addr_type : BDADDR_LE_PUBLIC, 0};
    if (!h)
        types[1] = BDADDR_LE_RANDOM; /* never heard (--ble-tag): try both */
    for (int t = 0; t < 2 && types[t]; t++) {
        int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
        if (fd < 0)
            break;
        struct bt_sockaddr_l2 a = {.l2_family = AF_BLUETOOTH, .l2_cid = ATT_CID, .l2_bdaddr_type = BDADDR_LE_PUBLIC};
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
            close(fd);
            break;
        }
        addr_of(mac, a.l2_bdaddr);
        a.l2_bdaddr_type = types[t];
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
        if (r != 0 && errno == EINPROGRESS) {
            struct pollfd p = {fd, POLLOUT, 0};
            int err = 0;
            socklen_t el = sizeof(err);
            r = poll(&p, 1, BLE_CONNECT_TIMEOUT_MS) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && !err
                    ? 0
                    : -1;
        }
        if (r != 0) {
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, 0);
        att_reset(&lb.att, fd, mac);
        if (att_setup(&lb.att)) {
            ap_log("ble: connected %012llX, MTU %u", (unsigned long long)mac, lb.att.mtu);
            return true;
        }
        close(fd);
        lb.att.att = -1;
        break;
    }
    ap_log("ble: %012llX: no connection", (unsigned long long)mac);
    scan_set(true);
    return false;
}

static void lb_disconnect(void *ctx)
{
    (void)ctx;
    if (lb.att.att >= 0)
        close(lb.att.att);
    lb.att.att = -1;
    scan_set(true);
}

static bool lb_write(void *ctx, esl_gicisky_chan_t chan, const uint8_t *data, size_t len)
{
    (void)ctx;
    return att_write(&lb.att, chan, data, len);
}

static bool lb_wait(void *ctx, const uint8_t *prefix, size_t len)
{
    (void)ctx;
    return att_wait(&lb.att, prefix, len);
}

static bool lb_info(void *ctx, esl_push_info_t *info)
{
    (void)ctx;
    const heard_t *h = heard_get(lb.att.cur_mac);
    info->rssi = h ? h->rssi : 0;
    return true;
}

/* ---- the push table, kept in DIR/ble-push: one "MAC model shown" line per tag ---- */

static void push_restore(void)
{
    FILE *f = fopen(lb.push_file, "r");
    if (!f)
        return;
    char line[80];
    unsigned n = 0, model;
    unsigned long long mac;
    unsigned long shown;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "%llx %u %lx", &mac, &model, &shown) == 3 && mac &&
            esl_push_add(&lb.push, mac, (uint8_t)model, (uint32_t)shown))
            n++;
    fclose(f);
    if (n)
        ap_log("ble: restored %u tag(s) with the image they show", n);
}

static void push_save(void)
{
    if (!esl_push_unsaved(&lb.push))
        return;
    char tmp[310];
    snprintf(tmp, sizeof(tmp), "%s.new", lb.push_file);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    for (unsigned i = 0; i < ESL_PUSH_TAGS; i++) {
        const esl_push_tag_t *t = &lb.push.tags[i];
        if (t->mac)
            fprintf(f, "%012llX %u %08lX\n", (unsigned long long)t->mac, t->model, (unsigned long)t->shown);
    }
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!ok || rename(tmp, lb.push_file) != 0)
        return;
    for (unsigned i = 0; i < ESL_PUSH_TAGS; i++)
        if (lb.push.tags[i].mac && lb.push.tags[i].dirty)
            esl_push_saved(&lb.push, lb.push.tags[i].mac);
}

/* ---- the thread ---- */

static void *ble_thread(void *arg)
{
    esl_image_t *im = arg;
    uint64_t next_round = 0;
    scan_set(true);
    while (!*lb.stop) {
        uint64_t now = ap_now_ms();
        if (now >= next_round) {
            esl_push_round(&lb.push, &ap.store, &ap.layouts, im, now, 1);
            push_save();
            next_round = ap_now_ms() + lb.round_ms;
        }
        struct pollfd p = {lb.hci, POLLIN, 0};
        if (poll(&p, 1, 200) == 1) {
            uint8_t e[260];
            ssize_t n = read(lb.hci, e, sizeof(e));
            if (n > 0 && e[0] == HCI_EVENT_PKT)
                scan_event(e, (size_t)n, ap_now_ms());
        }
    }
    scan_set(false);
    if (lb.att.att >= 0)
        close(lb.att.att);
    return NULL;
}

bool ble_start(const char *spec, const char *data_dir, const char *tag_mac, bool auto_add, unsigned round_ms,
               esl_image_t *im, volatile sig_atomic_t *stop)
{
    int dev = 0;
    if (spec && *spec && sscanf(spec, "hci%d", &dev) != 1) {
        fprintf(stderr, "openesl-ap: --ble %s: an adapter such as hci0\n", spec);
        return false;
    }
    lb.hci = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
    struct bt_sockaddr_hci a = {AF_BLUETOOTH, (unsigned short)dev, HCI_CHANNEL_RAW};
    if (lb.hci < 0 || bind(lb.hci, (struct sockaddr *)&a, sizeof(a)) != 0) {
        fprintf(stderr, "openesl-ap: --ble hci%d: %s%s\n", dev, strerror(errno),
                errno == EPERM || errno == EACCES ? " (needs root or cap_net_raw,cap_net_admin)" : "");
        if (lb.hci >= 0)
            close(lb.hci);
        lb.hci = -1;
        return false;
    }
    struct bt_hci_filter f = {0};
    f.type_mask = 1u << HCI_EVENT_PKT;
    f.event_mask[EVT_CMD_COMPLETE >> 5] |= 1u << (EVT_CMD_COMPLETE & 31);
    f.event_mask[EVT_CMD_STATUS >> 5] |= 1u << (EVT_CMD_STATUS & 31);
    f.event_mask[EVT_LE_META >> 5] |= 1u << (EVT_LE_META & 31);
    if (setsockopt(lb.hci, SOL_HCI, HCI_FILTER, &f, sizeof(f)) != 0) {
        fprintf(stderr, "openesl-ap: --ble hci%d: the event filter: %s\n", dev, strerror(errno));
        close(lb.hci);
        lb.hci = -1;
        return false;
    }
    lb.dev = dev;
    lb.auto_add = auto_add;
    lb.round_ms = round_ms ? round_ms : 5000;
    lb.stop = stop;
    snprintf(lb.push_file, sizeof(lb.push_file), "%s/ble-push", data_dir);

    lb.ble = (esl_ble_t){
        .ctx = &lb, .fb = lb.fb, .fb_cap = sizeof(lb.fb), .model = ESL_MODEL_294, .connect = lb_connect,
        .disconnect = lb_disconnect, .write = lb_write, .wait = lb_wait, .info = lb_info,
    };
    size_t cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        fprintf(stderr, "openesl-ap: --ble: no memory for the push buffer\n");
        return false;
    }
    esl_push_out_t out = {&lb.ble, esl_ble_send};
    esl_push_init(&lb.push, &out, buf, cap);
    push_restore();
    uint64_t fixed = parse_mac(tag_mac);
    if (fixed && !esl_push_get(&lb.push, fixed) && esl_push_add(&lb.push, fixed, ESL_MODEL_294, 0))
        ap_log("ble: tag %012llX added from --ble-tag", (unsigned long long)fixed);

    if (pthread_create(&lb.thread, NULL, ble_thread, im) != 0) {
        fprintf(stderr, "openesl-ap: --ble: no thread\n");
        return false;
    }
    lb.running = true;
    ap_log("ble: Bluetooth tags on hci%d (Gicisky 2.9\" BWRY)%s", dev, auto_add ? ", adding every one it hears" : "");
    return true;
}

/* After *stop is set: waits for a transfer in progress to end, and the scan is off */
void ble_stop(void)
{
    if (lb.running)
        pthread_join(lb.thread, NULL);
    lb.running = false;
    if (lb.hci >= 0)
        close(lb.hci);
    lb.hci = -1;
}
