/*
 * hwtest.c - bring-up program for the ZBS243 HAL (python tools/build.py hwtest).
 *
 * Prints the MAC and the EPD BUSY level, draws a test pattern on the ST-GM16XX2
 * panel (200x200 BW, stock SSD init from docs/st-gm16xx2.md), then prints a
 * counter once a second on the debug UART (pad 4, 115200 8N1). Compare the time
 * stamps on the PC with the counter to check hal_delay_ms.
 *
 * Radio: each round it sends an OpenEPaperLink check-in (PKT_AVAIL_DATA_REQ, PAN
 * 0x4447, channel 11) and prints every frame it receives in the next 100 ms.
 * An OEPL AP in range answers with PKT_AVAIL_DATA_INFO, which tests TX and RX.
 * Then battery, temperature, deep sleep, the watchdog, a boot counter kept in
 * the persistent store.
 */
#include "../hal.h"
#include "../hal_epd.h"
#include "zbs243.h"

static void put_hex(uint8_t v)
{
    static const char __code hex[] = "0123456789ABCDEF";
    hal_uart_putc(hex[v >> 4]);
    hal_uart_putc(hex[v & 0x0F]);
}

/* ---- Bit-banged SPI to the panel, mode 0, MSB first ---- */
static void spi_byte(uint8_t b)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        hal_epd_mosi(b & 0x80);
        hal_epd_sck(1);
        hal_epd_sck(0);
        b <<= 1;
    }
}

static void epd_cmd(uint8_t c)
{
    hal_epd_dc(0);
    hal_epd_cs(0);
    spi_byte(c);
    hal_epd_cs(1);
    hal_epd_dc(1);
}

static void epd_data(uint8_t d)
{
    hal_epd_cs(0);
    spi_byte(d);
    hal_epd_cs(1);
}

/* Waits while BUSY is high. Returns the time in units of 10 ms (255 = timeout). */
static uint8_t epd_wait(uint8_t max_10ms)
{
    uint8_t t = 0;
    while (hal_epd_busy()) {
        if (t == max_10ms)
            return 255;
        hal_delay_ms(10);
        t++;
    }
    return t;
}

static void put_wait(const char *what, uint8_t t)
{
    hal_uart_puts(what);
    put_hex(t);
    hal_uart_puts("\r\n");
}

#define EPD_W   200
#define EPD_H   200

/* Pixel (x, y) is black: a 4 px frame, and 25 px checker squares in the middle. */
static bool test_black(uint8_t x, uint8_t y)
{
    if (x < 4 || y < 4 || x >= EPD_W - 4 || y >= EPD_H - 4)
        return true;
    if (x < 50 || y < 50 || x >= 150 || y >= 150)
        return false;
    return ((x / 25) ^ (y / 25)) & 1;
}

static void epd_test(void)
{
    uint8_t x, y, b, bit, t;

    hal_epd_cs(1);
    hal_epd_sck(0);
    hal_epd_rst(0);
    hal_delay_ms(10);
    hal_epd_rst(1);
    hal_delay_ms(10);
    put_wait("EPD reset, busy 10ms: ", epd_wait(100));

    epd_cmd(0x12);                     /* soft reset */
    put_wait("EPD 0x12, busy 10ms: ", epd_wait(100));

    epd_cmd(0x01); epd_data((uint8_t)(EPD_H - 1)); epd_data(0x00); epd_data(0x00);
    epd_cmd(0x3A); epd_data(0x1B);
    epd_cmd(0x3B); epd_data(0x0B);
    epd_cmd(0x11); epd_data(0x01);     /* X+, Y- */
    epd_cmd(0x44); epd_data(0x00); epd_data(EPD_W / 8 - 1);
    epd_cmd(0x45); epd_data((uint8_t)(EPD_H - 1)); epd_data(0x00); epd_data(0x00); epd_data(0x00);
    epd_cmd(0x3C); epd_data(0x33);
    epd_cmd(0x0C); epd_data(0xC7); epd_data(0x8F); epd_data(0x8F);
    epd_cmd(0x21); epd_data(0x83);

    /* Black/white RAM: 1 = white. Rows from y = 0 at the top (Y counter starts at 199). */
    epd_cmd(0x4E); epd_data(0x00);
    epd_cmd(0x4F); epd_data((uint8_t)(EPD_H - 1)); epd_data(0x00);
    epd_cmd(0x24);
    for (y = 0; y < EPD_H; y++) {
        for (x = 0; x < EPD_W; x += 8) {
            b = 0xFF;
            for (bit = 0; bit < 8; bit++)
                if (test_black(x + bit, y))
                    b &= (uint8_t)~(0x80 >> bit);
            epd_data(b);
        }
    }
    epd_cmd(0x4E); epd_data(0x00);
    epd_cmd(0x4F); epd_data((uint8_t)(EPD_H - 1)); epd_data(0x00);
    epd_cmd(0x26);
    for (y = 0; y < EPD_H; y++)
        for (x = 0; x < EPD_W; x += 8)
            epd_data(0xFF);
    hal_uart_puts("EPD RAM written\r\n");

    epd_cmd(0x1A); epd_data(25); epd_data(0x00);   /* temperature, 25 C */
    epd_cmd(0x22); epd_data(0xD7);
    epd_cmd(0x20);
    t = epd_wait(250);
    if (t == 255)
        t = epd_wait(250);                         /* allow up to 5 s */
    put_wait("EPD refresh, busy 10ms: ", t);

    epd_cmd(0x21); epd_data(0x03);
    epd_cmd(0x10); epd_data(0x01);                 /* deep sleep */
}

#define OEPL_CHANNEL    11
#define OEPL_PAN        0x4447
#define OEPL_HW_TYPE    0x25    /* ST-GM16XX2 (our OEPL tag type) */

static uint8_t __xdata frame[64];
static uint8_t __xdata rx[127];

static void put_frame(const uint8_t *f, int8_t n)
{
    int8_t i;
    for (i = 0; i < n; i++) {
        put_hex(f[i]);
        if ((i & 7) == 7) {
            hal_uart_puts("\r\n");
            hal_delay_ms(5);    /* the D1 mini's software UART drops long bursts */
        }
    }
    hal_uart_puts("\r\n");
}

/* OEPL MacFrameBcast + PKT_AVAIL_DATA_REQ + struct AvailDataReq (21 bytes). */
static uint8_t build_checkin(const uint8_t *mac, uint8_t seq)
{
    uint8_t i, sum, n = 0;
    uint8_t *req;

    frame[n++] = 0x21;          /* data, ACK request */
    frame[n++] = 0xC8;          /* dst short, src extended */
    frame[n++] = seq;
    frame[n++] = (uint8_t)OEPL_PAN;
    frame[n++] = OEPL_PAN >> 8;
    frame[n++] = 0xFF;          /* broadcast */
    frame[n++] = 0xFF;
    frame[n++] = (uint8_t)OEPL_PAN;
    frame[n++] = OEPL_PAN >> 8;
    for (i = 0; i < 8; i++)
        frame[n++] = mac[i];
    frame[n++] = 0xE5;          /* PKT_AVAIL_DATA_REQ */
    req = &frame[n];
    for (i = 0; i < 21; i++)
        req[i] = 0;
    req[3] = 25;                /* temperature */
    req[4] = (uint8_t)3000;     /* battery mV */
    req[5] = 3000 >> 8;
    req[6] = OEPL_HW_TYPE;
    req[7] = 1;                 /* wake-up reason: boot */
    req[11] = OEPL_CHANNEL;
    sum = 0;
    for (i = 1; i < 21; i++)
        sum += req[i];
    req[0] = sum;
    frame[n + 21] = 0;          /* OEPL sends one extra byte after the struct */
    return n + 22;
}

/* Received frame: an AvailDataInfo reply is checked with OEPL's byte-sum checksum. */
static void show_rx(int8_t r)
{
    uint8_t j, sum = 0;

    hal_uart_puts("RX");
    hal_uart_puts(" n=");
    put_hex((uint8_t)r);
    if (r == 39 && rx[21] == 0xE6) {       /* MacFrameNormal (21) + type + 17 bytes */
        for (j = 23; j < 39; j++)
            sum += rx[j];
        hal_uart_puts(sum == rx[22] ? " AvailDataInfo checksum OK" : " AvailDataInfo checksum BAD");
    }
    hal_uart_puts("\r\n");
    hal_delay_ms(5);
    put_frame(rx, r);
}

/* Bring-up: dump the radio's xdata registers 0xDF00-0xDFFF (skipping the RX FIFO byte). */
static void radio_dump(void)
{
    uint16_t a;
    for (a = 0xDF00; a <= 0xDFFF; a++) {
        if ((a & 0x1F) == 0) {
            hal_uart_puts("\r\nR");
            put_hex((uint8_t)a);
            hal_uart_putc(' ');
            hal_delay_ms(3);
        }
        put_hex(a == 0xDF98 ? 0 : *(volatile __xdata uint8_t *)a);
    }
    hal_uart_puts("\r\n");
}

static void radio_test(const uint8_t *mac, uint8_t seq)
{
    int8_t r;
    uint8_t k;

    r = radio_tx(frame, build_checkin(mac, seq));
    hal_uart_puts(r ? "TX ok\r\n" : "TX fail\r\n");
    for (k = 0; k < 4; k++) {
        r = radio_rx(rx, sizeof(rx), 25);
        if (r >= 0)
            show_rx(r);
    }
}

void main(void)
{
    uint8_t mac[8];
    uint16_t n = 0;
    int8_t i;

    hal_init();
    hal_uart_puts("\r\nOpenESL hwtest\r\nMAC ");
    hal_mac_read(mac);
    for (i = 7; i >= 0; i--)
        put_hex(mac[i]);
    hal_uart_puts("\r\nBUSY ");
    hal_uart_putc(hal_epd_busy() ? '1' : '0');
    hal_uart_puts("\r\n");

    {
        /* Persistent store: a boot counter that must survive resets */
        uint8_t cnt = 0;
        if (hal_store_read(&cnt, 1) != 1)
            cnt = 0;
        cnt++;
        hal_uart_puts("boot count ");
        put_hex(cnt);
        hal_uart_puts(hal_store_write(&cnt, 1) ? " (saved)\r\n" : " (save FAILED)\r\n");
    }

    epd_test();

    radio_set_pan(OEPL_PAN);
    radio_init(OEPL_CHANNEL);
    hal_uart_puts("radio up\r\n");

    /* Each round: check in, print battery and temperature, then wait 10 s: busy
       on even rounds, deep sleep on odd ones. After 12 rounds, hang without kicking the watchdog: the tag must
       restart within ~60 s, and the boot count must go up by one. */
    for (;;) {
        hal_wdt_kick();
        radio_on();
        radio_test(mac, (uint8_t)n);
        radio_off();
        hal_uart_puts("battery mV ");
        {
            uint16_t mv = battery_read_mv();
            put_hex((uint8_t)(mv >> 8));
            put_hex((uint8_t)mv);
        }
        hal_uart_puts("\r\ntemp ");
        put_hex((uint8_t)hal_temp_c());
        hal_uart_puts(" C (hex), wait 10 s\r\n");
        hal_delay_ms(5);
        if (n & 1) {
            power_sleep_s(10);
        } else {
            uint8_t k;
            for (k = 0; k < 40; k++)
                hal_delay_ms(250);
        }
        hal_uart_puts("woke ");
        put_hex((uint8_t)n);
        hal_uart_puts("\r\n");
        if (++n == 12) {
            hal_uart_puts("watchdog test: hanging\r\n");
            for (;;)
                ;
        }
    }
}
