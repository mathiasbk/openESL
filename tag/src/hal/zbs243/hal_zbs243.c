/*
 * hal_zbs243.c - the hardware layer for ZBS243 (SEM9110).
 *
 * STUBS. The ZBS243 registers (clock, GPIO, UART, timer, watchdog, ADC, flash)
 * have no official documentation. They are filled in as they are found from
 * measurements, experiments, community research and a backup of the original
 * firmware, and every fact is recorded in docs/kildelogg.md with its source.
 *
 * Only standard 8051 (Intel MCS-51) is used for now: interrupts off (EA = 0).
 */
#include <8051.h>
#include "../hal.h"
#include "../hal_epd.h"

void hal_init(void)
{
    EA = 0;   /* no interrupts in version 1 (MCS-51 standard SFR IE.7) */
    /* TODO phase 1: clock setup, GPIO direction, UART TX, watchdog */
}

void hal_delay_ms(uint8_t ms)
{
    /* TODO phase 1: calibrate against a known clock (logic analyser). Rough loop for now. */
    volatile uint16_t i;
    while (ms--)
        for (i = 0; i < 1000; i++)
            ;
}

void hal_wdt_kick(void)
{
    /* TODO phase 8 (requirement T10): watchdog registers unknown */
}

void hal_uart_putc(char c)
{
    (void)c;  /* TODO phase 1: UART TX on debug pad 4 (see docs/kildelogg.md) */
}

void hal_uart_puts(const char *s)
{
    while (*s)
        hal_uart_putc(*s++);
}

void hal_mac_read(uint8_t *mac8)
{
    /*
     * TODO phase 0: the stock MAC is in flash 0xFC06-0xFC0B (kildelogg 2026-10-10).
     * The byte order, and how they become 8 bytes (00 00 + 6 bytes?), must be confirmed
     * against the flasher's "mac -m" before use. Until then: zeros.
     */
    uint8_t i;
    for (i = 0; i < 8; i++)
        mac8[i] = 0;
}

uint16_t battery_read_mv(void)
{
    return 0;  /* TODO phase 6: ADC / internal reference unknown */
}

void power_sleep_s(uint16_t s)
{
    /* TODO phase 6: deep sleep with timer wake-up unknown. For now: busy wait. */
    while (s--) {
        uint8_t k;
        for (k = 0; k < 4; k++)
            hal_delay_ms(250);
        hal_wdt_kick();
    }
}

/* ---- Display pins (TODO phase 0: measure ZBS243 pin -> FPC) ---- */
void hal_epd_power(bool on) { (void)on; }
void hal_epd_cs(bool high)  { (void)high; }
void hal_epd_dc(bool high)  { (void)high; }
void hal_epd_rst(bool high) { (void)high; }
void hal_epd_sck(bool high) { (void)high; }
void hal_epd_mosi(bool high) { (void)high; }
bool hal_epd_busy(void)     { return false; }
