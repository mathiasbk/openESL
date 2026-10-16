/*
 * epd.c - the SSD16xx display over bit-banged SPI.
 *
 * UNCONFIRMED: the controller on the ST-GM29XX2 is not confirmed (phase 0). The
 * command codes below are the common ones in the SSD1680 family and must be checked
 * against the datasheet of the controller actually fitted, before they are entered
 * in docs/kildelogg.md. The same applies to polarity (EPD_BW_INVERT) and RAM
 * direction (EPD_DATA_ENTRY) on the 2.9".
 * No custom waveform: the update uses the display's OTP waveform.
 *
 * The window and line count come from model.h (DRAW_W lines of DRAW_H pixels). On a
 * black/white-only model (MODEL_HAS_RED = 0) only the black plane is written.
 * UNCONFIRMED for the 1.54" (ST-GM16XX2, 200 x 200): that it is an SSD16xx at all,
 * and whether its controller wants the second RAM (0x26) written too before a full update.
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
#define CMD_DUMMY_LINE      0x3A
#define CMD_GATE_TIME       0x3B
#define CMD_SOFT_START      0x0C
#define CMD_UPDATE_CTRL1    0x21
#define CMD_WRITE_TEMP      0x1A

#define EPD_BW_INVERT       1      /* 1 = the black plane stores 1 for white */
#define EPD_BUSY_TIMEOUT_MS 30000u /* a red update takes 15-20 s */
#if MODEL == 154
#define EPD_UPDATE_SEQ      0xD7   /* stock firmware, with the temperature from 0x1A */
#define EPD_TEMP_C          25     /* TODO phase 6: the ZBS243 temperature sensor */
#else
#define EPD_UPDATE_SEQ      0xF7   /* full update with the OTP waveform */
#endif
#if MODEL == 154
/*
 * On this panel a RAM line (Y) is a screen row, so one layout column cannot be
 * one RAM line: that gave a transposed image (rotated and mirrored, seen on the
 * tag 2026-10-14). Instead 8 layout columns are transposed into one RAM byte
 * column (X), and the counter runs down Y first.
 * With X+ and Y+ (0x07) the image came out upside down (rotated 180 degrees, seen
 * on the tag 2026-10-16), so both counters now run backwards from the far corner
 * of the window, and the bits in each byte are reversed (EPD_BIT_REVERSE): layout
 * pixel (x, y) lands in RAM at (DRAW_W-1-x, DRAW_H-1-y). draw_column is still
 * called with x increasing, so drawing costs the same.
 */
#define EPD_TRANSPOSE       1
#define EPD_DATA_ENTRY      0x04   /* X- and Y-, Y first */
#define EPD_BIT_REVERSE     1      /* layout column x+k is bit k (LSB first) */
#if DRAW_W % 8
#error "EPD_BIT_REVERSE needs DRAW_W to be a multiple of 8"
#endif
#else
#define EPD_TRANSPOSE       0
#define EPD_DATA_ENTRY      0x03   /* X+ and Y+, X first */
#define EPD_BIT_REVERSE     0
#endif

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

#if EPD_TRANSPOSE
#define RAM_X_BYTES     ((DRAW_W + 7) / 8)   /* RAM X = layout x, 8 pixels per byte */
#define RAM_Y_LINES     DRAW_H               /* RAM Y = layout y */
#else
#define RAM_X_BYTES     DRAW_COL_BYTES       /* RAM X = layout y, one column per line */
#define RAM_Y_LINES     DRAW_W               /* RAM Y = layout x */
#endif

/* Window start and end: a counter that runs backwards starts at the high end */
#define RAM_X_START     ((EPD_DATA_ENTRY & 0x01) ? 0 : RAM_X_BYTES - 1)
#define RAM_X_END       ((EPD_DATA_ENTRY & 0x01) ? RAM_X_BYTES - 1 : 0)
#define RAM_Y_START     ((EPD_DATA_ENTRY & 0x02) ? 0 : RAM_Y_LINES - 1)
#define RAM_Y_END       ((EPD_DATA_ENTRY & 0x02) ? RAM_Y_LINES - 1 : 0)

static void set_window(void)
{
    cmd(CMD_DATA_ENTRY);
    data(EPD_DATA_ENTRY);
    cmd(CMD_RAM_X_RANGE);              /* X in bytes */
    data(RAM_X_START);
    data(RAM_X_END);
    cmd(CMD_RAM_Y_RANGE);              /* Y in lines */
    data((uint8_t)RAM_Y_START);
    data((uint8_t)(RAM_Y_START >> 8));
    data((uint8_t)RAM_Y_END);
    data((uint8_t)(RAM_Y_END >> 8));
}

static void set_cursor(void)
{
    cmd(CMD_RAM_X_COUNTER);
    data(RAM_X_START);
    cmd(CMD_RAM_Y_COUNTER);
    data((uint8_t)RAM_Y_START);
    data((uint8_t)(RAM_Y_START >> 8));
}

#if EPD_TRANSPOSE
static uint8_t __xdata rows[DRAW_H];    /* 8 layout columns, one byte per row */

/* Streams one plane: red = 0 gives the black plane, 1 gives the red plane. */
static void stream_plane(draw_scene_t *s, uint8_t red)
{
    uint16_t x;
    uint8_t y, k, m, b;
    const uint8_t *col = red ? col_red : col_black;

    set_cursor();
    cmd(red ? CMD_WRITE_RED : CMD_WRITE_BW);
    hal_epd_dc(1);
    hal_epd_cs(0);
    draw_begin(s);
    for (x = 0; x < DRAW_W; x += 8) {
        for (y = 0; y < DRAW_H; y++)
            rows[y] = 0;
        for (k = 0; k < 8 && x + k < DRAW_W; k++) {
            draw_column(s, x + k, col_black, col_red);
            m = EPD_BIT_REVERSE ? (uint8_t)(0x01 << k) : (uint8_t)(0x80 >> k);
            for (y = 0; y < DRAW_H; y++)
                if (col[y >> 3] & (uint8_t)(0x80 >> (y & 7)))
                    rows[y] |= m;
        }
        for (y = 0; y < DRAW_H; y++) {
            b = rows[y];
            if (!red && EPD_BW_INVERT)
                b = (uint8_t)~b;
            spi_byte(b);
        }
        hal_wdt_kick();
    }
    hal_epd_cs(1);
}
#else
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
#endif

static void init_panel(void)
{
#if MODEL == 154
    cmd(CMD_DRIVER_OUTPUT);            /* gate lines */
    data((uint8_t)(DRAW_W - 1));
    data((uint8_t)((DRAW_W - 1) >> 8));
    data(0x00);
    cmd(CMD_DUMMY_LINE);
    data(0x1B);
    cmd(CMD_GATE_TIME);
    data(0x0B);
    set_window();
    cmd(CMD_BORDER);
    data(0x33);
    cmd(CMD_SOFT_START);
    data(0xC7);
    data(0x8F);
    data(0x8F);
    cmd(CMD_UPDATE_CTRL1);
    data(0x83);
#else
    cmd(CMD_DRIVER_OUTPUT);            /* 296 lines */
    data((uint8_t)(DRAW_W - 1));
    data((uint8_t)((DRAW_W - 1) >> 8));
    data(0x00);
    cmd(CMD_BORDER);
    data(0x05);
    cmd(CMD_TEMP_SENSOR);              /* internal temperature sensor */
    data(0x80);
    set_window();
#endif
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

    init_panel();

    stream_plane(s, 0);
#if MODEL_HAS_RED
    stream_plane(s, 1);
#endif

#if MODEL == 154
    cmd(CMD_WRITE_TEMP);
    data(EPD_TEMP_C);
    data(0x00);
#endif
    cmd(CMD_UPDATE_CTRL2);
    data(EPD_UPDATE_SEQ);
    cmd(CMD_MASTER_ACTIVATE);
    ok = wait_busy(EPD_BUSY_TIMEOUT_MS);

#if MODEL == 154
    cmd(CMD_UPDATE_CTRL1);             /* stock firmware does this before sleeping */
    data(0x03);
#endif
    cmd(CMD_DEEP_SLEEP);               /* must always be in deep sleep, or the battery drains */
    data(0x01);
    hal_epd_power(0);
    return ok;
}
