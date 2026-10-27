#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "esp_log.h"
#include "sdkconfig.h"

/* The radio's transmit power (GET/PUT /api/config "tx_power"): menuconfig's
 * ESL_TX_POWER_DBM until one is set over the API, which is kept in NVS (cfg/tx_dbm)
 * and handed to the radio with radio_set_tx_power() (radio.c, radio_net.c). */

static const char *TAG = "radio";

static void set(void *ctx, int8_t dbm, bool builtin)
{
    (void)ctx;
    char v[8] = "";
    if (!builtin)
        snprintf(v, sizeof(v), "%d", dbm);
    cfg_set_str("tx_dbm", v);
    radio_set_tx_power(dbm);
    ESP_LOGI(TAG, "transmit power %d dBm%s", dbm, builtin ? " (built-in)" : "");
}

esl_tx_power_t *radio_tx_power(void)
{
    static esl_tx_power_t tx = {CONFIG_ESL_TX_POWER_DBM, CONFIG_ESL_TX_POWER_DBM, set, NULL};
    static bool loaded;
    if (!loaded) {
        loaded = true;
        char v[8];
        cfg_get_str("tx_dbm", "", v, sizeof(v));
        char *end;
        long n = strtol(v, &end, 10);
        if (v[0] && !*end && n >= ESL_TX_POWER_MIN && n <= ESL_TX_POWER_MAX)
            tx.dbm = (int8_t)n;
    }
    return &tx;
}
