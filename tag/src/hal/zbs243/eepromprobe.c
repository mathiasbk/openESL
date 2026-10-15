/*
 * eepromprobe.c - experiment: is there a SPI flash on P1.1 (nCS), and can we read,
 * erase and write it? Needed to decide how tag OTA stores a new image (docs/tag-ota.md).
 * Not part of the firmware.
 *
 * Pins as in hal_zbs243.c and OEPL: P0.0 SCK, P0.1 MOSI (shared with the display),
 * P0.2 MISO, P1.1 nCS. SPI mode 0, bit-banged.
 *
 * No debug UART on the jig: the output also goes to a log written to flash page 0x3D
 * (0xF400) at the end, read back with the flasher. The write test uses only the last
 * 4 KB sector of the SPI flash.
 */
#include "../hal.h"
#include "zbs243.h"

#define LOG_PAGE 0x3D
static __xdata uint8_t logbuf[1024];
static uint16_t loglen;

static void putc_(char c)
{
    if (loglen < sizeof(logbuf))
        logbuf[loglen++] = (uint8_t)c;
    hal_uart_putc(c);
}

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

static void put_hex(uint8_t v)
{
    static const char __code hex[] = "0123456789ABCDEF";
    putc_(hex[v >> 4]);
    putc_(hex[v & 0x0F]);
}

static void log_save(void)
{
    uint8_t speed = CLKSPEED;
    uint16_t t = 0xFFFF, src = (uint16_t)logbuf;

    CLKSPEED = 0x21;
    CFGPAGE = 4;
    SETTINGS |= 0x38;
    FWRTHREE = 3;
    FPGNO = LOG_PAGE;
    FWRDSTL = 0;
    FWRDSTH = 0;
    FWRLENL = 0xFF;                   /* 1024 bytes */
    FWRLENH = 0x03;
    FWRSRCL = (uint8_t)src;
    FWRSRCH = (uint8_t)(src >> 8);
    TRIGGER |= 0x08;
    while (!(TCON2 & 0x08) && --t)
        ;
    TCON2 &= (uint8_t)~0x48;
    SETTINGS &= (uint8_t)~0x10;
    CFGPAGE = 0;
    CLKSPEED = speed;
}

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

/* Whole-byte writes to P1, no read-modify-write: the port does not read back what was
 * written. 0x80 = EPD nCS (P1.7) high, BS1 (P1.2) low, flash nCS (P1.1) low; 0x82 = both high. */
static void sel(void)   { P0_0 = 0; P1 = 0x80; }
static void desel(void) { P1 = 0x82; }

static void addr(uint32_t a)
{
    xfer((uint8_t)(a >> 16));
    xfer((uint8_t)(a >> 8));
    xfer((uint8_t)a);
}

static __xdata uint8_t buf[16];

/* Command c, then n dummy/address bytes from pre[], then len bytes read into buf. */
static void cmd_read(uint8_t c, const uint8_t *pre, uint8_t npre, uint8_t len)
{
    uint8_t i;
    sel();
    xfer(c);
    for (i = 0; i < npre; i++)
        xfer(pre[i]);
    for (i = 0; i < len; i++)
        buf[i] = xfer(0xFF);
    desel();
}

static void show(const char *what, uint8_t len)
{
    uint8_t i;
    puts_(what);
    for (i = 0; i < len; i++) {
        putc_(' ');
        put_hex(buf[i]);
    }
    puts_("\r\n");
}

static void read_at(uint32_t a, uint8_t len)
{
    uint8_t i;
    sel();
    xfer(0x03);
    addr(a);
    for (i = 0; i < len; i++)
        buf[i] = xfer(0xFF);
    desel();
}

static void simple(uint8_t c)
{
    sel();
    xfer(c);
    desel();
}

/* Waits for WIP (status bit 0) to clear; returns the number of ms waited, 0xFFFF on timeout. */
static uint16_t wait_ready(uint16_t max_ms)
{
    uint16_t ms = 0;
    uint8_t s;
    for (;;) {
        sel();
        xfer(0x05);
        s = xfer(0xFF);
        desel();
        if (!(s & 0x01))
            return ms;
        if (ms++ >= max_ms)
            return 0xFFFF;
        hal_delay_ms(1);
        hal_wdt_kick();
    }
}

static void put_u16(uint16_t v)
{
    put_hex((uint8_t)(v >> 8));
    put_hex((uint8_t)v);
}

static void write_test(uint32_t a)
{
    uint8_t i;
    uint16_t ms;

    puts_("sector ");
    put_hex((uint8_t)(a >> 16));
    put_u16((uint16_t)a);
    puts_("\r\n");
    read_at(a, 16);
    show("before      ", 16);

    simple(0x06);                     /* write enable */
    cmd_read(0x05, 0, 0, 1);
    show("status WEL  ", 1);
    sel();
    xfer(0x20);                       /* 4 KB sector erase */
    addr(a);
    desel();
    ms = wait_ready(1000);
    puts_("erase ms    ");
    put_u16(ms);
    puts_("\r\n");
    read_at(a, 16);
    show("after erase ", 16);

    simple(0x06);
    sel();
    xfer(0x02);                       /* page program */
    addr(a);
    for (i = 0; i < 16; i++)
        xfer((uint8_t)(0xA5 ^ (i * 17)));
    desel();
    ms = wait_ready(100);
    puts_("program ms  ");
    put_u16(ms);
    puts_("\r\n");
    read_at(a, 16);
    show("after write ", 16);
}

void main(void)
{
    static const uint8_t __code three[3] = { 0, 0, 0 };
    static const uint8_t __code sfdp[4] = { 0, 0, 0, 0 };   /* address 0 + one dummy byte */
    uint8_t cap;

    hal_init();
    hal_wdt_kick();
    puts_("\r\nOpenESL eepromprobe\r\n");

    simple(0xFF);                     /* leave continuous-read mode, if any */
    cmd_read(0xAB, three, 3, 2);      /* release from deep power-down; old-style device id */
    show("AB id       ", 2);
    hal_delay_ms(1);
    cmd_read(0x9F, 0, 0, 3);          /* JEDEC id: maker, type, capacity (2^n bytes) */
    show("9F jedec    ", 3);
    cap = buf[2];
    cmd_read(0x90, three, 3, 2);      /* maker / device id */
    show("90 mfr/dev  ", 2);
    cmd_read(0x05, 0, 0, 1);
    show("05 status1  ", 1);
    cmd_read(0x35, 0, 0, 1);
    show("35 status2  ", 1);
    cmd_read(0x5A, sfdp, 4, 16);      /* SFDP header: "SFDP" if supported */
    show("5A sfdp     ", 16);
    read_at(0, 16);
    show("03 @000000  ", 16);
    read_at(0x1000, 16);
    show("03 @001000  ", 16);

    if (cap >= 0x10 && cap <= 0x18)   /* 64 KB .. 16 MB */
        write_test((1UL << cap) - 0x1000);
    else
        puts_("capacity byte not plausible, no write test\r\n");

    simple(0xB9);                     /* deep power-down again, as the firmware should leave it */
    puts_("done\r\n");
    log_save();
    for (;;) {
        hal_wdt_kick();
        hal_delay_ms(1000);
    }
}
