#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_json.h"

/* WiFi setup, independent of ESP-IDF (krav A1). The glue in main/wifi.c does
 * the radio work; this part checks what the user sent and writes the pages and
 * JSON, so the host tests can cover it.
 *
 * Three ways in:
 *   - GET/PUT /api/wifi with the API token (and over the USB port, which needs
 *     none); PUT can also set a new token,
 *   - the setup page at /setup, on the AP's own network "openesl-XXXX" (open
 *     while no WiFi is saved, or the saved one has not answered for a while)
 *     and on the normal network; once a token is set, it asks for it,
 *   - NVS cfg/ssid and cfg/pass, or menuconfig, as before. */

#define ESL_WIFI_SSID_MAX  32 /* bytes, as in 802.11 */
#define ESL_WIFI_PASS_MIN  8  /* WPA2-PSK passphrase */
#define ESL_WIFI_PASS_MAX  63
#define ESL_WIFI_TOKEN_MIN 8
#define ESL_WIFI_TOKEN_MAX 128
#define ESL_WIFI_SCAN_MAX  16

typedef enum {
    ESL_WIFI_OFF = 0,   /* WiFi not started (sniffer build, or the radio failed) */
    ESL_WIFI_STA,       /* joining or joined the saved network */
    ESL_WIFI_SETUP,     /* only the setup network: nothing saved */
    ESL_WIFI_STA_SETUP, /* the saved network did not answer: both at once */
} esl_wifi_mode_t;

typedef struct {
    esl_wifi_mode_t mode;
    bool connected;          /* has an IP on the saved network */
    char ssid[ESL_WIFI_SSID_MAX + 1];       /* saved network, "" if none */
    char ip[16];             /* "" when not connected */
    int8_t rssi;             /* of the saved network, when connected */
    char setup_ssid[ESL_WIFI_SSID_MAX + 1]; /* "" when the setup network is off */
    char setup_ip[16];
    unsigned scan_count;     /* networks found by the last scan */
    char scan[ESL_WIFI_SCAN_MAX][ESL_WIFI_SSID_MAX + 1];
    int8_t scan_rssi[ESL_WIFI_SCAN_MAX];
    bool token_set;          /* an API token is set (the token itself is never shown) */
} esl_wifi_status_t;

typedef struct {
    void *ctx;
    void (*status)(void *ctx, esl_wifi_status_t *out);
    /* Saves and joins ssid (pass may be ""). ssid "" forgets the network and
     * goes back to the setup network. Returns 0, or -1 if it could not be saved. */
    int (*set)(void *ctx, const char *ssid, const char *pass);
    /* Saves a new API token; NULL when tokens cannot be changed this way. */
    int (*set_token)(void *ctx, const char *token);
    /* Starts a new scan; the result shows in status() later. May be NULL. */
    void (*scan)(void *ctx);
} esl_wifi_ops_t;

const char *esl_wifi_mode_name(esl_wifi_mode_t m);

/* Checks a network name and passphrase. Returns NULL or an error message. */
const char *esl_wifi_check(const char *ssid, size_t ssid_len, const char *pass, size_t pass_len);

/* Checks a new API token: 8 to 128 printable ASCII characters, no spaces.
 * Returns NULL or an error message. */
const char *esl_wifi_check_token(const char *token, size_t len);

/* Finds key in an application/x-www-form-urlencoded body and decodes it into
 * out (NUL-terminated). Returns the decoded length, -1 if the key is missing,
 * -2 if it does not fit, -3 for a bad %-escape or a NUL byte. */
int esl_form_get(const char *body, size_t len, const char *key, char *out, size_t out_max);

/* The JSON for GET /api/wifi. Never contains the password or the token, only
 * whether a token is set. */
void esl_wifi_write_json(esl_sb_t *out, const esl_wifi_status_t *st);

/* The setup page (GET /setup). msg is an HTML-safe line shown on top, or NULL.
 * token_set says whether the form must ask for the current token. */
void esl_wifi_page(esl_sb_t *out, const esl_wifi_status_t *st, bool token_set, const char *fw, const char *msg);
#define ESL_WIFI_PAGE_SIZE 8192

/* POST /setup with the form fields ssid, pass, token and new_token. When a
 * token is set, the form must carry it in "token"; when none is set, new_token
 * (optional) becomes the token. lock (may be NULL) is the API's lockout
 * against guessing (esl_api_lock_t): a wrong token counts there too, and the
 * form answers 429 while the API is locked. Writes the answer page and
 * returns the HTTP status. */
struct esl_api_lock;
int esl_wifi_setup_post(const esl_wifi_ops_t *ops, const char *token, const char *fw, const char *body, size_t len,
                        struct esl_api_lock *lock, uint64_t now_ms, esl_sb_t *out);
