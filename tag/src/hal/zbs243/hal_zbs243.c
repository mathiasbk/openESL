/*
 * hal_zbs243.c - the hardware layer for ZBS243 (SEM9110).
 *
 * Register facts are from Dmitry Grinberg's ZBS243 reverse engineering, as used in
 * atc1441's ZBS_Flasher examples and the OpenEPaperLink tag firmware (see zbs243.h
 * and docs/kildelogg.md). The code itself is written for this project.
 *
 * No interrupts are used: the delay polls timer 0, the UART polls its TX flag.
 */
#include "../hal.h"
#include "../hal_epd.h"
#include "zbs243.h"

#define TICKS_PER_MS    1333u                  /* 16 MHz / 12 */
#define UART_BRG        138u                   /* 16 MHz / 139 = 115108 baud (115200) */

/* Output pins cannot be read back (zbs243.h), so pins we toggle are not read. */

static void wdt_off(void)
{
    CFGPAGE = 4;
    WDTENA = 0;
    WDTCONF &= (uint8_t)~0x80;
    CFGPAGE = 0;
}

static void gpio_init(void)
{
    P0INTEN = 0;
    P1INTEN = 0;
    P2INTEN = 0;

    /*
     * P0.0 EPD SCK, P0.1 EPD MOSI: GPIO outputs (SPI is bit-banged).
     * P0.6 UART TX (debug pad 4): peripheral function, output.
     */
    P0FUNC = 0x40;
    P0 = 0x00;
    P0DIR = (uint8_t)~(0x01 | 0x02 | 0x40);
    P0PULL = 0x00;

    /* P1.1 EEPROM nCS high, P1.2 EPD BS1 low (4-wire SPI), P1.7 EPD nCS high. */
    P1FUNC = 0x00;
    P1 = 0x82;
    P1DIR = (uint8_t)~(0x02 | 0x04 | 0x80);
    P1PULL = 0x00;

    /* P2.0 EPD nRESET high, P2.1 EPD BUSY input, P2.2 EPD D/C. */
    P2FUNC = 0x00;
    P2 = 0x01;
    P2DIR = (uint8_t)~(0x01 | 0x04);
    P2PULL = 0x00;
}

static void uart_init(void)
{
    UARTBRGH = UART_BRG >> 8;
    UARTBRGL = UART_BRG & 0xFF;
    UARTSTA = 0x12;          /* enable, and mark TX as ready so the first byte goes out */
}

static void timer_init(void)
{
    TCON &= 0x0F;            /* timer 0 and 1 stopped, flags cleared */
    TMOD = 0x31;             /* timer 0: 16-bit; timer 1: stopped */
}

void hal_init(void)
{
    IEN0 = 0;                /* no interrupts in version 1 */
    CLKSPEED = 0x01;         /* 16 MHz crystal */
    CLKEN = 0x01 | 0x20;     /* timers, UART */
    wdt_off();               /* TODO phase 8 (T10): run it with a long timeout */
    gpio_init();
    uart_init();
    timer_init();
}

void hal_delay_ms(uint8_t ms)
{
    while (ms--) {
        TCON &= (uint8_t)~0x30;
        TH0 = (uint8_t)((0x10000u - TICKS_PER_MS) >> 8);
        TL0 = (uint8_t)(0x10000u - TICKS_PER_MS);
        TCON |= 0x10;
        while (!(TCON & 0x20))
            ;
    }
    TCON &= (uint8_t)~0x30;
}

void hal_wdt_kick(void)
{
    CFGPAGE = 4;
    WDTPET = 0;
    CFGPAGE = 0;
}

void hal_uart_putc(char c)
{
    while (!(UARTSTA & 0x02))
        ;
    UARTSTA &= (uint8_t)~0x02;
    UARTBUF = (uint8_t)c;
}

void hal_uart_puts(const char *s)
{
    while (*s)
        hal_uart_putc(*s++);
}

/* Copies len bytes from the info page (offset ofs) to xdata. Flash DMA needs CLKSPEED 0x21. */
static void infopage_read(uint16_t ofs, uint8_t __xdata *dst, uint8_t len)
{
    uint8_t speed = CLKSPEED;
    CLKSPEED = 0x21;
    CFGPAGE = 4;
    SETTINGS = 0x08;
    FWRTHREE = 3;
    FPGNO = 0x80;
    FWRDSTL = (uint8_t)(uint16_t)dst;
    FWRDSTH = (uint8_t)((uint16_t)dst >> 8);
    FWRSRCL = (uint8_t)ofs;
    FWRSRCH = (uint8_t)(ofs >> 8) & 0x03;
    FWRLENL = len - 1;
    FWRLENH = 0;
    TRIGGER |= 0x08;
    while (!(TCON2 & 0x08))
        ;
    TCON2 &= (uint8_t)~0x48;
    SETTINGS &= (uint8_t)~0x10;
    CFGPAGE = 0;
    CLKSPEED = speed;
}

void hal_mac_read(uint8_t *mac8)
{
    /*
     * The MAC is at info page 0x10-0x17, least significant byte first, which is
     * the air order. ZBS_Flasher's "mac -m" writes it there; checked against
     * the flasher's MAC backups (kildelogg 2026-10-14).
     */
    static __xdata uint8_t buf[8];
    uint8_t i;
    infopage_read(0x10, buf, 8);
    for (i = 0; i < 8; i++)
        mac8[i] = buf[i];
}

uint16_t battery_read_mv(void)
{
    return 0;  /* TODO phase 6: ADC / internal reference */
}

void power_sleep_s(uint16_t s)
{
    /* TODO phase 6: deep sleep with timer wake-up. For now: busy wait. */
    while (s--) {
        uint8_t k;
        for (k = 0; k < 4; k++)
            hal_delay_ms(250);
        hal_wdt_kick();
    }
}

/* ---- Display pins: CS P1.7, D/C P2.2, RESET P2.0, BUSY P2.1, SCK P0.0, MOSI P0.1 ---- */
void hal_epd_power(bool on) { (void)on; }   /* no switchable display supply known */
void hal_epd_cs(bool high)   { P1_7 = high; }
void hal_epd_dc(bool high)   { P2_2 = high; }
void hal_epd_rst(bool high)  { P2_0 = high; }
void hal_epd_sck(bool high)  { P0_0 = high; }
void hal_epd_mosi(bool high) { P0_1 = high; }
bool hal_epd_busy(void)      { return P2_1; }   /* SSD-type: high while busy */
