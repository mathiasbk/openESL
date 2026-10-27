#include <string.h>
#include "app.h"
#include "esp_coexist.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "radio.h"
#include "sdkconfig.h"

/* The link over the chip's own 802.15.4 radio (ESP32-C6, ESP32-C5): the driver
 * for radio.c (docs/ap-firmware.md, Radio).
 *
 * The receive callback runs in interrupt context: it only copies the frame to
 * radio.c's queue. send() transmits a reply and waits until it is out, sending
 * it again when the radio refused it. */

static const char *TAG = "radio";

static SemaphoreHandle_t tx_idle;
static uint8_t tx_buf[1 + ESL_PSDU_MAX]; /* length byte + PSDU */

/* esp_ieee802154_tx_error_t of the last transmit that failed, -1 = it went out */
static volatile int tx_err = -1;

/* A reply the radio refused (WiFi has the antenna under coexistence, say) is sent again
   while the tag still listens: it waits CFG_RX_TIMEOUT_MS (20 ms) after its POLL or
   request, and its own retry comes only after that. */
#define TX_RETRY_US      12000   /* from the request's arrival */
#define TX_RETRY_GAP_US  700     /* let a WiFi frame or another tag's frame pass */
#define TX_RETRY_MAX     6

/* ---- driver callbacks (weak symbols in esp_ieee802154, ISR context) ---- */

void IRAM_ATTR esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    esl_link_rx_t f;
    f.len = frame[0];
    if (f.len > ESL_PSDU_MAX)
        f.len = ESL_PSDU_MAX;
    memcpy(f.psdu, &frame[1], f.len);
    f.rssi = info->rssi;
    f.channel = info->channel;
    f.rx_us = esp_timer_get_time();
    esp_ieee802154_receive_handle_done(frame);
    radio_rx_from_isr(&f);
}

void IRAM_ATTR esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack, esp_ieee802154_frame_info_t *ack_info)
{
    (void)frame;
    (void)ack_info;
    BaseType_t woken = pdFALSE;
    if (ack)
        esp_ieee802154_receive_handle_done(ack);
    xSemaphoreGiveFromISR(tx_idle, &woken);
    portYIELD_FROM_ISR(woken);
}

void IRAM_ATTR esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    (void)frame;
    BaseType_t woken = pdFALSE;
    tx_err = (int)error;
    xSemaphoreGiveFromISR(tx_idle, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---- the link ---- */

static int link_send(esl_link_t *l, const uint8_t *psdu, size_t len, const esl_link_rx_t *req)
{
    /* The previous reply must be out of the radio before tx_buf is reused. */
    if (xSemaphoreTake(tx_idle, pdMS_TO_TICKS(20)) != pdTRUE)
        ESP_LOGW(TAG, "previous transmit did not finish");
    tx_buf[0] = (uint8_t)len;
    memcpy(&tx_buf[1], psdu, len);
    tx_err = -1;
    esp_err_t err = esp_ieee802154_transmit(tx_buf, false);
    esl_link_sending(l, req, esp_timer_get_time());
    if (err != ESP_OK) {
        xSemaphoreGive(tx_idle);
        return err;
    }
    /* Wait for each try to finish; tx_idle stays given for the next reply */
    int first_err = -1;
    for (int tries = 0;; tries++) {
        if (xSemaphoreTake(tx_idle, pdMS_TO_TICKS(20)) != pdTRUE)
            return ESP_OK; /* no word from the driver; the next send waits for it */
        int e = tx_err;
        if (e < 0) {
            xSemaphoreGive(tx_idle);
            if (tries)
                ESP_LOGW(TAG, "reply sent after %d resends (tx error %d)", tries, first_err);
            return ESP_OK;
        }
        if (first_err < 0)
            first_err = e;
        if (tries >= TX_RETRY_MAX || esp_timer_get_time() + TX_RETRY_GAP_US - req->rx_us > TX_RETRY_US) {
            xSemaphoreGive(tx_idle);
            ESP_LOGW(TAG, "reply not sent (tx error %d) after %d resends", e, tries);
            return ESP_FAIL;
        }
        esp_rom_delay_us(TX_RETRY_GAP_US);
        tx_err = -1;
        err = esp_ieee802154_transmit(tx_buf, false);
        if (err != ESP_OK) {
            xSemaphoreGive(tx_idle);
            return err;
        }
        l->stats.tx_resent++;
    }
}

static int link_set_channel(esl_link_t *l, uint8_t channel)
{
    (void)l;
    esp_err_t err = esp_ieee802154_set_channel(channel);
    return err == ESP_OK ? esp_ieee802154_receive() : err;
}

static int link_start(esl_link_t *l, uint8_t channel, uint16_t pan, uint16_t short_addr, bool promiscuous)
{
    (void)l;
    tx_idle = xSemaphoreCreateBinary();
    if (!tx_idle)
        return ESP_ERR_NO_MEM;
    xSemaphoreGive(tx_idle);

    esp_err_t err = esp_ieee802154_enable();
    if (err != ESP_OK)
        return err;
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    /* Without this the arbiter never shares the radio: 802.15.4 receiving
       keeps WiFi deaf (no scan results, "no AP found", no traffic), and the
       state outlives a USB or software reset. The sniffer runs without WiFi. */
    if (!promiscuous) {
        err = esp_coex_wifi_i154_enable();
        if (err != ESP_OK)
            ESP_LOGE(TAG, "WiFi/802.15.4 coexistence: %s", esp_err_to_name(err));
    }
#endif
    esp_ieee802154_set_channel(channel);
    esp_ieee802154_set_txpower(CONFIG_ESL_TX_POWER_DBM);
    esp_ieee802154_set_panid(pan);
    esp_ieee802154_set_short_address(short_addr);
    esp_ieee802154_set_promiscuous(promiscuous);
    esp_ieee802154_set_rx_when_idle(true);
    return esp_ieee802154_receive();
}

/* The driver takes it at the next transmit */
static int link_set_tx_power(esl_link_t *l, int8_t dbm)
{
    (void)l;
    return esp_ieee802154_set_txpower(dbm);
}

esl_link_t radio_link = {
    .name = "802.15.4",
    .start = link_start,
    .send = link_send,
    .set_channel = link_set_channel,
    .set_tx_power = link_set_tx_power,
};
