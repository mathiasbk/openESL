#include <string.h>
#include "app.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"

static const char *TAG = "wifi";

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected (reason %d), reconnecting", d->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ip = data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&ip->ip_info.ip));
    }
}

esp_err_t wifi_start(const char *ssid, const char *password, const char *hostname)
{
    if (!ssid[0])
        ESP_LOGE(TAG, "No WiFi SSID set (menuconfig or NVS cfg/ssid)");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Power save would delay API replies; the AP runs on USB power anyway. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set(hostname);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS: %s.local", hostname);
    } else {
        ESP_LOGW(TAG, "mDNS failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}
