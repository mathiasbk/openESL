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
#define HAL_STORE_MAX 16  /* the settings record is 15 bytes (app.c) */
bool hal_store_write(const uint8_t *data, uint8_t len);
uint8_t hal_store_read(uint8_t *data, uint8_t maxlen);

/* ---- SPI flash for firmware updates (docs/tag-ota.md; W25Q80, 1 MB, on the 1.54") ----
 * hal_xflash_open wakes the chip from deep power-down and returns false if no chip
 * answers; hal_xflash_close puts it back to sleep. The rest need it open. Addresses are
 * 24-bit; hal_xflash_write programs bytes inside one 256-byte page that was erased
 * (hal_xflash_erase erases the 4 KB sector at addr). */
bool hal_xflash_open(void);
void hal_xflash_close(void);
void hal_xflash_read(uint32_t addr, uint8_t __xdata *buf, uint16_t len);
bool hal_xflash_erase(uint32_t addr);
bool hal_xflash_write(uint32_t addr, const uint8_t __xdata *buf, uint16_t len);

/* Reads the chip's own flash (the running application), for the rollback copy. */
void hal_code_read(uint16_t addr, uint8_t __xdata *buf, uint16_t len);
/* Restarts the chip, which runs the bootloader. Does not return on the tag. */
void hal_reset(void);

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
/* The radio's RSSI byte and LQI (0..127) of the last frame radio_rx returned (0 before the first). */
uint8_t radio_rx_rssi(void);
uint8_t radio_rx_lqi(void);

/* ---- Power ---- */
/* Deep sleep for s seconds. Radio and display must be off. */
void power_sleep_s(uint16_t s);
/* About a second of deep sleep while the display refreshes (its pins keep their idle
   levels, BUSY is read again after it). The radio must be off. False when it cannot be
   done now; the caller then waits awake. */
bool hal_doze_1s(void);

#endif /* HAL_H */
