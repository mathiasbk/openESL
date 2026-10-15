#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esl_api.h"
#include "esl_layout.h"
#include "esl_store.h"

/* store_nvs.c: the tag table with a FreeRTOS mutex and NVS persistence */
esp_err_t store_nvs_init(esl_store_t *st, unsigned cap, uint16_t next_poll_s);
/* The layout files per model, same mutex; call after store_nvs_init(). */
esp_err_t layouts_nvs_init(esl_layouts_t *ls);
/* Reads a string from NVS namespace "cfg", falling back to def. */
void cfg_get_str(const char *key, const char *def, char *out, size_t out_len);

/* wifi.c: joins ssid, or opens the setup network when there is none or it
 * does not answer. token (token_cap bytes) is the API token buffer, which the
 * setup page may change. Never fails: WiFi trouble leaves the mode "off". */
const esl_wifi_ops_t *wifi_start(const char *ssid, const char *password, const char *hostname, char *token,
                                 size_t token_cap);

/* http_api.c. api_handle() is esl_api_handle() under a mutex shared by the
 * HTTP and serial transports; call api_handle_init() before either starts. */
void api_handle_init(void);
int api_handle(const esl_api_t *api, const char *method, const char *uri, const char *auth,
               const char *body, size_t body_len, uint64_t now_ms, esl_sb_t *out);
esp_err_t http_api_start(const esl_api_t *api);

/* serial_api.c: the same API over the USB serial port */
esp_err_t serial_api_start(const esl_api_t *api);

/* radio_ieee802154.c (AP or sniffer mode) or radio_stub.c. ls (layout files,
 * sent to the tags in blocks) may be NULL. */
esp_err_t radio_start(esl_store_t *st, esl_layouts_t *ls, uint8_t channel, uint16_t pan_id);
