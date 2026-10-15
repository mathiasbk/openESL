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
/* Writes one there ("" erases it). */
void cfg_set_str(const char *key, const char *val);

/* wifi.c: joins ssid, or opens the setup network when there is none or it
 * does not answer. token (token_cap bytes) is the API token buffer, which the
 * setup page may change. Never fails: WiFi trouble leaves the mode "off". */
const esl_wifi_ops_t *wifi_start(const char *ssid, const char *password, const char *hostname, char *token,
                                 size_t token_cap);

/* sched.c: scheduled content kept in NVS, applied by its own task, and the
 * clock from NTP. Call after wifi_start(). NULL when there is no memory for it. */
esl_sched_t *sched_start(esl_store_t *st, unsigned cap);

/* http_api.c. api_handle() is esl_api_handle() under a mutex shared by the
 * HTTP and serial transports; call api_handle_init() before either starts.
 * api_handle_health() is esl_api_health() under the same mutex. */
void api_handle_init(void);
int api_handle(const esl_api_t *api, const char *method, const char *uri, const char *auth,
               const char *body, size_t body_len, uint64_t now_ms, esl_sb_t *out);
int api_handle_health(const esl_api_t *api, uint64_t now_ms, uint8_t *prev, esl_sb_t *out);
esp_err_t http_api_start(const esl_api_t *api);

/* serial_api.c: the same API over the USB serial port */
esp_err_t serial_api_start(const esl_api_t *api);

/* display.c (ESL_DISPLAY): AP status on a small ST7789 LCD. Call once api is
 * filled in; it refreshes on its own. */
esp_err_t display_start(const esl_api_t *api);

/* health.c (ESL_WEBHOOK): posts the health report (GET /api/health) to the
 * webhook URL whenever it changes. Returns the notify struct for api->notify,
 * NULL if the task could not start. Call once api->store is filled in. */
esl_notify_t *health_start(const esl_api_t *api);

/* radio_ieee802154.c (AP or sniffer mode) or radio_stub.c. ls (layout files,
 * sent to the tags in blocks) may be NULL. */
esp_err_t radio_start(esl_store_t *st, esl_layouts_t *ls, uint8_t channel, uint16_t pan_id);
