/* fake_hal.c - hal.h for the PC tests: radio with a queue of replies, fixed MAC and battery. */
#include <string.h>
#include "hal/hal.h"
#include "fake.h"

uint8_t fake_tx[FAKE_MAX_FRAMES][128];
uint8_t fake_tx_len[FAKE_MAX_FRAMES];
int fake_tx_count;
int fake_rx_calls;
int fake_radio_on;
uint8_t fake_channel;
uint8_t fake_ap_channel;
uint8_t fake_tx_channel[FAKE_MAX_FRAMES];

static uint8_t rxq[FAKE_MAX_FRAMES][128];
static uint8_t rxq_len[FAKE_MAX_FRAMES];
static int rxq_head, rxq_tail;

uint8_t fake_mac[8] = { 0x38, 0x3B, 0xCB, 0xE5, 0x97, 0x01, 0x00, 0x00 };
uint16_t fake_batt_mv = 2950;
unsigned fake_delay_ms_total;

void fake_radio_reset(void)
{
    fake_tx_count = 0;
    fake_rx_calls = 0;
    rxq_head = rxq_tail = 0;
    fake_delay_ms_total = 0;
}

void fake_rx_push(const uint8_t *frame, uint8_t len)
{
    if (rxq_tail >= FAKE_MAX_FRAMES)
        return;
    memcpy(rxq[rxq_tail], frame, len);
    rxq_len[rxq_tail] = len;
    rxq_tail++;
}

void fake_rx_lose(void)
{
    if (rxq_tail < FAKE_MAX_FRAMES)
        rxq_len[rxq_tail++] = 0;
}

void hal_init(void) {}
void hal_delay_ms(uint8_t ms) { fake_delay_ms_total += ms; }
void hal_wdt_kick(void) {}
void hal_uart_putc(char c) { (void)c; }
void hal_uart_puts(const char *s) { (void)s; }
void hal_mac_read(uint8_t *mac8) { memcpy(mac8, fake_mac, 8); }
uint16_t battery_read_mv(void) { return fake_batt_mv; }
void power_sleep_s(uint16_t s) { (void)s; }
bool fake_doze_ok = true;
int fake_doze_calls;
bool hal_doze_1s(void)
{
    if (!fake_doze_ok)
        return false;
    fake_doze_calls++;
    return true;
}
uint8_t fake_rx_rssi = 0xC8, fake_rx_lqi = 0x6E;
static uint8_t last_rssi, last_lqi;
uint8_t radio_rx_rssi(void) { return last_rssi; }
uint8_t radio_rx_lqi(void) { return last_lqi; }
int8_t hal_temp_c(void) { return 20; }

uint8_t fake_store[HAL_STORE_MAX];
uint8_t fake_store_len;
int fake_store_writes;
bool fake_store_radio_on_write;


void fake_store_clear(void)
{
    fake_store_len = 0;
    fake_store_writes = 0;
    fake_store_radio_on_write = false;
}

bool hal_store_write(const uint8_t *data, uint8_t len)
{
    if (len > HAL_STORE_MAX)
        return false;
    if (fake_radio_on)
        fake_store_radio_on_write = true;
    memcpy(fake_store, data, len);
    fake_store_len = len;
    fake_store_writes++;
    return true;
}

uint8_t hal_store_read(uint8_t *data, uint8_t maxlen)
{
    if (!fake_store_len || fake_store_len > maxlen)
        return 0;
    memcpy(data, fake_store, fake_store_len);
    return fake_store_len;
}

void radio_init(uint8_t channel) { fake_channel = channel; }
void radio_on(void) { fake_radio_on = 1; }
void radio_off(void) { fake_radio_on = 0; }

bool radio_tx(const uint8_t *frame, uint8_t len)
{
    if (!fake_radio_on)
        return false;
    if (fake_tx_count < FAKE_MAX_FRAMES) {
        memcpy(fake_tx[fake_tx_count], frame, len);
        fake_tx_len[fake_tx_count] = len;
        fake_tx_channel[fake_tx_count] = fake_channel;
    }
    fake_tx_count++;
    return true;
}

int8_t radio_rx(uint8_t *buf, uint8_t maxlen, uint8_t timeout_ms)
{
    uint8_t n;
    fake_rx_calls++;
    if (!fake_radio_on || rxq_head >= rxq_tail || (fake_ap_channel && fake_channel != fake_ap_channel)) {
        fake_delay_ms_total += timeout_ms;
        return -1;
    }
    n = rxq_len[rxq_head];
    if (n == 0) {                       /* fake_rx_lose(): this answer never arrives */
        rxq_head++;
        fake_delay_ms_total += timeout_ms;
        return -1;
    }
    if (n > maxlen)
        n = maxlen;
    memcpy(buf, rxq[rxq_head], n);
    rxq_head++;
    last_rssi = fake_rx_rssi;
    last_lqi = fake_rx_lqi;
    return (int8_t)n;
}
