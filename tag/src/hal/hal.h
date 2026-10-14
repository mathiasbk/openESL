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

/* ---- Radio (802.15.4) ---- */
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
