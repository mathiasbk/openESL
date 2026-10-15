#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "app";

static esl_store_t store;
static esl_api_t api;
static char ssid[33], password[65], token[129];

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "poweron";
    case ESP_RST_EXT:        return "ext";
    case ESP_RST_SW:         return "sw";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int_wdt";
    case ESP_RST_TASK_WDT:   return "task_wdt";
    case ESP_RST_WDT:        return "wdt";
    case ESP_RST_DEEPSLEEP:  return "deepsleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                 return "unknown";
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition reformatted, stored tag content is lost");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    const char *version = esp_app_get_description()->version;
    ESP_LOGI(TAG, "OpenESL AP %s, channel %d, PAN 0x%04X, max %d tags",
             version, CONFIG_ESL_CHANNEL, CONFIG_ESL_PAN_ID, CONFIG_ESL_MAX_TAGS);
    const char *why = reset_reason_name(esp_reset_reason());
    if (!strcmp(why, "poweron") || !strcmp(why, "sw") || !strcmp(why, "usb") || !strcmp(why, "ext"))
        ESP_LOGI(TAG, "reset reason: %s", why);
    else
        ESP_LOGW(TAG, "reset reason: %s", why);

    ESP_ERROR_CHECK(store_nvs_init(&store, CONFIG_ESL_MAX_TAGS, CONFIG_ESL_NEXT_POLL_S));

#if CONFIG_ESL_RADIO_SNIFFER
    /* Sniffer only: no WiFi, so nothing competes with the radio. */
    ESP_ERROR_CHECK(radio_start(&store, NULL, CONFIG_ESL_CHANNEL, CONFIG_ESL_PAN_ID));
    return;
#endif

    cfg_get_str("ssid", CONFIG_ESL_WIFI_SSID, ssid, sizeof(ssid));
    cfg_get_str("pass", CONFIG_ESL_WIFI_PASSWORD, password, sizeof(password));
    cfg_get_str("token", CONFIG_ESL_API_TOKEN, token, sizeof(token));
    if (!token[0])
        ESP_LOGE(TAG, "No API token set (setup page, menuconfig or NVS cfg/token): every HTTP API request gets 401");

    api.wifi = wifi_start(ssid, password, CONFIG_ESL_MDNS_HOSTNAME, token, sizeof(token));

    api.store = &store;
    api.token = token;
    api.fw_version = version;
    api.channel = CONFIG_ESL_CHANNEL;
    api.reset_reason = why;
    /* About 19 KB (JSON and compiled binary per model), so from the heap rather than .bss of every build. */
    api.layouts = calloc(1, sizeof(esl_layouts_t));
    if (!api.layouts)
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(layouts_nvs_init(api.layouts));
    api_handle_init();
    ESP_ERROR_CHECK(http_api_start(&api));
#if CONFIG_ESL_SERIAL_API
    ESP_ERROR_CHECK(serial_api_start(&api));
#endif

    ESP_ERROR_CHECK(radio_start(&store, api.layouts, CONFIG_ESL_CHANNEL, CONFIG_ESL_PAN_ID));
}
