#include "sdkconfig.h"
#include "ble.h"
#include "esl_ble.h"
#include "esl_gicisky.h"
#include "esl_persist.h"
#include "esl_proto.h"
#include "esl_push.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "os/os_mbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Gicisky 2.9" BWRY tag (docs/gicisky-296-bwry.md): FEF0 service, FEF1 commands and
 * status, FEF2 image chunks. The central negotiates an MTU large enough for the 244-byte
 * chunk writes, discovers the two value handles and the FEF1 CCCD, enables notifications
 * and leaves the rest to esl_gicisky_send. The push task runs one round at a time; the
 * NimBLE host task signals it through the semaphores and queues below. */

static const char *TAG = "ble";

#define BLE_CONNECT_TIMEOUT_MS 15000
#define BLE_DISC_TIMEOUT_MS    10000
#define BLE_WRITE_TIMEOUT_MS   5000
#define BLE_NOTIFY_TIMEOUT_MS  5000
#define BLE_MTU_MIN            247  /* 3 bytes of ATT header + the 244-byte chunk */

#define GICISKY_COMPANY_ID     0x5053
#define GICISKY_MODEL_BYTE     0x2E

void ble_store_config_init(void);

typedef struct {
    uint16_t len;
    uint8_t data[16];
} notify_t;

typedef struct {
    uint64_t mac;
    ble_addr_t addr;
    int8_t rssi;
    uint16_t battery_mv;
    uint8_t model;
} scan_report_t;

typedef struct {
    uint8_t own_addr_type;
    uint16_t conn_handle;
    bool connected;
    bool busy;
    bool scanning;
    uint16_t svc_start, svc_end;
    uint16_t fef1_val, fef2_val, fef1_cccd, fef2_cccd;
    uint16_t mtu;
    int write_status;
    uint64_t cur_mac; /* the tag being connected to, for nim_info */
    SemaphoreHandle_t conn_sem, disc_sem, setup_sem, write_sem, mtu_sem;
    QueueHandle_t notify_q, scan_q;
    uint8_t fb[ESL_GICISKY_FB_SIZE];
} ble_state_t;

static ble_state_t nim;
static esl_ble_t ble;

/* What the scan last heard from each tag: its address (with the address type, which a
 * MAC alone does not carry) and signal. One entry per tag, so several tags in range do
 * not overwrite each other's address. Only the push task reads and writes it. */
typedef struct {
    uint64_t mac;
    ble_addr_t addr;
    int8_t rssi;
    uint16_t battery_mv;
} heard_t;

static heard_t heard[ESL_PUSH_TAGS];
static unsigned heard_next;

static heard_t *heard_get(uint64_t mac)
{
    for (unsigned i = 0; i < ESL_PUSH_TAGS; i++)
        if (heard[i].mac == mac)
            return &heard[i];
    return NULL;
}

static void heard_put(const scan_report_t *r)
{
    heard_t *h = heard_get(r->mac);
    if (!h) {
        for (unsigned i = 0; i < ESL_PUSH_TAGS && !h; i++)
            if (!heard[i].mac)
                h = &heard[i];
        if (!h)
            h = &heard[heard_next++ % ESL_PUSH_TAGS];
    }
    h->mac = r->mac;
    h->addr = r->addr;
    h->rssi = r->rssi;
    h->battery_mv = r->battery_mv;
}

static int gap_event(struct ble_gap_event *event, void *arg);
static void nim_disconnect(void *ctx);

static struct {
    esl_store_t *st;
    esl_layouts_t *ls;
    esl_image_t *im;
} push_cfg;

static uint64_t mac_of(const ble_addr_t *a)
{
    uint64_t m = 0;
    for (int i = 5; i >= 0; i--)
        m = (m << 8) | a->val[i];
    return m;
}

static void addr_of(uint64_t mac, ble_addr_t *a)
{
    a->type = BLE_ADDR_PUBLIC;
    for (int i = 0; i < 6; i++) {
        a->val[i] = (uint8_t)mac;
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

static void drain(SemaphoreHandle_t s)
{
    while (xSemaphoreTake(s, 0) == pdTRUE)
        ;
}

static void start_scan(void)
{
    ble_state_t *s = &nim;
    if (s->busy || s->scanning)
        return;
    struct ble_gap_disc_params p = {0};
    p.filter_duplicates = 0;
    p.passive = 1;
    int rc = ble_gap_disc(s->own_addr_type, BLE_HS_FOREVER, &p, gap_event, s);
    if (rc == 0 || rc == BLE_HS_EALREADY)
        s->scanning = true;
    else
        ESP_LOGW(TAG, "scan: %d", rc);
}

static void handle_disc(const struct ble_gap_disc_desc *d)
{
    uint8_t model = 0;
    for (unsigned i = 0; i + 1 < d->length_data;) {
        uint8_t l = d->data[i];
        if (l == 0 || i + 1 + l > d->length_data)
            break;
        uint8_t type = d->data[i + 1];
        const uint8_t *p = d->data + i + 2;
        if (type == 0xFF && l >= 4) {
            uint16_t company = (uint16_t)(p[0] | (p[1] << 8));
            if (company == GICISKY_COMPANY_ID && p[2] == GICISKY_MODEL_BYTE)
                model = ESL_MODEL_294;
        }
        i += l + 1;
    }
    if (!model)
        return;
    scan_report_t r = {
        .mac = mac_of(&d->addr),
        .addr = d->addr,
        .rssi = d->rssi,
        .battery_mv = 0,
        .model = model,
    };
    xQueueSend(nim.scan_q, &r, 0);
}

static int mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    (void)conn_handle;
    ble_state_t *s = arg;
    if (error->status == 0)
        s->mtu = mtu;
    xSemaphoreGive(s->mtu_sem);
    return 0;
}

static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                         void *arg)
{
    (void)conn_handle;
    (void)attr;
    ble_state_t *s = arg;
    s->write_status = error->status;
    xSemaphoreGive(s->setup_sem);
    return 0;
}

static int cccd2_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                          void *arg)
{
    (void)conn_handle;
    (void)attr;
    ble_state_t *s = arg;
    if (error->status != 0)
        ESP_LOGW(TAG, "FEF2 CCCD write: %d", error->status);
    s->write_status = 0; /* FEF1 is on; that is the one the transfer needs */
    xSemaphoreGive(s->setup_sem);
    return 0;
}

/* FEF1's CCCD is written; FEF2's (when the tag has one) follows, as the maker's app does.
 * The setup result is FEF1's: the tag answers every step there. */
static int cccd1_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                          void *arg)
{
    (void)attr;
    ble_state_t *s = arg;
    if (error->status != 0 || !s->fef2_cccd)
        return cccd_write_cb(conn_handle, error, attr, arg);
    uint8_t v[2] = {1, 0};
    s->write_status = -1;
    if (ble_gattc_write_flat(conn_handle, s->fef2_cccd, v, sizeof v, cccd2_write_cb, s) != 0) {
        s->write_status = 0; /* FEF1 is on; FEF2 notifications are not needed */
        xSemaphoreGive(s->setup_sem);
    }
    return 0;
}

static int write_cb(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    ble_state_t *s = arg;
    s->write_status = error->status;
    xSemaphoreGive(s->write_sem);
    return 0;
}

static int dsc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                       const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle;
    ble_state_t *s = arg;
    if (error->status == 0 && dsc) {
        /* A CCCD belongs to the characteristic whose value handle is the closest below it.
           Only the first one after each value counts: the search runs to the end of the
           service, so it also passes the CCCDs of the characteristics after it */
        if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
            uint16_t h = dsc->handle;
            bool fef2 = h > s->fef2_val && (s->fef2_val > s->fef1_val || h < s->fef1_val);
            bool fef1 = !fef2 && h > s->fef1_val;
            if (fef1 && !s->fef1_cccd)
                s->fef1_cccd = h;
            else if (fef2 && !s->fef2_cccd)
                s->fef2_cccd = h;
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (!s->fef1_cccd) {
            xSemaphoreGive(s->setup_sem);
            return 0;
        }
        uint8_t v[2] = {1, 0};
        s->write_status = -1;
        int rc = ble_gattc_write_flat(conn_handle, s->fef1_cccd, v, sizeof v, cccd1_write_cb, s);
        if (rc != 0) {
            ESP_LOGW(TAG, "CCCD write: %d", rc);
            xSemaphoreGive(s->setup_sem);
        }
    }
    return 0;
}

static int chr_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr,
                       void *arg)
{
    ble_state_t *s = arg;
    if (error->status == 0 && chr) {
        uint16_t u = ble_uuid_u16(&chr->uuid.u);
        if (u == 0xFEF1)
            s->fef1_val = chr->val_handle;
        else if (u == 0xFEF2)
            s->fef2_val = chr->val_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (!s->fef1_val || !s->fef2_val) {
            xSemaphoreGive(s->setup_sem);
            return 0;
        }
        uint16_t from = s->fef1_val < s->fef2_val ? s->fef1_val : s->fef2_val;
        int rc = ble_gattc_disc_all_dscs(conn_handle, from, s->svc_end, dsc_disc_cb, s);
        if (rc != 0) {
            ESP_LOGW(TAG, "descriptor discovery: %d", rc);
            xSemaphoreGive(s->setup_sem);
        }
    }
    return 0;
}

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc,
                       void *arg)
{
    ble_state_t *s = arg;
    if (error->status == 0 && svc) {
        s->svc_start = svc->start_handle;
        s->svc_end = svc->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (!s->svc_start) {
            xSemaphoreGive(s->setup_sem);
            return 0;
        }
        int rc = ble_gattc_disc_all_chrs(conn_handle, s->svc_start, s->svc_end, chr_disc_cb, s);
        if (rc != 0) {
            ESP_LOGW(TAG, "characteristic discovery: %d", rc);
            xSemaphoreGive(s->setup_sem);
        }
    }
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    ble_state_t *s = arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (!s->busy)
            handle_disc(&event->disc);
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s->conn_handle = event->connect.conn_handle;
            s->connected = true;
        } else {
            s->connected = false;
            ESP_LOGW(TAG, "%012llX: connection failed: %d", (unsigned long long)s->cur_mac,
                     event->connect.status);
        }
        xSemaphoreGive(s->conn_sem);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s->connected = false;
        s->conn_handle = BLE_HS_CONN_HANDLE_NONE;
        xSemaphoreGive(s->disc_sem);
        if (!s->busy)
            start_scan();
        return 0;
    case BLE_GAP_EVENT_MTU:
        s->mtu = event->mtu.value;
        xSemaphoreGive(s->mtu_sem);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        notify_t n;
        n.len = (uint16_t)OS_MBUF_PKTLEN(event->notify_rx.om);
        if (n.len > sizeof n.data)
            n.len = sizeof n.data;
        if (n.len)
            os_mbuf_copydata(event->notify_rx.om, 0, n.len, n.data);
        xQueueSend(s->notify_q, &n, 0);
        return 0;
    }
    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset: %d", reason);
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "no BLE address");
        return;
    }
    if (ble_hs_id_infer_auto(0, &nim.own_addr_type) != 0) {
        ESP_LOGE(TAG, "no BLE address type");
        return;
    }
    start_scan();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool nim_connect(void *ctx, uint64_t mac)
{
    ble_state_t *s = ctx;
    if (!s || s->busy || s->connected)
        return false;
    s->busy = true;
    if (s->scanning) {
        ble_gap_disc_cancel();
        s->scanning = false;
    }

    ble_addr_t addr;
    const heard_t *h = heard_get(mac);
    if (h)
        addr = h->addr;
    else
        addr_of(mac, &addr); /* not heard yet (CONFIG_ESL_BLE_TAG_MAC): assume public */
    s->cur_mac = mac;

    drain(s->conn_sem);
    drain(s->disc_sem);
    drain(s->setup_sem);
    drain(s->mtu_sem);
    drain(s->write_sem);
    xQueueReset(s->notify_q); /* a late notification from the last tag must not answer this one */
    s->connected = false;
    s->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s->svc_start = s->svc_end = 0;
    s->fef1_val = s->fef2_val = s->fef1_cccd = s->fef2_cccd = 0;
    s->write_status = -1;

    int rc = ble_gap_connect(s->own_addr_type, &addr, BLE_CONNECT_TIMEOUT_MS, NULL, gap_event, s);
    if (rc != 0) {
        ESP_LOGW(TAG, "%012llX: connect: %d", (unsigned long long)mac, rc);
        s->busy = false;
        start_scan();
        return false;
    }
    if (xSemaphoreTake(s->conn_sem, pdMS_TO_TICKS(BLE_CONNECT_TIMEOUT_MS + 2000)) != pdTRUE || !s->connected) {
        ESP_LOGW(TAG, "%012llX: no connection (address type %u)", (unsigned long long)mac, addr.type);
        if (!s->connected)
            ble_gap_conn_cancel();
        s->busy = false;
        start_scan();
        return false;
    }

    s->mtu = ble_att_mtu(s->conn_handle);
    if (s->mtu < BLE_MTU_MIN) {
        drain(s->mtu_sem);
        rc = ble_gattc_exchange_mtu(s->conn_handle, mtu_cb, s);
        if (rc == 0)
            xSemaphoreTake(s->mtu_sem, pdMS_TO_TICKS(2000));
        s->mtu = ble_att_mtu(s->conn_handle);
    }
    if (s->mtu < BLE_MTU_MIN) {
        ESP_LOGW(TAG, "MTU %u is too small for the image chunks", s->mtu);
        nim_disconnect(s);
        return false;
    }

    static const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(0xFEF0);
    rc = ble_gattc_disc_svc_by_uuid(s->conn_handle, &svc_uuid.u, svc_disc_cb, s);
    if (rc != 0) {
        ESP_LOGW(TAG, "service discovery: %d", rc);
        nim_disconnect(s);
        return false;
    }
    if (xSemaphoreTake(s->setup_sem, pdMS_TO_TICKS(BLE_DISC_TIMEOUT_MS)) != pdTRUE || s->write_status != 0 ||
        !s->fef1_val || !s->fef2_val || !s->fef1_cccd) {
        ESP_LOGW(TAG, "setup failed: FEF1 %u, FEF2 %u, CCCD %u, status %d", s->fef1_val, s->fef2_val,
                 s->fef1_cccd, s->write_status);
        nim_disconnect(s);
        return false;
    }
    ESP_LOGI(TAG, "connected %012llX, MTU %u, CCCD FEF1 0x%04x FEF2 0x%04x", (unsigned long long)mac, s->mtu,
             s->fef1_cccd, s->fef2_cccd);
    return true;
}

static void nim_disconnect(void *ctx)
{
    ble_state_t *s = ctx;
    if (s->connected && s->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        drain(s->disc_sem);
        ble_gap_terminate(s->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        xSemaphoreTake(s->disc_sem, pdMS_TO_TICKS(3000));
    }
    s->connected = false;
    s->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s->busy = false;
    ESP_LOGI(TAG, "disconnected");
    start_scan();
}

static bool nim_write(void *ctx, esl_gicisky_chan_t chan, const uint8_t *data, size_t len)
{
    ble_state_t *s = ctx;
    if (!s || !s->connected || len > 0xFFFF)
        return false;
    uint16_t h = chan == ESL_GICISKY_CMD ? s->fef1_val : s->fef2_val;
    if (!h)
        return false;
    drain(s->write_sem);
    s->write_status = -1;
    int rc = ble_gattc_write_flat(s->conn_handle, h, data, (uint16_t)len, write_cb, s);
    if (rc != 0) {
        ESP_LOGW(TAG, "write: %d", rc);
        return false;
    }
    if (xSemaphoreTake(s->write_sem, pdMS_TO_TICKS(BLE_WRITE_TIMEOUT_MS)) != pdTRUE)
        return false;
    return s->write_status == 0;
}

static bool nim_wait(void *ctx, const uint8_t *prefix, size_t len)
{
    ble_state_t *s = ctx;
    if (!s || !prefix || len > sizeof(((notify_t *)0)->data))
        return false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(BLE_NOTIFY_TIMEOUT_MS);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline)
            return false;
        notify_t n;
        if (xQueueReceive(s->notify_q, &n, deadline - now) != pdTRUE) {
            ESP_LOGW(TAG, "%012llX: no answer starting %02x %02x %02x", (unsigned long long)s->cur_mac,
                     prefix[0], len > 1 ? prefix[1] : 0, len > 2 ? prefix[2] : 0);
            return false;
        }
        if (n.len >= len && memcmp(n.data, prefix, len) == 0)
            return true;
        /* Not the answer this step waits for: logged, so a tag that answers another way
           than the one the protocol was recorded from shows what it said */
        char hex[3 * sizeof n.data + 1];
        size_t o = 0;
        for (unsigned i = 0; i < n.len; i++)
            o += (size_t)snprintf(hex + o, sizeof hex - o, "%02x ", n.data[i]);
        hex[o ? o - 1 : 0] = 0;
        ESP_LOGW(TAG, "%012llX: unexpected notification %s", (unsigned long long)s->cur_mac, hex);
    }
}

static bool nim_info(void *ctx, esl_push_info_t *info)
{
    ble_state_t *s = ctx;
    if (!s || !info)
        return false;
    const heard_t *h = heard_get(s->cur_mac);
    info->battery_mv = h ? h->battery_mv : 0;
    info->rssi = h ? h->rssi : 0;
    return true;
}

/* The push table and the image each tag was last given live in NVS ("esl_push",
 * one blob per table slot), so a restart does not send every picture again. */
#define BLE_PUSH_NS "esl_push"
static nvs_handle_t push_nvs;
static bool push_nvs_ok;

static void push_key(unsigned slot, char key[8])
{
    snprintf(key, 8, "p%u", slot);
}

static void push_restore(esl_push_t *p)
{
    if (!push_nvs_ok)
        return;
    unsigned restored = 0;
    for (unsigned i = 0; i < ESL_PUSH_TAGS; i++) {
        char key[8];
        push_key(i, key);
        uint8_t blob[ESL_PERSIST_PUSH_LEN];
        size_t len = sizeof(blob);
        uint64_t mac;
        uint8_t model;
        uint32_t shown;
        if (nvs_get_blob(push_nvs, key, blob, &len) != ESP_OK ||
            !esl_persist_push_decode(blob, len, &mac, &model, &shown) || !mac)
            continue;
        if (esl_push_get(p, mac) || p->tags[i].mac) {
            /* a stray key from an older table: the tag is kept once */
            nvs_erase_key(push_nvs, key);
            continue;
        }
        esl_push_tag_t *t = &p->tags[i];
        memset(t, 0, sizeof(*t));
        t->mac = mac;
        t->model = model;
        t->shown = shown;
        restored++;
    }
    if (restored)
        ESP_LOGI(TAG, "restored %u tag(s) with the image they show", restored);
}

static void push_save_dirty(esl_push_t *p)
{
    if (!push_nvs_ok)
        return;
    bool wrote[ESL_PUSH_TAGS] = {false};
    bool any = false;
    for (unsigned i = 0; i < ESL_PUSH_TAGS; i++) {
        esl_push_tag_t *t = &p->tags[i];
        if (!t->mac || !t->dirty)
            continue;
        uint8_t blob[ESL_PERSIST_PUSH_LEN];
        size_t n = esl_persist_push_encode(t->mac, t->model, t->shown, blob);
        char key[8];
        push_key(i, key);
        if (nvs_set_blob(push_nvs, key, blob, n) == ESP_OK) {
            wrote[i] = true;
            any = true;
        }
    }
    if (!any)
        return;
    esp_err_t err = nvs_commit(push_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saving the push table: %s", esp_err_to_name(err));
        return;
    }
    for (unsigned i = 0; i < ESL_PUSH_TAGS; i++)
        if (wrote[i])
            esl_push_saved(p, p->tags[i].mac);
}

static void push_task(void *arg)
{
    (void)arg;
    size_t cap = esl_image_mem_size() / ESL_IMAGE_BUFS;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for the push buffer (%u bytes)", (unsigned)cap);
        vTaskDelete(NULL);
        return;
    }
    static esl_push_t push;
    esl_push_out_t out = {&ble, esl_ble_send};
    esl_push_init(&push, &out, buf, cap);
    push_restore(&push);

    uint64_t fixed = parse_mac(CONFIG_ESL_BLE_TAG_MAC);
    if (fixed) {
        esl_push_add(&push, fixed, ESL_MODEL_294, 0);
        ESP_LOGI(TAG, "tag %012llX added from CONFIG_ESL_BLE_TAG_MAC", (unsigned long long)fixed);
    }

    for (;;) {
        scan_report_t r;
        while (xQueueReceive(nim.scan_q, &r, 0) == pdTRUE) {
            if (r.model != ESL_MODEL_294)
                continue;
            heard_put(&r);
            if (CONFIG_ESL_BLE_AUTO_ADD && !esl_push_get(&push, r.mac)) {
                esl_push_add(&push, r.mac, r.model, 0);
                ESP_LOGI(TAG, "tag %012llX added (model %u, address type %u, rssi %d)", (unsigned long long)r.mac,
                         r.model, r.addr.type, r.rssi);
            }
            uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
            esl_push_seen(&push, push_cfg.st, push_cfg.ls, r.mac, r.rssi, r.battery_mv, now);
        }
        uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
        uint32_t sent = push.stats.sent, away = push.stats.away, refused = push.stats.refused;
        unsigned n = esl_push_round(&push, push_cfg.st, push_cfg.ls, push_cfg.im, now, 1);
        if (n) {
            /* max_sends 1: the tag tried this round is the one before the cursor */
            const esl_push_tag_t *t = &push.tags[(push.cursor + ESL_PUSH_TAGS - 1) % ESL_PUSH_TAGS];
            if (push.stats.sent != sent)
                ESP_LOGI(TAG, "%012llX: image sent", (unsigned long long)t->mac);
            else if (push.stats.refused != refused)
                ESP_LOGW(TAG, "%012llX: image refused", (unsigned long long)t->mac);
            else if (push.stats.away != away)
                ESP_LOGW(TAG, "%012llX: not delivered (%u failed tries), next try in %llu s",
                         (unsigned long long)t->mac, t->fails,
                         (unsigned long long)((t->next_ms > now ? t->next_ms - now : 0) / 1000));
        }
        push_save_dirty(&push);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ESL_BLE_ROUND_MS));
    }
}

esp_err_t ble_start(esl_store_t *st, esl_layouts_t *ls, esl_image_t *im)
{
    if (!st || !im)
        return ESP_ERR_INVALID_ARG;

    memset(&nim, 0, sizeof nim);
    nim.conn_sem = xSemaphoreCreateBinary();
    nim.disc_sem = xSemaphoreCreateBinary();
    nim.setup_sem = xSemaphoreCreateBinary();
    nim.write_sem = xSemaphoreCreateBinary();
    nim.mtu_sem = xSemaphoreCreateBinary();
    nim.notify_q = xQueueCreate(16, sizeof(notify_t));
    nim.scan_q = xQueueCreate(16, sizeof(scan_report_t));
    if (!nim.conn_sem || !nim.disc_sem || !nim.setup_sem || !nim.write_sem || !nim.mtu_sem ||
        !nim.notify_q || !nim.scan_q) {
        ESP_LOGE(TAG, "no memory for the BLE queues");
        return ESP_ERR_NO_MEM;
    }

    ble.ctx = &nim;
    ble.fb = nim.fb;
    ble.fb_cap = sizeof nim.fb;
    ble.model = ESL_MODEL_294;
    ble.connect = nim_connect;
    ble.disconnect = nim_disconnect;
    ble.write = nim_write;
    ble.wait = nim_wait;
    ble.info = nim_info;

    push_cfg.st = st;
    push_cfg.ls = ls;
    push_cfg.im = im;

    push_nvs_ok = nvs_open(BLE_PUSH_NS, NVS_READWRITE, &push_nvs) == ESP_OK;
    if (!push_nvs_ok)
        ESP_LOGW(TAG, "no NVS namespace %s: the push table is not kept", BLE_PUSH_NS);

    int rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %d", rc);
        return rc;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_store_config_init();
    nimble_port_freertos_init(host_task);

    if (xTaskCreate(push_task, "ble_push", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the push task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BLE central started, model %s", esl_model_name(ESL_MODEL_294));
    return ESP_OK;
}
