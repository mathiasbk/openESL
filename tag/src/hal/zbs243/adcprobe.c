/*
 * adcprobe.c - experiment: sweep the temperature ADC's settings and keep the results
 * in flash page 0x3D (0xF400), to compare runs at different supply voltages.
 * Not part of the firmware. Record: 'A', 'D', count, then per setting
 * cfg, cal2, cal1, cal3, cal4, cal5, cal6, sum of 8 samples (16 bit, MSB first; 0xFFFF = timeout).
 * The last byte of the page counts the runs since flashing (0 = first run).
 */
#include "../hal.h"
#include "zbs243.h"

#define LOG_PAGE 0x3D
static __code const uint8_t __at (LOG_PAGE * 0x400u + 1023) prev_runs[1];
static __xdata uint8_t logbuf[1024];
static uint16_t loglen;
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

static uint8_t bitswap(uint8_t v)
{
    uint8_t r = 0, i;
    for (i = 0; i < 8; i++) {
        r = (uint8_t)((r << 1) | (v & 1));
        v >>= 1;
    }
    return r;
}

static __xdata uint8_t c[7];

static uint16_t adc_sum(void)
{
    uint8_t h, l, t, k;
    uint16_t sum = 0;

    CLKEN |= 0x80;
    CFGPAGE = 4;
    TEMPCFG = c[0];
    TEMPCAL2 = c[1];
    TEMPCAL1 = c[2];
    TEMPCAL3 = c[3];
    TEMPCAL4 = c[4];
    TEMPCAL5 = c[5];
    TEMPCAL6 = c[6];
    TEMPCFG &= (uint8_t)~0x08;
    CFGPAGE = 0;
    IEN1 &= (uint8_t)~0x10;
    for (k = 0; k < 9; k++) {
        t = 20;
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
        if (k) {
            sum += (uint16_t)bitswap(h) << 2;
            if (l & 1)
                sum += 2;
            if (l & 2)
                sum += 1;
        }
    }
    CLKEN &= (uint8_t)~0x80;
    return sum;
}

static void run(void)
{
    uint16_t r;
    uint8_t i;
    if (loglen + 9 > sizeof(logbuf))
        return;
    hal_wdt_kick();
    r = adc_sum();
    for (i = 0; i < 7; i++)
        logbuf[loglen++] = c[i];
    logbuf[loglen++] = (uint8_t)(r >> 8);
    logbuf[loglen++] = (uint8_t)r;
    logbuf[2]++;
}

static const uint8_t __code cfgs[] = {0x81, 0x82, 0x83, 0x80, 0x84, 0x88 | 0x01, 0x91, 0xA1, 0xC1};
static const uint8_t __code cal2s[] = {0x22, 0x02, 0x20, 0x26, 0x2A, 0x12, 0x32, 0x00};
static const uint8_t __code cal1s[] = {0x55, 0x54, 0x51, 0x45, 0x15, 0xAA, 0x00, 0xFF};

void main(void)
{
    uint8_t i, j;

    hal_init();
    logbuf[0] = 'A';
    logbuf[1] = 'D';
    logbuf[2] = 0;
    loglen = 3;
    c[3] = 0; c[4] = 0; c[5] = 0xFF; c[6] = 3;
    for (i = 0; i < sizeof(cfgs); i++) {
        c[0] = cfgs[i]; c[1] = 0x22; c[2] = 0x55;
        run();
    }
    c[0] = 0x81;
    for (i = 0; i < sizeof(cal2s); i++) {
        c[1] = cal2s[i];
        for (j = 0; j < sizeof(cal1s); j++) {
            c[2] = cal1s[j];
            run();
        }
    }
    c[1] = 0x22; c[2] = 0x55;
    for (i = 0; i < 8; i++) {
        c[3] = (uint8_t)(1 << i); run(); c[3] = 0;
        c[4] = (uint8_t)(1 << i); run(); c[4] = 0;
        c[6] = (uint8_t)(3 ^ (1 << i)); run(); c[6] = 3;
        c[5] = (uint8_t)(0xFF ^ (1 << i)); run(); c[5] = 0xFF;
    }
    c[0] = 0x81; c[1] = 0x22; c[2] = 0x55;
    run();                              /* the stock setting again, last */
    logbuf[1023] = (uint8_t)(prev_runs[0] + 1);   /* run counter: 0xFF + 1 = 0 on the first run */
    log_save();
    for (;;) {
        hal_wdt_kick();
        hal_delay_ms(1000);
    }
}
