/*
 * battprobe.c - experiment: can the ST-GM16XX2 be read, and on which pin?
 * Not part of the firmware. Prints raw values on the debug UART.
 */
#include "../hal.h"
#include "zbs243.h"

/* No debug UART on the jig: the output also goes to a log that is written to
 * flash page 0x3D (0xF400) at the end, to be read back with the flasher. */
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
#define hal_uart_puts puts_

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

static void byte_out(uint8_t b)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        P0_1 = (b & 0x80) != 0;
        P0_0 = 1;
        P0_0 = 0;
        b <<= 1;
    }
}

static void cmd(uint8_t c, int16_t arg)
{
    P1_7 = 0;
    P2_2 = 0;
    byte_out(c);
    P2_2 = 1;
    if (arg >= 0)
        byte_out((uint8_t)arg);
    P1_7 = 1;
}

static __xdata uint8_t r1[4], r2[4];

/* Command c, then n bytes clocked in, sampling P0.1 and P0.2 on each rising edge. */
static void read_both(uint8_t c, uint8_t n, uint8_t slow)
{
    uint8_t i, j, a, b;

    P1_7 = 0;
    P2_2 = 0;
    byte_out(c);
    P0DIR |= 0x02;
    P2_2 = 1;
    for (j = 0; j < n; j++) {
        a = b = 0;
        for (i = 0; i < 8; i++) {
            P0_0 = 1;
            if (slow)
                hal_delay_ms(1);
            a = (uint8_t)((a << 1) | P0_1);
            b = (uint8_t)((b << 1) | P0_2);
            P0_0 = 0;
            if (slow)
                hal_delay_ms(1);
        }
        r1[j] = a;
        r2[j] = b;
    }
    P1_7 = 1;
    P0DIR &= (uint8_t)~0x02;
}

static void show(const char *what, uint8_t c, uint8_t n, uint8_t slow)
{
    uint8_t j;
    read_both(c, n, slow);
    hal_uart_puts(what);
    hal_uart_puts(" P01=");
    for (j = 0; j < n; j++)
        put_hex(r1[j]);
    hal_uart_puts(" P02=");
    for (j = 0; j < n; j++)
        put_hex(r2[j]);
    hal_uart_puts("\r\n");
}

static void wait_busy(void)
{
    uint8_t t = 200;
    while (P2_1 && t--)
        hal_delay_ms(1);
}

static void epd_probe(uint8_t pull)
{
    uint8_t n;

    P0PULL = pull;
    hal_uart_puts(pull ? "-- pull-up on P0.1/P0.2\r\n" : "-- no pull-ups\r\n");
    P2_0 = 0;
    hal_delay_ms(1);
    P2_0 = 1;
    hal_delay_ms(1);
    cmd(0x12, -1);
    wait_busy();
    show("2F after reset", 0x2F, 1, 0);
    show("2F slow       ", 0x2F, 1, 1);
    show("1B temp reg   ", 0x1B, 2, 0);
    show("2D otp opt    ", 0x2D, 4, 0);
    show("2E user id    ", 0x2E, 4, 0);
    show("71 status(UC) ", 0x71, 1, 0);
    cmd(0x22, 0xC0);
    cmd(0x20, -1);
    wait_busy();
    for (n = 3; n < 8; n++) {
        cmd(0x15, n);
        wait_busy();
        hal_uart_puts("15 ");
        put_hex(n);
        show(" 2F", 0x2F, 1, 0);
    }
    cmd(0x10, 0x03);
    P0PULL = 0;
}

/* The temperature ADC with a different TEMPCFG / TEMPCAL2 / TEMPCAL1: raw 10-bit result. */
static uint16_t adc_raw(uint8_t cfg, uint8_t cal2, uint8_t cal1)
{
    uint8_t h, l, t, k;
    uint16_t r = 0;

    CLKEN |= 0x80;
    CFGPAGE = 4;
    TEMPCFG = cfg;
    TEMPCAL2 = cal2;
    TEMPCAL1 = cal1;
    TEMPCAL4 = 0;
    TEMPCAL3 = 0;
    TEMPCAL6 = 3;
    TEMPCAL5 = 0xFF;
    TEMPCFG &= (uint8_t)~0x08;
    CFGPAGE = 0;
    IEN1 &= (uint8_t)~0x10;
    for (k = 0; k < 3; k++) {
        t = 50;
        IEN1 |= 0x10;
        CFGPAGE = 4;
        while (!(TEMPCFG & 0x08)) {
            CFGPAGE = 0;
            if (!t--) {
                IEN1 &= (uint8_t)~0x10;
                CLKEN &= (uint8_t)~0x80;
                return 0xFFFF;
            }
            hal_delay_ms(1);
            CFGPAGE = 4;
        }
        h = TEMPRETH;
        l = TEMPRETL;
        TEMPCFG &= (uint8_t)~0x08;
        CFGPAGE = 0;
        IEN1 &= (uint8_t)~0x10;
        r = ((uint16_t)h << 8) | l;
    }
    CLKEN &= (uint8_t)~0x80;
    return r;
}

static void adc_show(uint8_t cfg, uint8_t cal2, uint8_t cal1)
{
    uint16_t r = adc_raw(cfg, cal2, cal1);
    hal_uart_puts("adc cfg ");
    put_hex(cfg);
    hal_uart_puts(" cal2 ");
    put_hex(cal2);
    hal_uart_puts(" cal1 ");
    put_hex(cal1);
    hal_uart_puts(" raw HL ");
    put_hex((uint8_t)(r >> 8));
    put_hex((uint8_t)r);
    hal_uart_puts("\r\n");
}

void main(void)
{
    uint8_t i;

    hal_init();
    hal_wdt_kick();
    hal_uart_puts("\r\nOpenESL battprobe\r\n");
    epd_probe(0);
    epd_probe(0x06);
    for (i = 0; i < 8; i++)
        adc_show((uint8_t)(0x80 | i), 0x22, 0x55);
    adc_show(0x91, 0x22, 0x55);
    adc_show(0xA1, 0x22, 0x55);
    adc_show(0xC1, 0x22, 0x55);
    adc_show(0x01, 0x22, 0x55);
    for (i = 0; i < 8; i++)
        adc_show(0x81, (uint8_t)(0x22 | (i << 6) | (i & 1 ? 0x01 : 0)), 0x55);
    hal_uart_puts("done\r\n");
    log_save();
    for (;;) {
        hal_wdt_kick();
        hal_delay_ms(1000);
    }
}
