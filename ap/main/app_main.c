#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "radio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "app";

static esl_store_t store;
static esl_api_t api;
static char ssid[33], password[65], token[129], hostname[33];

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
    ESP_LOGI(TAG, "OpenESL AP %s, PAN 0x%04X, max %d tags", version, CONFIG_ESL_PAN_ID, CONFIG_ESL_MAX_TAGS);
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
    cfg_get_str("hostname", "", hostname, sizeof(hostname));
    if (!hostname[0]) {
        /* A new AP gets the suffix of its setup network, so two APs on one network don't share
           a name; one that already has a network keeps the old name its users know. Saved, so
           the name stays when the network is set later. */
        uint8_t mac[6];
        if (!ssid[0] && esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK)
            snprintf(hostname, sizeof(hostname), "%.27s-%02x%02x", CONFIG_ESL_MDNS_HOSTNAME, mac[4], mac[5]);
        else
            snprintf(hostname, sizeof(hostname), "%s", CONFIG_ESL_MDNS_HOSTNAME);
        cfg_set_str("hostname", hostname);
    }
    if (!token[0])
        ESP_LOGE(TAG, "No API token set (setup page, menuconfig or NVS cfg/token): every HTTP API request gets 401");

    api.wifi = wifi_start(ssid, password, hostname, token, sizeof(token));

#if CONFIG_ESL_MAX_SCHEDULED
    api.sched = sched_start(&store, CONFIG_ESL_MAX_SCHEDULED);
    api.clock = sched_clock();
    store_nvs_set_clock(api.sched);
#endif
    api.store = &store;
    /* GET /api/tags?since= (#270): a since from before this restart gets the whole list */
    static esl_api_changes_t changes;
    changes.boot = esp_random();
    api.changes = &changes;
    api.token = token;
    static esl_api_lock_t lock;
    api.lock = &lock;
    api.fw_version = version;
    api.reset_reason = why;
#if CONFIG_ESL_RADIO_DRIVER_NET
    api.link = radio_net_link();
    api.links = radio_net_links(&api.n_links);
    api.radio_ops = radio_net_ops();
#elif !CONFIG_ESL_RADIO_STUB
    api.radio_stats = &radio_link.stats;
#endif
#if !CONFIG_ESL_RADIO_STUB && !CONFIG_ESL_RADIO_SNIFFER
    api.tx_power = radio_tx_power();
    api.channel_settable = true; /* radio.c and radio_net.c move it (esl_store_channel_step) */
#endif
#if CONFIG_ESL_WEB_DASHBOARD
    api.dashboard = true;
#endif
    /* A pointer per model; each stored file takes its JSON and its compiled size from the heap. */
    api.layouts = calloc(1, sizeof(esl_layouts_t));
    if (!api.layouts)
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(layouts_nvs_init(api.layouts));
    /* Content that does not fit the stored files is held back, as after an upload (#186) */
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
        unsigned bad = esl_store_check_layout(&store, api.layouts, m, NULL, NULL);
        if (bad)
            ESP_LOGW(TAG, "%u tags of model %s have content that does not fit its layout file", bad, esl_model_name(m));
    }
#if CONFIG_ESL_DATASOURCES
    api.sources = datasrc_start(&store, api.layouts, api.sched);
#endif
    api.tagfw = tagfw_start();
    esl_radio_set_tagfw(api.tagfw);
    /* Images for tags that show what the AP draws: two buffers of the largest screen
       (about 10 KB), drawn when a tag needs one and dropped again (esl_image.h). Only
       the radio task uses them for now, so no lock. Without the memory the AP still
       works, but those tags get no new content. */
    static esl_image_t images;
    uint8_t *image_mem = malloc(esl_image_mem_size());
    if (image_mem) {
        esl_image_init(&images, image_mem, NULL);
        esl_radio_set_image(&images);
        /* Drawn after the reply, not in the tag's 20 ms listening window (krav A3) */
        esl_radio_set_draw_later(true);
    } else {
        ESP_LOGE(TAG, "no memory for the image buffers (%u bytes)", (unsigned)esl_image_mem_size());
    }
    api_handle_init();
#if CONFIG_ESL_WEBHOOK
    api.notify = health_start(&api);
#endif
    ESP_ERROR_CHECK(http_api_start(&api));
#if CONFIG_ESL_SERIAL_API
    ESP_ERROR_CHECK(serial_api_start(&api));
#endif

#if CONFIG_ESL_DISPLAY || CONFIG_ESL_RADIO_DRIVER_UART
    err = display_start(&api);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "display: %s", esp_err_to_name(err)); /* the AP works without it */
#endif

    esl_channel_state_t ch;
    esl_store_channel_state(&store, 0, &ch);
    ESP_ERROR_CHECK(radio_start(&store, api.layouts, ch.channel, CONFIG_ESL_PAN_ID));
    /* The tag table is the one thing that grows with the number of tags (store_nvs.c
       stops it before WiFi and HTTP run short); this shows how much
       room is left for it. */
    ESP_LOGI(TAG, "free heap %lu bytes, tag table %u slots of %u bytes", (unsigned long)esp_get_free_heap_size(),
             esl_store_slots(&store), (unsigned)sizeof(esl_tag_t));
#if CONFIG_ESL_OTA
    /* Everything started. A new image with a WiFi network to join is kept once
       it can be reached (wifi.c, api_handle()), otherwise it is good enough now;
       the bootloader goes back to the previous one if it is not kept. */
    ota_watch_running(ssid[0] != 0);
#endif
}
