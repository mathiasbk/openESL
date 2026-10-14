#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esl_api.h"
#include "esl_store.h"

/* store_nvs.c: the tag table with a FreeRTOS mutex and NVS persistence */
esp_err_t store_nvs_init(esl_store_t *st, unsigned cap, uint16_t next_poll_s);
/* Reads a string from NVS namespace "cfg", falling back to def. */
void cfg_get_str(const char *key, const char *def, char *out, size_t out_len);

/* wifi.c */
esp_err_t wifi_start(const char *ssid, const char *password, const char *hostname);

/* http_api.c */
esp_err_t http_api_start(const esl_api_t *api);

/* radio_ieee802154.c (AP or sniffer mode) or radio_stub.c */
esp_err_t radio_start(esl_store_t *st, uint8_t channel, uint16_t pan_id);
