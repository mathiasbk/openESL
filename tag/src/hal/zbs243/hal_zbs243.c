/*
 * hal_zbs243.c - the hardware layer for ZBS243 (SEM9110).
 *
 * Register facts are from Dmitry Grinberg's ZBS243 reverse engineering, as used in
 * atc1441's ZBS_Flasher examples and the OpenEPaperLink tag firmware (see zbs243.h
 * and docs/kildelogg.md). The code itself is written for this project.
 *
 * No interrupts are used: the delay polls timer 0, the UART polls its TX flag, and
 * the temperature sensor's "done" flag is polled.
 */
#include "../hal.h"
#include "../hal_epd.h"
#include "zbs243.h"

#define TICKS_PER_MS    1333u                  /* 16 MHz / 12 */
#define UART_BRG        138u                   /* 16 MHz / 139 = 115108 baud (115200) */

/* Output pins cannot be read back (zbs243.h), so pins we toggle are not read. */

/*
 * The watchdog counts up from its reset value at about 62 kHz (OEPL: 0xFF8E797F =
 * 120 s) and resets the chip on overflow. 60 s: 0x1000000 - 60 * 62000 = 0xC73CC0.
 */
#define WDT_RELOAD      0xC73CC0ul

static void wdt_off(void)
{
    CFGPAGE = 4;
    WDTENA = 0;
    WDTCONF &= (uint8_t)~0x80;
    CFGPAGE = 0;
}

static void wdt_on(void)
{
    CFGPAGE = 4;
    WDTPET = 0;
    WDTRSTVALH = (uint8_t)(WDT_RELOAD >> 16);
    WDTRSTVALM = (uint8_t)(WDT_RELOAD >> 8);
    WDTRSTVALL = (uint8_t)WDT_RELOAD;
    WDTCONF |= 0x80;
    WDTENA = 1;
    CFGPAGE = 0;
}

static void eeprom_sleep(void);

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
    wdt_off();
    gpio_init();
    uart_init();
    timer_init();
    eeprom_sleep();
    wdt_on();                /* requirement T10: a hang resets the tag after 60 s */
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

/*
 * Persistent stores, kept out of the firmware by CODE_SIZE in build.py:
 *   pages 0x3E (0xF800-0xFBFF) and 0x3D (0xF400-0xF7FF): the record (hal_store_*), in turn
 *     in one and then the other, so the last good copy stays while a new one is written
 *     (a write cut short by a battery change no longer loses the record, and with it the
 *     radio key): 0xA6, sequence number, length, data, sum of the data bytes. The newer
 *     good copy counts. Firmware before radio security kept one copy in page 0x3E as 0xA5,
 *     length, data, sum; it is read as older than any new one. (Page 0x3D was free; the
 *     probes in this folder wrote their logs there, which the checks turn away.)
 * Main flash reads directly as code memory.
 */
#define STORE_PAGE_A    0x3E
#define STORE_PAGE_B    0x3D
#define STORE_MAGIC_OLD 0xA5
#define STORE_MAGIC     0xA6
static __code const uint8_t __at (STORE_PAGE_A * 0x400u) store_a[4 + HAL_STORE_MAX];
static __code const uint8_t __at (STORE_PAGE_B * 0x400u) store_b[4 + HAL_STORE_MAX];
static __xdata uint8_t store_buf[4 + HAL_STORE_MAX];

/* Erases flash page `page` and writes len (1..1024) bytes from XRAM at src to its start.
   False if the controller did not finish. */
static bool flash_write_page(uint8_t page, uint16_t src, uint16_t len)
{
    uint8_t speed;
    uint32_t t = 0x40000;             /* erase + 1 KB write takes some tens of ms */

    speed = CLKSPEED;
    CLKSPEED = 0x21;                  /* flash operations only work at this speed */
    CFGPAGE = 4;
    SETTINGS |= 0x38;                 /* erase the page, then write */
    FWRTHREE = 3;
    FPGNO = page;
    FWRDSTL = 0;
    FWRDSTH = 0;
    FWRLENL = (uint8_t)(len - 1);
    FWRLENH = (uint8_t)((len - 1) >> 8);
    FWRSRCL = (uint8_t)src;
    FWRSRCH = (uint8_t)(src >> 8);
    TRIGGER |= 0x08;
    while (!(TCON2 & 0x08) && --t)
        ;
    TCON2 &= (uint8_t)~0x48;
    SETTINGS &= (uint8_t)~0x10;
    CFGPAGE = 0;
    CLKSPEED = speed;
    return t != 0;
}

/* The length of the good record at f (0 = none); *seq its sequence number, and 0x100 more
   for the old format so it never counts as newer */
static uint8_t store_check(const uint8_t __code *f, uint16_t *seq)
{
    uint8_t i, len, off, sum = 0;

    if (f[0] == STORE_MAGIC) {
        off = 3;
        *seq = f[1];
    } else if (f[0] == STORE_MAGIC_OLD && f == store_a) {
        off = 2;
        *seq = 0x100;
    } else {
        return 0;
    }
    len = f[off - 1];
    if (len == 0 || len > HAL_STORE_MAX)
        return 0;
    for (i = 0; i < len; i++)
        sum += f[off + i];
    return sum == f[off + len] ? len : 0;
}

/* The newer good copy (0 = none), its length and sequence number */
static const uint8_t __code *store_newest(uint8_t *len, uint16_t *seq)
{
    uint16_t sa, sb;
    uint8_t la = store_check(store_a, &sa), lb = store_check(store_b, &sb);

    /* sequence numbers wrap: b is newer when it is 1..127 ahead of a (an old-format a is
       older than any b) */
    if (lb && (!la || sa == 0x100 || (int8_t)((uint8_t)sb - (uint8_t)sa) > 0)) {
        *len = lb;
        *seq = sb;
        return store_b;
    }
    *len = la;
    *seq = sa;
    return la ? store_a : 0;
}

bool hal_store_write(const uint8_t *data, uint8_t len)
{
    uint8_t i, sum = 0, n;
    uint16_t seq, check;
    const uint8_t __code *cur = store_newest(&n, &seq);
    /* into the page that does not hold the newest copy */
    const uint8_t __code *to = cur == store_a ? store_b : store_a;

    if (len == 0 || len > HAL_STORE_MAX)
        return false;
    store_buf[0] = STORE_MAGIC;
    store_buf[1] = cur ? (uint8_t)(seq + 1) : 0;
    store_buf[2] = len;
    for (i = 0; i < len; i++) {
        store_buf[3 + i] = data[i];
        sum += data[i];
    }
    store_buf[3 + len] = sum;
    return flash_write_page(to == store_a ? STORE_PAGE_A : STORE_PAGE_B, (uint16_t)store_buf, (uint16_t)len + 4) &&
           store_check(to, &check) == len && store_newest(&n, &seq) == to;
}

uint8_t hal_store_read(uint8_t *data, uint8_t maxlen)
{
    uint8_t i, len;
    uint16_t seq;
    const uint8_t __code *f = store_newest(&len, &seq);

    if (!f || len > maxlen)
        return 0;                       /* also maxlen 0: just checks the record */
    f += f[0] == STORE_MAGIC ? 3 : 2;
    for (i = 0; i < len; i++)
        data[i] = f[i];
    return len;
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

/*
 * Battery voltage from the display controller's voltage detector, as OEPL does on
 * SSD panels: analog on (0x22 = 0xC0, 0x20), then for threshold n = 3..7 (0x15 n)
 * the status register 0x2F bit 0x10 says "below 1.9 V + n * 0.1 V". The result
 * is a 100 mV step. Reading uses the bidirectional data line (P0.1 as input).
 * The display must not be in use; it is left in deep sleep.
 *
 * On the ST-GM16XX2 the status register reads 0xFF (2026-10-14, tag on 3.3 V from
 * the flasher), so the panel does not answer reads; then this returns 0 = unknown.
 * OEPL on the same tag reports 2150 mV for the same reason.
 */
static void epd_byte(uint8_t b)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        P0_1 = (b & 0x80) != 0;
        P0_0 = 1;
        P0_0 = 0;
        b <<= 1;
    }
}

/*
 * The SPI flash (nCS P1.1, SCK and MOSI shared with the display) sits on the
 * battery: in standby it takes tens of µA, in deep power-down about 1 µA. Send
 * DEEP POWER-DOWN (0xB9, SPI mode 0) once at start-up; only a firmware update
 * wakes it (hal_xflash_open), and hal_xflash_close puts it back (the display
 * traffic keeps its nCS high, and the chip stays powered through the ZBS243's
 * sleep). Whole-byte writes to P1,
 * since reading a port back does not give the written levels (kildelogg.md);
 * the levels are those gpio_init() set, with P1.1 low in between.
 */
static void eeprom_sleep(void)
{
    P1 = 0x80;                       /* nCS low; EPD nCS stays high, BS1 low */
    epd_byte(0xB9);
    P1 = 0x82;                       /* nCS high */
}

static void epd_cmd1(uint8_t c, int16_t arg)     /* arg < 0: no parameter */
{
    P1_7 = 0;
    P2_2 = 0;
    epd_byte(c);
    P2_2 = 1;
    if (arg >= 0)
        epd_byte((uint8_t)arg);
    P1_7 = 1;
}

static uint8_t epd_read_reg(uint8_t c)
{
    uint8_t i, v = 0;

    P1_7 = 0;
    P2_2 = 0;
    epd_byte(c);
    P0DIR |= 0x02;                    /* data line in */
    P2_2 = 1;
    for (i = 0; i < 8; i++) {
        P0_0 = 1;
        v = (uint8_t)((v << 1) | P0_1);
        P0_0 = 0;
    }
    P1_7 = 1;
    P0DIR &= (uint8_t)~0x02;
    return v;
}

static bool epd_wait(uint8_t max_ms)
{
    while (P2_1) {
        if (!max_ms--)
            return false;
        hal_delay_ms(1);
    }
    return true;
}

static uint16_t measure_battery_mv(void)
{
    uint16_t mv = 2600;              /* above every threshold tried */
    uint8_t n, sta;

    P2_0 = 0;                        /* hardware reset */
    hal_delay_ms(1);
    P2_0 = 1;
    hal_delay_ms(1);
    epd_cmd1(0x12, -1);              /* soft reset */
    epd_wait(30);
    epd_cmd1(0x22, 0xC0);            /* clock and analog on */
    epd_cmd1(0x20, -1);
    epd_wait(150);
    for (n = 3; n < 8; n++) {
        epd_cmd1(0x15, n);
        epd_wait(150);
        sta = epd_read_reg(0x2F);
        if (sta == 0xFF) {
            mv = 0;                  /* nothing drives the data line: no reading */
            break;
        }
        if (sta & 0x10) {
            mv = 1850 + (uint16_t)n * 100;
            break;
        }
    }
    epd_cmd1(0x13, -1);
    epd_wait(15);
    epd_cmd1(0x10, 0x03);            /* deep sleep */
    return mv;
}

/* Measured on the first call and then every BATT_EVERY calls (it wakes the display):
   every 4 hours at the 60 s interval. A coin cell's voltage moves over weeks, and each
   measurement costs more than an hour of polls (batteri-analyse, 2026-10-18). */
#define BATT_EVERY      240
static uint16_t batt_mv;
static uint8_t batt_age;

uint16_t battery_read_mv(void)
{
    if (!batt_age-- ) {
        batt_age = (uint8_t)(BATT_EVERY - 1);
        batt_mv = measure_battery_mv();
    }
    return batt_mv;
}

/* Reverses the bit order of a byte (the sensor's result register is bit-reversed). */
static uint8_t bitswap(uint8_t v)
{
    uint8_t r = 0, i;
    for (i = 0; i < 8; i++) {
        r = (uint8_t)((r << 1) | (v & 1));
        v >>= 1;
    }
    return r;
}

static __xdata uint8_t temp_h, temp_l;

/* One conversion: start by setting IEN1 bit 0x10, wait for TEMPCFG 0x08. Polled. */
static bool temp_convert(void)
{
    uint8_t t = 50;

    IEN1 |= 0x10;
    CFGPAGE = 4;
    while (!(TEMPCFG & 0x08)) {
        CFGPAGE = 0;
        if (!t--) {
            IEN1 &= (uint8_t)~0x10;
            return false;
        }
        hal_delay_ms(1);
        CFGPAGE = 4;
    }
    temp_h = TEMPRETH;
    temp_l = TEMPRETL;
    TEMPCFG &= (uint8_t)~0x08;
    CFGPAGE = 0;
    IEN1 &= (uint8_t)~0x10;
    return true;
}

int8_t hal_temp_c(void)
{
    static __xdata uint8_t cal[4];
    uint16_t sum = 0, at30, at45, d;
    uint8_t i;

    CLKEN |= 0x80;
    CFGPAGE = 4;
    TEMPCFG = 0x81;
    TEMPCAL2 = 0x22;
    TEMPCAL1 = 0x55;
    TEMPCAL4 = 0;
    TEMPCAL3 = 0;
    TEMPCAL6 = 3;
    TEMPCAL5 = 0xFF;
    TEMPCFG &= (uint8_t)~0x08;
    CFGPAGE = 0;
    IEN1 &= (uint8_t)~0x10;

    for (i = 0; i < 9; i++) {
        if (!temp_convert())
            break;
        if (i) {                          /* the first result is thrown away */
            sum += (uint16_t)bitswap(temp_h) << 2;
            if (temp_l & 1)
                sum += 2;
            if (temp_l & 2)
                sum += 1;
        }
    }
    CLKEN &= (uint8_t)~0x80;
    if (i != 9)
        return 20;

    /* Factory calibration at info page 0x0B: raw value at 30 C, then at 45 C, MSB first */
    infopage_read(0x0B, cal, 4);
    at30 = ((uint16_t)cal[0] << 8) | cal[1];
    at45 = ((uint16_t)cal[2] << 8) | cal[3];
    if (at30 == 0xFFFF || at45 == 0xFFFF || at45 <= at30)
        return 20;                        /* as the stock firmware */
    sum = (sum + 4) / 8;
    d = at45 - at30;
    if (sum >= at30)
        return (int8_t)(30 + ((uint32_t)(sum - at30) * 15 + d / 2) / d);
    return (int8_t)(30 - ((uint32_t)(at30 - sum) * 15 + d / 2) / d);
}

/*
 * Deep sleep with the sleep timer in the radio block (1 s ticks). The CPU stops
 * and carries on after the last instruction here when the timer runs out. The
 * radio must be off. The watchdog is stopped while asleep.
 *
 * XRAM 0xE000-0xE2FF does not survive deep sleep (measured 2026-10-14); the
 * linker keeps all variables at 0xE300 and up (build.py). Internal RAM survives.
 */
void power_sleep_s(uint16_t s)
{
    if (!s)
        return;
    wdt_off();

    /* Pins to a low-power state: everything input with pull-up, except the
     * EPD lines, which keep their idle levels (CS high, RESET high). */
    P0FUNC = 0;
    P1FUNC = 0;
    P2FUNC = 0;
    P0DIR = 0xFF;
    P0PULL = 0xFF;
    P1 = 0x82;
    P1DIR = (uint8_t)~(0x02 | 0x04 | 0x80);
    P1PULL = 0x00;
    P2 = 0x01;
    P2DIR = (uint8_t)~(0x01 | 0x04);
    P2PULL = 0x00;

    RADIO_IRQ_PENDING = 0;
    RADIO_UNK_C1 &= (uint8_t)~0x81;
    TCON &= (uint8_t)~0x20;
    CFGPAGE = 4;
    RADIO_COMMAND = 0xCA;
    RADIO_COMMAND = 0xC5;
    RADIO_SLEEP_LO = (uint8_t)s;
    RADIO_SLEEP_MID = (uint8_t)(s >> 8);
    RADIO_SLEEP_HI = 0;
    __asm__("nop");
    RADIO_SLEEP_CFG = 0x16;             /* 1 s ticks */
    __asm__("nop\nnop\nnop\nnop");
    RADIO_SLEEP_CFG |= 0x80;            /* arm */
    __asm__("nop\nnop");
    RADIO_POWER_CTL = 0x44;             /* sleep */
    __asm__("nop\nnop");
    CFGPAGE = 0;

    /* Awake again: let timer 0 tick once, as OEPL does, then restore the pins. */
    TL0 = 0x00;
    TH0 = 0xFF;
    TCON |= 0x10;
    while (TH0 == 0xFF)
        ;
    TCON &= (uint8_t)~0x30;
    RADIO_UNK_C1 |= 0x81;

    gpio_init();
    uart_init();
    wdt_on();
}



/*
 * Sleep while the display refreshes (#261): the core used to wait awake on BUSY for
 * the whole 2.9 s. power_sleep_s keeps the display's CS and RESET high and puts its
 * clock and data pins to inputs with pull-up, which it ignores with CS high. Only
 * after the radio has been on once since reset: that is how every sleep in the main
 * loop starts, and the sleep timer is in the radio block. The start screen at boot
 * waits awake.
 */
extern bool radio_ever_on;

bool hal_doze_1s(void)
{
    if (!radio_ever_on)
        return false;
    power_sleep_s(1);
    return true;
}

/* ---- Display pins: CS P1.7, D/C P2.2, RESET P2.0, BUSY P2.1, SCK P0.0, MOSI P0.1 ---- */
void hal_epd_power(bool on) { (void)on; }   /* no switchable display supply known */
/* P1 as a whole byte (it does not read back): flash nCS P1.1 high, BS1 P1.2 low, as
   gpio_init() and xflash_zbs243.c leave it, so a bit write cannot disturb them while
   the image streams from the flash to the display. */
void hal_epd_cs(bool high)   { P1 = high ? 0x82 : 0x02; }
void hal_epd_dc(bool high)   { P2_2 = high; }
void hal_epd_rst(bool high)  { P2_0 = high; }
void hal_epd_sck(bool high)  { P0_0 = high; }
void hal_epd_mosi(bool high) { P0_1 = high; }
bool hal_epd_busy(void)      { return P2_1; }   /* SSD-type: high while busy */
