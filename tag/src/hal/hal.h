/*
 * hal.h - everything the app logic needs from the hardware.
 *
 * Implemented in hal/zbs243/ (the real tag) and in test/fake_hal.c (PC tests).
 * The ZBS243 registers are not publicly documented. Each function in hal/zbs243/
 * is a stub until the register facts are measured or found, and every fact that
 * is used must be listed in docs/kildelogg.md.
 */
#ifndef HAL_H
#define HAL_H

#include "../compat.h"

/* ---- Start-up, delay, watchdog (hal_clock, hal_timer) ---- */
void hal_init(void);                 /* clock, pins, watchdog */
void hal_delay_ms(uint8_t ms);
void hal_wdt_kick(void);

/* ---- Debug UART (hal_uart). Output only. ---- */
void hal_uart_putc(char c);
void hal_uart_puts(const char *s);

/* ---- Identity and battery ---- */
/* The tag's 64-bit MAC in the order it is sent over the air (least significant byte first). */
void hal_mac_read(uint8_t *mac8);
uint16_t battery_read_mv(void);
/* Chip temperature in whole degrees C (factory calibrated; 20 if no calibration). */
int8_t hal_temp_c(void);

/* ---- Persistent store: one small record in flash (requirement T12, settings) ----
 * hal_store_write erases and rewrites the whole record (max HAL_STORE_MAX bytes);
 * the radio must be off. hal_store_read returns its length, or 0 if there is none
 * or it is damaged. */
#define HAL_STORE_MAX 200
bool hal_store_write(const uint8_t *data, uint8_t len);
uint8_t hal_store_read(uint8_t *data, uint8_t maxlen);

/* ---- Second store: the layout file (requirement T12), one flash page ----
 * Raw bytes, no header: the file carries its own CRC-32 and structure, which the app
 * checks on reading. hal_layout_store_write erases the page and writes len bytes
 * (1..HAL_LAYOUT_STORE_MAX) from XRAM; the radio must be off. hal_layout_store_read
 * copies the first len bytes of the page (an erased page reads as 0xFF). */
#define HAL_LAYOUT_STORE_MAX 1024
bool hal_layout_store_write(const uint8_t __xdata *data, uint16_t len);
void hal_layout_store_read(uint8_t __xdata *data, uint16_t len);

/* ---- Radio (802.15.4) ----
 * radio_init only remembers the channel; the radio stays off (nothing drawn
 * from the battery) until radio_on, and radio_off turns it off again. The app
 * turns it on for each exchange only, so the display is never refreshed with
 * the radio on. */
void radio_init(uint8_t channel);
void radio_on(void);
void radio_off(void);
/* Sends a frame without FCS (the radio adds FCS if it can, otherwise radio.c does). */
bool radio_tx(const uint8_t *frame, uint8_t len);
/* Waits up to timeout_ms for a frame with a correct FCS. Returns length without FCS, or -1. */
int8_t radio_rx(uint8_t *buf, uint8_t maxlen, uint8_t timeout_ms);

/* ---- Power ---- */
/* Deep sleep for s seconds. Radio and display must be off. */
void power_sleep_s(uint16_t s);

#endif /* HAL_H */
