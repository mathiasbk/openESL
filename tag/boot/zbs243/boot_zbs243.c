/*
 * boot_zbs243.c - the bootloader in flash page 0 (0x0000-0x03FF) of the ZBS243.
 *
 * Runs at every reset (not after deep sleep, which resumes where it stopped), does what
 * boot_core.c decides, and jumps to the application at ESL_FW_APP_BASE. An update never
 * rewrites this page; only the jig does.
 *
 * SPI flash pins as in eepromprobe.c: P0.0 SCK, P0.1 MOSI, P0.2 MISO, P1.1 nCS. P1 is
 * written as a whole byte, since the port does not read back what was written
 * (0x80: flash nCS low, display nCS P1.7 high; 0x82: both high). The flash register
 * sequence is the one in hal_zbs243.c (flash_write_page), see docs/kildelogg.md.
 */
#include "../boot_core.h"
#include "proto.h"
#include "hal/zbs243/zbs243.h"

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

static void cmd(uint8_t c)
{
    P1 = 0x80;
    xfer(c);
}

static void cmd_addr(uint8_t c, uint8_t hi, uint16_t lo)
{
    cmd(c);
    xfer(hi);
    xfer((uint8_t)(lo >> 8));
    xfer((uint8_t)lo);
}

#define END() (P1 = 0x82)

void bl_spi_read(uint8_t hi, uint16_t lo, uint16_t len)
{
    uint8_t __xdata *p = boot_buf;

    cmd_addr(0x03, hi, lo);
    while (len--) {
        *p = xfer(0xFF);
        boot_crc_byte(*p++);
    }
    END();
}

void bl_spi_mark(uint8_t hi, uint16_t lo)
{
    cmd(0x06);                  /* write enable */
    END();
    cmd_addr(0x02, hi, lo);     /* page program, one byte */
    xfer(0x00);
    END();
    cmd(0x05);                  /* wait while the status says "busy" (a byte takes microseconds) */
    while (xfer(0xFF) & 0x01)
        ;
    END();
}

void bl_page_write(uint8_t page)
{
    uint16_t src = (uint16_t)boot_buf;

    CLKSPEED = 0x21;
    CFGPAGE = 4;
    SETTINGS |= 0x38;           /* erase the page, then write */
    FWRTHREE = 3;
    FPGNO = page;
    FWRDSTL = 0;
    FWRDSTH = 0;
    FWRLENL = 0xFF;             /* 1024 bytes */
    FWRLENH = 0x03;
    FWRSRCL = (uint8_t)src;
    FWRSRCH = (uint8_t)(src >> 8);
    TRIGGER |= 0x08;
    while (!(TCON2 & 0x08))
        ;
    TCON2 &= (uint8_t)~0x48;
    SETTINGS &= (uint8_t)~0x10;
    CFGPAGE = 0;
    CLKSPEED = 0x01;
}

void main(void)
{
    uint8_t i;

    CFGPAGE = 4;                /* watchdog off: a copy takes a few seconds */
    WDTENA = 0;
    WDTCONF &= (uint8_t)~0x80;
    CFGPAGE = 0;
    CLKSPEED = 0x01;
    P0FUNC = 0;
    P1FUNC = 0;
    P0 = 0;
    P0DIR = (uint8_t)~0x03;     /* SCK and MOSI out, MISO in */
    END();
    P1DIR = (uint8_t)~0x82;

    cmd(0xAB);                  /* wake the SPI flash from deep power-down (3 us) */
    END();
    for (i = 0; i < 50; i++)
        ;
    boot_core_run();
    cmd(0xB9);                  /* and back to deep power-down; the application wakes it */
    END();
    __asm__("ljmp 0x0400");
}
