/*
 * epd.c - the SSD16xx display over bit-banged SPI.
 *
 * UNCONFIRMED: the controller on the ST-GM29XX2 is not confirmed (phase 0). The
 * command codes below are the common ones in the SSD1680 family and must be checked
 * against the datasheet of the controller actually fitted, before they are entered
 * in docs/kildelogg.md. The same applies to polarity (EPD_BW_INVERT) and RAM
 * direction (EPD_DATA_ENTRY).
 * No custom waveform: the update uses the display's OTP waveform.
 */
#include "epd.h"
#include "hal/hal.h"
#include "hal/hal_epd.h"

/* Commands (SSD1680 family, unconfirmed) */
#define CMD_DRIVER_OUTPUT   0x01
#define CMD_DEEP_SLEEP      0x10
#define CMD_DATA_ENTRY      0x11
#define CMD_SW_RESET        0x12
#define CMD_TEMP_SENSOR     0x18
#define CMD_MASTER_ACTIVATE 0x20
#define CMD_UPDATE_CTRL2    0x22
#define CMD_WRITE_BW        0x24
#define CMD_WRITE_RED       0x26
#define CMD_BORDER          0x3C
#define CMD_RAM_X_RANGE     0x44
#define CMD_RAM_Y_RANGE     0x45
#define CMD_RAM_X_COUNTER   0x4E
#define CMD_RAM_Y_COUNTER   0x4F

#define EPD_BW_INVERT       1      /* 1 = the black plane stores 1 for white */
#define EPD_DATA_ENTRY      0x03   /* X+ and Y+, X first */
#define EPD_UPDATE_SEQ      0xF7   /* full update with the OTP waveform */
#define EPD_BUSY_TIMEOUT_MS 30000u /* a red update takes 15-20 s */

static uint8_t __xdata col_black[DRAW_COL_BYTES];
static uint8_t __xdata col_red[DRAW_COL_BYTES];

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

static void cmd(uint8_t c)
{
    hal_epd_dc(0);
    hal_epd_cs(0);
    spi_byte(c);
    hal_epd_cs(1);
}

static void data(uint8_t d)
{
    hal_epd_dc(1);
    hal_epd_cs(0);
    spi_byte(d);
    hal_epd_cs(1);
}

static bool wait_busy(uint16_t timeout_ms)
{
    while (hal_epd_busy()) {
        if (!timeout_ms--)
            return false;
        hal_delay_ms(1);
        if ((timeout_ms & 0x3FF) == 0)
            hal_wdt_kick();
    }
    return true;
}

static void set_window(void)
{
    cmd(CMD_DATA_ENTRY);
    data(EPD_DATA_ENTRY);
    cmd(CMD_RAM_X_RANGE);              /* X in bytes: 0 .. 15 (128 pixels) */
    data(0);
    data(DRAW_COL_BYTES - 1);
    cmd(CMD_RAM_Y_RANGE);              /* Y: 0 .. 295 */
    data(0);
    data(0);
    data((uint8_t)(DRAW_W - 1));
    data((uint8_t)((DRAW_W - 1) >> 8));
}

static void set_cursor(void)
{
    cmd(CMD_RAM_X_COUNTER);
    data(0);
    cmd(CMD_RAM_Y_COUNTER);
    data(0);
    data(0);
}

/* Streams one plane: red = 0 gives the black plane, 1 gives the red plane. */
static void stream_plane(draw_scene_t *s, uint8_t red)
{
    uint16_t x;
    uint8_t i, b;

    set_cursor();
    cmd(red ? CMD_WRITE_RED : CMD_WRITE_BW);
    hal_epd_dc(1);
    hal_epd_cs(0);
    draw_begin(s);
    for (x = 0; x < DRAW_W; x++) {
        draw_column(s, x, col_black, col_red);
        for (i = 0; i < DRAW_COL_BYTES; i++) {
            b = red ? col_red[i] : col_black[i];
            if (!red && EPD_BW_INVERT)
                b = (uint8_t)~b;
            spi_byte(b);
        }
        if ((x & 31) == 0)
            hal_wdt_kick();
    }
    hal_epd_cs(1);
}

bool epd_draw(draw_scene_t *s)
{
    bool ok;

    hal_epd_power(1);
    hal_epd_cs(1);
    hal_epd_sck(0);
    hal_epd_rst(0);
    hal_delay_ms(10);
    hal_epd_rst(1);
    hal_delay_ms(10);

    cmd(CMD_SW_RESET);
    if (!wait_busy(1000)) {
        hal_epd_power(0);
        return false;
    }

    cmd(CMD_DRIVER_OUTPUT);            /* 296 lines */
    data((uint8_t)(DRAW_W - 1));
    data((uint8_t)((DRAW_W - 1) >> 8));
    data(0x00);
    cmd(CMD_BORDER);
    data(0x05);
    cmd(CMD_TEMP_SENSOR);              /* internal temperature sensor */
    data(0x80);
    set_window();

    stream_plane(s, 0);
    stream_plane(s, 1);

    cmd(CMD_UPDATE_CTRL2);
    data(EPD_UPDATE_SEQ);
    cmd(CMD_MASTER_ACTIVATE);
    ok = wait_busy(EPD_BUSY_TIMEOUT_MS);

    cmd(CMD_DEEP_SLEEP);               /* must always be in deep sleep, or the battery drains */
    data(0x01);
    hal_epd_power(0);
    return ok;
}
