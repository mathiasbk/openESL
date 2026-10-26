#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esl_api.h"
#include "esl_image.h"
#include "esl_layout.h"
#include "esl_radio.h"
#include "esl_screen.h"
#include "esl_store.h"

/* store_nvs.c: the tag table with a FreeRTOS mutex and NVS persistence */
esp_err_t store_nvs_init(esl_store_t *st, unsigned max, uint16_t next_poll_s);
/* The clock for the time a tag was last heard (stored with it); call after sched_start(). */
void store_nvs_set_clock(esl_sched_t *sched);
/* Saves every tag's last poll now: before a planned restart (OTA). */
void store_nvs_flush(void);
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
 * HTTP and serial transports; call api_handle_init() before either starts. */
void api_handle_init(void);
int api_handle(const esl_api_t *api, const char *method, const char *uri, const char *auth,
               const char *body, size_t body_len, uint64_t now_ms, esl_sb_t *out);
/* api_handle() for PUT /api/tag-firmware (esl_api_tagfw_piece_uri() true), with a body of
 * len bytes that read(ctx, buf, n) brings in (false when they did not all come): it goes
 * on in pieces of up to piece_cap bytes, each a part of its own, so no buffer holds a
 * whole part. The reply (the last piece's) goes to out, on resp; *got is the bytes read,
 * short of len after an error reply. -1 when read failed. */
typedef bool (*api_read_fn)(void *ctx, char *buf, size_t n);
int api_handle_tagfw_put(const esl_api_t *api, const char *uri, const char *auth, size_t len, api_read_fn read,
                         void *ctx, char *piece, size_t piece_cap, char *resp, size_t resp_cap, esl_sb_t *out,
                         size_t *got);
esp_err_t http_api_start(const esl_api_t *api);

/* serial_api.c: the same API over the USB serial port */
esp_err_t serial_api_start(const esl_api_t *api);

/* display.c (ESL_DISPLAY or ESL_RADIO_DRIVER_UART): the AP's status screen, on a small
 * ST7789 LCD on this board and/or the radio coprocessor's. Call once api is
 * filled in; it refreshes on its own. */
esp_err_t display_start(const esl_api_t *api);

/* radio_uart.c: sends the status screen to the radio coprocessor (SCREEN), which
 * shows it if it has an LCD. Dropped while the radio is not up. */
void radio_screen(const esl_screen_t *s);

/* radio_uart.c: new firmware for the radio coprocessor over the link (link v2),
 * for ota.c. radio_ota_begin() starts an upload of size bytes, radio_ota_write()
 * sends the next len bytes and returns once the radio has written them, and
 * radio_ota_end() has the radio check the image and restart into it. Each
 * returns 0, or an HTTP status with the reason in err. radio_info() writes the
 * radio's JSON object for GET /api/ota: up, version, fw, link, ota, display. */
int radio_ota_begin(uint32_t size, char *err, size_t errlen);
int radio_ota_write(const void *data, size_t len, char *err, size_t errlen);
int radio_ota_end(char *err, size_t errlen);
void radio_info(esl_sb_t *out);

/* health.c (ESL_WEBHOOK): posts the health report (GET /api/health) to the
 * webhook URL whenever it changes. Returns the notify struct for api->notify,
 * NULL if the task could not start. Call once api->store is filled in. */
esl_notify_t *health_start(const esl_api_t *api);

/* ota.c (ESL_OTA): new AP firmware over the API. ota_part() writes len bytes
 * at offset of an image of size bytes to the other app slot and writes the
 * JSON reply (offset 0 starts an upload; the last part selects the slot and
 * restarts the AP after a second). ota_info() writes GET /api/ota.
 * ota_parse_query() reads ?offset=N&total=T from a URI (false when only one
 * is given). ota_watch_running(wait) is called once the AP is up: a new
 * image is kept at once without wait, otherwise once ota_confirm_running()
 * is called (WiFi got an address, an API request was answered) within a few
 * minutes, or the AP goes back to the old one. ota_confirm_running() is cheap
 * after the first call. */
int ota_part(size_t offset, size_t size, const void *data, size_t len, esl_sb_t *out);
int ota_info(esl_sb_t *out);
bool ota_parse_query(const char *uri, size_t *offset, size_t *total);
void ota_confirm_running(void);
void ota_watch_running(bool wait);

/* tagfw.c: the store for tag firmware images (the "tagfw" partition). NULL
 * when the partition table has none. */
esl_tagfw_t *tagfw_start(void);

/* captive.c (ESL_WIFI_CAPTIVE): the DNS server of the setup network, which
 * answers every name with 192.168.4.1 so phones open the setup form by
 * themselves. wifi.c starts it with the setup network and stops it with it. */
void captive_dns_start(void);
void captive_dns_stop(void);

/* radio.c (AP or sniffer mode, over the link of the radio driver) or
 * radio_stub.c. ls (layout files, which the AP draws with) may be NULL. */
esp_err_t radio_start(esl_store_t *st, esl_layouts_t *ls, uint8_t channel, uint16_t pan_id);
#if CONFIG_ESL_RADIO_DRIVER_NET
/* radio_net.c: the link to the network radio, for GET /api/tags ("radio") */
const esl_radio_link_t *radio_net_link(void);
#endif
