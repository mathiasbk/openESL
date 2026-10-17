#include <stdio.h>
#include <string.h>
#include "app.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"
#include "nvs.h"
#include "sdkconfig.h"

/* WiFi with a setup network (krav A1).
 *
 * With a network saved (NVS cfg/ssid, or menuconfig) the AP joins it as a
 * station. If it has no IP after CONFIG_ESL_WIFI_SETUP_AFTER_S, or nothing is
 * saved at all, the AP also opens its own network "openesl-XXXX" with the setup
 * page at http://192.168.4.1/setup (http_api.c). It keeps trying the saved
 * network meanwhile, and closes the setup network a minute after it gets an
 * IP. PUT /api/wifi (HTTP or USB port) and the setup page both end in
 * wifi_set() below. */

static const char *TAG = "wifi";

#define SETUP_OFF_AFTER_S 60 /* after joining, so the browser can still load "/setup" */
#define RETRY_IN_SETUP_S  20 /* joining hops channels, which drops phones on the setup network */

static struct {
    portMUX_TYPE mux; /* guards the fields read by status() */
    esl_wifi_mode_t mode;
    bool connected, scanning;
    char ssid[ESL_WIFI_SSID_MAX + 1];
    char pass[ESL_WIFI_PASS_MAX + 1];
    char ip[16];
    char setup_ssid[ESL_WIFI_SSID_MAX + 1];
    unsigned scan_count;
    char scan[ESL_WIFI_SCAN_MAX][ESL_WIFI_SSID_MAX + 1];
    int8_t scan_rssi[ESL_WIFI_SCAN_MAX];
    esp_timer_handle_t fallback, setup_off, retry;
    char *token;
    size_t token_cap;
} w = {.mux = portMUX_INITIALIZER_UNLOCKED};

static void setup_open(void)
{
    if (w.setup_ssid[0])
        return;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char name[ESL_WIFI_SSID_MAX + 1];
    snprintf(name, sizeof(name), "openesl-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, name);
    ap.ap.ssid_len = (uint8_t)strlen(name);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    const char *pw = CONFIG_ESL_WIFI_SETUP_PASSWORD;
    if (strlen(pw) >= 8) {
        strncpy((char *)ap.ap.password, pw, sizeof(ap.ap.password) - 1);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK)
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup network failed: %s", esp_err_to_name(err));
        return;
    }
    taskENTER_CRITICAL(&w.mux);
    strcpy(w.setup_ssid, name);
    w.mode = w.ssid[0] ? ESL_WIFI_STA_SETUP : ESL_WIFI_SETUP;
    taskEXIT_CRITICAL(&w.mux);
    ESP_LOGW(TAG, "setup network \"%s\" open%s: http://192.168.4.1/setup", name,
             ap.ap.authmode == WIFI_AUTH_OPEN ? " (no password)" : "");
#if CONFIG_ESL_WIFI_CAPTIVE
    captive_dns_start();
#endif
}

static void setup_close(void)
{
    if (!w.setup_ssid[0])
        return;
#if CONFIG_ESL_WIFI_CAPTIVE
    captive_dns_stop();
#endif
    esp_wifi_set_mode(WIFI_MODE_STA);
    taskENTER_CRITICAL(&w.mux);
    w.setup_ssid[0] = 0;
    w.mode = ESL_WIFI_STA;
    taskEXIT_CRITICAL(&w.mux);
    ESP_LOGI(TAG, "setup network closed");
}

static void start_scan(void)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err == ESP_OK)
        w.scanning = true;
    else
        ESP_LOGW(TAG, "scan not started: %s", esp_err_to_name(err));
}

static void scan_done(void)
{
    uint16_t n = 24;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    w.scanning = false;
    if (!recs || esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        free(recs);
        esp_wifi_clear_ap_list();
        return;
    }
    /* Records come strongest first; keep each name once. */
    taskENTER_CRITICAL(&w.mux);
    w.scan_count = 0;
    for (unsigned i = 0; i < n && w.scan_count < ESL_WIFI_SCAN_MAX; i++) {
        const char *s = (const char *)recs[i].ssid;
        bool dup = !s[0];
        for (unsigned j = 0; j < w.scan_count && !dup; j++)
            dup = strcmp(w.scan[j], s) == 0;
        if (dup)
            continue;
        strlcpy(w.scan[w.scan_count], s, sizeof(w.scan[0]));
        w.scan_rssi[w.scan_count++] = recs[i].rssi;
    }
    taskEXIT_CRITICAL(&w.mux);
    free(recs);
    ESP_LOGI(TAG, "scan: %u networks", w.scan_count);
}

static void join(void)
{
    if (!w.ssid[0])
        return;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
        ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
}

static void on_timer(void *arg)
{
    esp_timer_handle_t which = *(esp_timer_handle_t *)arg;
    if (which == w.fallback) {
        if (!w.connected) {
            ESP_LOGW(TAG, "\"%s\" not joined after %d s", w.ssid, CONFIG_ESL_WIFI_SETUP_AFTER_S);
            setup_open();
        }
    } else if (which == w.setup_off) {
        if (w.connected)
            setup_close();
    } else if (which == w.retry) {
        join();
    }
}

static void arm(esp_timer_handle_t t, unsigned s)
{
    esp_timer_stop(t);
    esp_timer_start_once(t, (uint64_t)s * 1000000);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        join();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        bool was = w.connected;
        taskENTER_CRITICAL(&w.mux);
        w.connected = false;
        w.ip[0] = 0;
        taskEXIT_CRITICAL(&w.mux);
        if (!w.ssid[0])
            return;
        if (was)
            ESP_LOGW(TAG, "disconnected (reason %d), reconnecting", d->reason);
        if (CONFIG_ESL_WIFI_SETUP_AFTER_S && !w.setup_ssid[0] && !esp_timer_is_active(w.fallback))
            arm(w.fallback, CONFIG_ESL_WIFI_SETUP_AFTER_S);
        if (w.setup_ssid[0])
            arm(w.retry, RETRY_IN_SETUP_S);
        else
            join();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scan_done();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ip = data;
        taskENTER_CRITICAL(&w.mux);
        w.connected = true;
        snprintf(w.ip, sizeof(w.ip), IPSTR, IP2STR(&ip->ip_info.ip));
        taskEXIT_CRITICAL(&w.mux);
        ESP_LOGI(TAG, "connected to \"%s\", IP %s", w.ssid, w.ip);
        esp_timer_stop(w.fallback);
        if (w.setup_ssid[0])
            arm(w.setup_off, SETUP_OFF_AFTER_S);
    }
}

static void nvs_put(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("cfg", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = val[0] ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "saving cfg/%s failed: %s", key, esp_err_to_name(err));
}

/* ---- esl_wifi_ops_t ---------------------------------------------------------- */

static void ops_status(void *ctx, esl_wifi_status_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    wifi_ap_record_t ap;
    bool have_rssi = w.connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    taskENTER_CRITICAL(&w.mux);
    out->mode = w.mode;
    out->connected = w.connected;
    strcpy(out->ssid, w.ssid);
    strcpy(out->ip, w.ip);
    strcpy(out->setup_ssid, w.setup_ssid);
    if (w.setup_ssid[0])
        strcpy(out->setup_ip, "192.168.4.1");
    out->scan_count = w.scan_count;
    memcpy(out->scan, w.scan, sizeof(out->scan));
    memcpy(out->scan_rssi, w.scan_rssi, sizeof(out->scan_rssi));
    taskEXIT_CRITICAL(&w.mux);
    out->rssi = have_rssi ? ap.rssi : 0;
    out->token_set = w.token && w.token[0];
}

static int ops_set(void *ctx, const char *ssid, const char *pass)
{
    (void)ctx;
    if (w.mode == ESL_WIFI_OFF)
        return -1;
    nvs_put("ssid", ssid);
    nvs_put("pass", pass);
    taskENTER_CRITICAL(&w.mux);
    strlcpy(w.ssid, ssid, sizeof(w.ssid));
    strlcpy(w.pass, pass, sizeof(w.pass));
    taskEXIT_CRITICAL(&w.mux);
    esp_timer_stop(w.retry);
    esp_timer_stop(w.setup_off);
    esp_wifi_disconnect();
    if (!ssid[0]) {
        ESP_LOGW(TAG, "network forgotten");
        esp_timer_stop(w.fallback);
        setup_open();
        return 0;
    }
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    taskENTER_CRITICAL(&w.mux);
    w.mode = w.setup_ssid[0] ? ESL_WIFI_STA_SETUP : ESL_WIFI_STA;
    taskEXIT_CRITICAL(&w.mux);
    ESP_LOGI(TAG, "joining \"%s\"", ssid);
    if (CONFIG_ESL_WIFI_SETUP_AFTER_S && !w.setup_ssid[0])
        arm(w.fallback, CONFIG_ESL_WIFI_SETUP_AFTER_S);
    join();
    return 0;
}

static int ops_set_token(void *ctx, const char *token)
{
    (void)ctx;
    if (strlen(token) >= w.token_cap)
        return -1;
    nvs_put("token", token);
    strcpy(w.token, token); /* the caller holds the API mutex */
    ESP_LOGI(TAG, "API token changed");
    return 0;
}

static void ops_scan(void *ctx)
{
    (void)ctx;
    if (w.mode != ESL_WIFI_OFF && !w.scanning)
        start_scan();
}

static const esl_wifi_ops_t ops = {NULL, ops_status, ops_set, ops_set_token, ops_scan};

const esl_wifi_ops_t *wifi_start(const char *ssid, const char *password, const char *hostname, char *token,
                                 size_t token_cap)
{
    w.token = token;
    w.token_cap = token_cap;
    strlcpy(w.ssid, ssid, sizeof(w.ssid));
    strlcpy(w.pass, password, sizeof(w.pass));

    esp_err_t err = esp_netif_init();
    if (err == ESP_OK)
        err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netif: %s", esp_err_to_name(err));
        return &ops; /* mode stays OFF; the API still answers over USB */
    }
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, hostname);
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return &ops;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    /* Credentials live in NVS cfg/ (ours), not in the driver's own copy. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    static esp_timer_handle_t *const handles[] = {&w.fallback, &w.setup_off, &w.retry};
    static const char *const names[] = {"wifi_fallback", "wifi_setup_off", "wifi_retry"};
    for (int i = 0; i < 3; i++) {
        esp_timer_create_args_t ta = {.callback = on_timer, .arg = (void *)handles[i], .name = names[i]};
        ESP_ERROR_CHECK(esp_timer_create(&ta, handles[i]));
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, w.ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, w.pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = w.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    w.mode = ESL_WIFI_STA;
    if (!w.ssid[0]) {
        ESP_LOGW(TAG, "no WiFi network saved");
        setup_open();
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        w.mode = ESL_WIFI_OFF;
        w.setup_ssid[0] = 0;
        return &ops;
    }
    /* Modem sleep is what gives 802.15.4 its turns on the shared radio: with
       WIFI_PS_NONE WiFi holds it and the AP hears no tags. It delays API
       replies a little (up to a beacon interval). */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (!w.ssid[0])
        start_scan();
    else if (CONFIG_ESL_WIFI_SETUP_AFTER_S)
        arm(w.fallback, CONFIG_ESL_WIFI_SETUP_AFTER_S);

    err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set(hostname);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS: %s.local", hostname);
    } else {
        ESP_LOGW(TAG, "mDNS failed: %s", esp_err_to_name(err));
    }
    return &ops;
}
