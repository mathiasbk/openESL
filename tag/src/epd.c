/*
 * epd.c - the SSD16xx display over bit-banged SPI.
 *
 * UNCONFIRMED: the controller on the ST-GM29XX2 is not confirmed (phase 0). The
 * command codes below are the common ones in the SSD1680 family and must be checked
 * against the datasheet of the controller actually fitted, before they are entered
 * in docs/kildelogg.md. The same applies to polarity (EPD_BW_INVERT) and RAM
 * direction (EPD_DATA_ENTRY).
 * No custom waveform: the update uses the display's OTP waveform.
 *
 * The window and line count come from model.h (DRAW_W lines of DRAW_H pixels), and how
 * the panel is driven (RAM direction, update sequence, init commands) from the model's
 * panel header in panels/, so a new panel is a header, not a change here. On a
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

/*
 * How the panel is driven comes from the model's panel header (tag/src/panels/, chosen
 * with "panel" in shared/models.json, included by model.h): EPD_TRANSPOSE,
 * EPD_ROTATE_180, EPD_DATA_ENTRY, EPD_GATE_SCAN, EPD_UPDATE_SEQ, EPD_WRITE_TEMP (with
 * EPD_TEMP_C), optionally EPD_SLEEP_CTRL1, and the EPD_INIT byte list.
 */
static const uint8_t __code panel_init[] = { EPD_INIT };

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

static void set_window(void)
{
    cmd(CMD_DATA_ENTRY);
    data(EPD_DATA_ENTRY);
    cmd(CMD_RAM_X_RANGE);              /* X in bytes */
    data(0);
    data(RAM_X_BYTES - 1);
    cmd(CMD_RAM_Y_RANGE);              /* Y in lines */
    data(0);
    data(0);
    data((uint8_t)(RAM_Y_LINES - 1));
    data((uint8_t)((RAM_Y_LINES - 1) >> 8));
}

static void set_cursor(void)
{
    cmd(CMD_RAM_X_COUNTER);
    data(0);
    cmd(CMD_RAM_Y_COUNTER);
    data(0);
    data(0);
}

#if EPD_TRANSPOSE
#if EPD_ROTATE_180 && (DRAW_W % 8)
#error "EPD_ROTATE_180 needs DRAW_W to be a multiple of 8"
#endif
static uint8_t __xdata rows[DRAW_H];    /* 8 layout columns, one byte per row */

/*
 * Streams one plane: red = 0 gives the black plane, 1 gives the red plane.
 * Layout columns are always computed left to right (draw_column is fastest that
 * way). With EPD_ROTATE_180 each group of 8 columns goes to RAM byte column
 * RAM_X_BYTES-1-g with the bits and the rows in reverse order, so the cursor is set
 * before each group.
 */
static void stream_plane(draw_scene_t *s, uint8_t red)
{
    uint16_t x;
    uint8_t y, k, m, b;
    const uint8_t *col = red ? col_red : col_black;

#if !EPD_ROTATE_180
    set_cursor();
    cmd(red ? CMD_WRITE_RED : CMD_WRITE_BW);
#endif
    draw_begin(s);
    for (x = 0; x < DRAW_W; x += 8) {
#if EPD_ROTATE_180
        cmd(CMD_RAM_X_COUNTER);
        data((uint8_t)(RAM_X_BYTES - 1 - (x >> 3)));
        cmd(CMD_RAM_Y_COUNTER);
        data(0);
        data(0);
        cmd(red ? CMD_WRITE_RED : CMD_WRITE_BW);
#endif
        for (y = 0; y < DRAW_H; y++)
            rows[y] = 0;
        for (k = 0; k < 8 && x + k < DRAW_W; k++) {
            draw_column(s, x + k, col_black, col_red);
#if EPD_ROTATE_180
            m = (uint8_t)(0x01 << k);
#else
            m = (uint8_t)(0x80 >> k);
#endif
            for (y = 0; y < DRAW_H; y++)
                if (col[y >> 3] & (uint8_t)(0x80 >> (y & 7)))
                    rows[y] |= m;
        }
        hal_epd_dc(1);
        hal_epd_cs(0);
        for (y = 0; y < DRAW_H; y++) {
#if EPD_ROTATE_180
            b = rows[DRAW_H - 1 - y];
#else
            b = rows[y];
#endif
            if (!red && EPD_BW_INVERT)
                b = (uint8_t)~b;
            spi_byte(b);
        }
        hal_epd_cs(1);
        hal_wdt_kick();
    }
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
    uint8_t i = 0, n;

    cmd(CMD_DRIVER_OUTPUT);            /* gate lines */
    data((uint8_t)(DRAW_W - 1));
    data((uint8_t)((DRAW_W - 1) >> 8));
    data(EPD_GATE_SCAN);
    while (i < sizeof(panel_init)) {
        if (panel_init[i] == EPD_OP_WINDOW) {
            set_window();
            i += 2;
            continue;
        }
        cmd(panel_init[i++]);
        for (n = panel_init[i++]; n; n--)
            data(panel_init[i++]);
    }
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

#if EPD_WRITE_TEMP
    cmd(CMD_WRITE_TEMP);
    data(EPD_TEMP_C);
    data(0x00);
#endif
    cmd(CMD_UPDATE_CTRL2);
    data(EPD_UPDATE_SEQ);
    cmd(CMD_MASTER_ACTIVATE);
    ok = wait_busy(EPD_BUSY_TIMEOUT_MS);

#ifdef EPD_SLEEP_CTRL1
    cmd(CMD_UPDATE_CTRL1);
    data(EPD_SLEEP_CTRL1);
#endif
    cmd(CMD_DEEP_SLEEP);               /* must always be in deep sleep, or the battery drains */
    data(0x01);
    hal_epd_power(0);
    return ok;
}
