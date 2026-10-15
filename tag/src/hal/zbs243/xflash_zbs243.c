/*
 * xflash_zbs243.c - the SPI flash (Winbond W25Q80 on the 1.54", docs/kildelogg.md
 * 2026-10-15) and the restart, for firmware updates (hal.h, docs/tag-ota.md).
 *
 * Pins: P0.0 SCK and P0.1 MOSI (shared with the display), P0.2 MISO, P1.1 nCS. SPI
 * mode 0, bit-banged. P1 is written as a whole byte, since the port does not read
 * back what was written: 0x80 = flash nCS low, display nCS (P1.7) high, BS1 (P1.2)
 * low; 0x82 = both nCS high, as gpio_init leaves it. The commands are the standard
 * SPI NOR ones (JEDEC id 0x9F, read 0x03, write enable 0x06, page program 0x02,
 * sector erase 0x20, status 0x05, deep power-down 0xB9 and release 0xAB).
 */
#include "../hal.h"
#include "zbs243.h"

static uint8_t xfer(uint8_t b)
{
    uint8_t i, r = 0;
    for (i = 0; i < 8; i++) {
        P0_1 = (b & 0x80) != 0;
        P0_0 = 1;
        r = (uint8_t)((r << 1) | P0_2);
        P0_0 = 0;
        b <<= 1;
    }
    return r;
}

static void sel(uint8_t c)
{
    P0_0 = 0;
    P1 = 0x80;
    xfer(c);
}

static void desel(void)
{
    P1 = 0x82;
}

static void sel_addr(uint8_t c, uint32_t a)
{
    sel(c);
    xfer((uint8_t)(a >> 16));
    xfer((uint8_t)(a >> 8));
    xfer((uint8_t)a);
}

/* Waits while the chip is busy; erase takes about 25 ms (measured), at most 400. */
static bool wait_ready(void)
{
    uint16_t ms;
    uint8_t s;

    for (ms = 0; ms < 500; ms++) {
        sel(0x05);
        s = xfer(0xFF);
        desel();
        if (!(s & 0x01))
            return true;
        hal_delay_ms(1);
    }
    return false;
}

bool hal_xflash_open(void)
{
    uint8_t maker;

    P0DIR |= 0x04;                    /* MISO in (it is after gpio_init too) */
    sel(0xAB);                        /* release from deep power-down, 3 us */
    desel();
    hal_delay_ms(1);
    sel(0x9F);
    maker = xfer(0xFF);
    desel();
    return maker != 0x00 && maker != 0xFF;
}

void hal_xflash_close(void)
{
    sel(0xB9);
    desel();
}

void hal_xflash_read(uint32_t addr, uint8_t __xdata *buf, uint16_t len)
{
    sel_addr(0x03, addr);
    while (len--)
        *buf++ = xfer(0xFF);
    desel();
}

bool hal_xflash_erase(uint32_t addr)
{
    sel(0x06);
    desel();
    sel_addr(0x20, addr);
    desel();
    return wait_ready();
}

bool hal_xflash_write(uint32_t addr, const uint8_t __xdata *buf, uint16_t len)
{
    sel(0x06);
    desel();
    sel_addr(0x02, addr);
    while (len--)
        xfer(*buf++);
    desel();
    return wait_ready();
}

void hal_code_read(uint16_t addr, uint8_t __xdata *buf, uint16_t len)
{
    const uint8_t __code *p = (const uint8_t __code *)addr;
    while (len--)
        *buf++ = *p++;
}

/* The watchdog (hal_zbs243.c) with a reset value just below overflow: a few ticks later
   the chip restarts and the bootloader runs. */
void hal_reset(void)
{
    CFGPAGE = 4;
    WDTENA = 0;
    WDTPET = 0;
    WDTRSTVALH = 0xFF;
    WDTRSTVALM = 0xFF;
    WDTRSTVALL = 0xF0;
    WDTCONF |= 0x80;
    WDTENA = 1;
    CFGPAGE = 0;
    for (;;)
        ;
}
