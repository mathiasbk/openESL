#include <stdio.h>
#include <string.h>
#include "app.h"
#include "esp_log.h"
#include "esp_random.h"

/* Radio security (esl_radio_set_sec, docs/protokoll.md "Security"): the installation key in
 * NVS (cfg/radio_key, 32 hex digits), made from the chip's random number generator the
 * first time, and "radio_secure" in /api/config (cfg/radio_req, "1" = on). */

static const char *TAG = "radio";

static void save(void *ctx)
{
    esl_radio_sec_t *s = ctx;
    char hex[2 * ESL_SEC_KEY_LEN + 1];
    for (int i = 0; i < ESL_SEC_KEY_LEN; i++)
        snprintf(&hex[2 * i], 3, "%02X", s->key[i]);
    cfg_set_str("radio_key", hex);
    cfg_set_str("radio_req", s->require ? "1" : "");
    ESP_LOGI(TAG, "radio security: only paired tags %s", s->require ? "on" : "off");
}

static bool parse(const char *hex, uint8_t *key)
{
    if (strlen(hex) != 2 * ESL_SEC_KEY_LEN)
        return false;
    for (int i = 0; i < ESL_SEC_KEY_LEN; i++) {
        unsigned v;
        if (sscanf(&hex[2 * i], "%2x", &v) != 1)
            return false;
        key[i] = (uint8_t)v;
    }
    return true;
}

esl_radio_sec_t *radio_sec(void)
{
    static esl_radio_sec_t sec = {.save = save, .ctx = &sec};
    static bool loaded;
    if (loaded)
        return &sec;
    loaded = true;
    char hex[2 * ESL_SEC_KEY_LEN + 1], req[4];
    cfg_get_str("radio_key", "", hex, sizeof(hex));
    cfg_get_str("radio_req", "", req, sizeof(req));
    sec.require = req[0] == '1';
    if (!parse(hex, sec.key) || !esl_radio_sec_has_key(&sec)) {
        do
            esp_fill_random(sec.key, sizeof(sec.key));
        while (!esl_radio_sec_has_key(&sec));
        save(&sec);
        ESP_LOGI(TAG, "made a new radio key");
    }
    return &sec;
}
