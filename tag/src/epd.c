/*
 * epd.c - the SSD16xx display over bit-banged SPI, with a fallback for boards that carry a
 * UC-type controller (UC8151 family) instead (EPD_UC_INIT in the panel header, see below).
 *
 * UNCONFIRMED: the controller on the ST-GM29XX2 is not confirmed (phase 0). The
 * command codes below are the common ones in the SSD1680 family and must be checked
 * against the datasheet of the controller actually fitted, before they are entered
 * in docs/sources.md. The same applies to polarity (EPD_BW_INVERT) and RAM
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
#include "image.h"
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

/* UC-type controller (UC8151 family), used when the panel header defines EPD_UC_INIT */
#define UC_DTM1             0x10   /* old image */
#define UC_DRF              0x12   /* display refresh */
#define UC_DTM2             0x13   /* new image */
#define UC_POF              0x02   /* power off */
#define UC_DSLP             0x07   /* deep sleep, with the check code 0xA5 */
#define UC_VDCS             0x82   /* VCOM DC setting */

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

/* Column x into col_black/col_red: from the scene, or from the image in the SPI flash
   when s is 0. A flash read deselects the display (P1 is written whole) and leaves
   its own chip deselected, so the display is selected again before its bytes go out. */
static void column(draw_scene_t *s, uint16_t x)
{
    if (s)
        draw_column(s, x, col_black, col_red);
    else
        image_column(x, col_black, col_red);
}

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

/*
 * The refresh after master activation (0x20). BUSY goes high a moment after the command,
 * not at once, so it is first awaited (a refresh that never raises it did not run). The
 * end then counts only once BUSY has stayed low for REFRESH_LOW_MS in a row: a single low
 * reading while the booster switches is not the end. Taking either for the end sent deep
 * sleep into a running refresh, which stops the panel partway: dark with faint text, or
 * half drawn with the old picture showing through (1.54").
 * The refresh itself (2.9 s, up to 20 s with red) is waited out awake: 0.9.0 slept a second
 * at a time here (#261), which stopped the panel partway in the same way.
 */
#define REFRESH_START_MS    100u
#define REFRESH_LOW_MS      10u

static bool wait_refresh(void)
{
    uint16_t ms = EPD_BUSY_TIMEOUT_MS;
    uint8_t t, low = 0;

    for (t = 0; t < REFRESH_START_MS && !hal_epd_busy(); t++)
        hal_delay_ms(1);
    if (t == REFRESH_START_MS)
        return false;
    while (low < REFRESH_LOW_MS) {
        if (hal_epd_busy())
            low = 0;
        else
            low++;
        if (!ms--)
            return false;
        hal_delay_ms(1);
        if ((ms & 0x3FF) == 0)
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
    if (s)
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
            column(s, x + k);
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
/* Streams one plane after the RAM write command c: red = 0 gives the black plane, 1 gives
   the red plane. */
static void stream_plane_cmd(draw_scene_t *s, uint8_t red, uint8_t c)
{
    uint16_t x;
    uint8_t i, b;

    cmd(c);
    hal_epd_dc(1);
    if (s)
        draw_begin(s);
    for (x = 0; x < DRAW_W; x++) {
        column(s, x);
        hal_epd_cs(0);
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

static void stream_plane(draw_scene_t *s, uint8_t red)
{
    set_cursor();
    stream_plane_cmd(s, red, red ? CMD_WRITE_RED : CMD_WRITE_BW);
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

/*
 * Deep sleep for whichever controller is fitted: hal_epd_power() cannot switch the display
 * off on the ZBS243, so every exit ends here. 0x10 0x01 is SSD-type deep sleep (on a UC-type
 * controller: one byte into the old-image RAM, harmless); 0x07 0xA5 is UC-type deep sleep
 * (an SSD-type controller is asleep by then and ignores it).
 */
static void sleep_any(void)
{
    cmd(CMD_DEEP_SLEEP);
    data(0x01);
    cmd(UC_DSLP);
    data(0xA5);
    hal_epd_power(0);
}

static void hw_reset(void)
{
    hal_epd_power(1);
    hal_epd_cs(1);
    hal_epd_sck(0);
    hal_epd_rst(0);
    hal_delay_ms(10);
    hal_epd_rst(1);
    hal_delay_ms(10);
}

#ifdef EPD_UC_INIT
/*
 * Some boards of the same tag carry a UC-type controller (UC8151 family) instead of the
 * SSD-type one (docs/st-gm16xx2.md). The two are told apart by BUSY, which is active high
 * on SSD-type and active low on UC-type: once out of reset an SSD-type controller holds it
 * low, a UC-type one high. The init list is in the panel header (EPD_UC_INIT).
 */
#if MODEL_HAS_RED
#error "the UC-type path streams one black/white plane"
#endif
static const uint8_t __code uc_init[] = { EPD_UC_INIT };
static bool uc_panel;          /* the display has answered as UC-type */

/* UC-type: BUSY is low while busy */
static bool uc_wait_ready(uint16_t timeout_ms)
{
    while (!hal_epd_busy()) {
        if (!timeout_ms--)
            return false;
        hal_delay_ms(1);
        if ((timeout_ms & 0x3FF) == 0)
            hal_wdt_kick();
    }
    return true;
}

#if EPD_TRANSPOSE
/*
 * The UC-type controller on the 1.54" (seen on a tag made in 2017) is written one gate line (screen row) at a time, two bits per pixel
 * (11 = white, 00 = black), all into DTM1; DTM2 is not used. It has no data entry mode, and
 * on this transposed panel a gate line is a layout row, so the rows are built in strips of
 * UC_STRIP_ROWS: every layout column is drawn once per strip and only its rows inside the
 * strip are kept, then the strip is sent. Gate line 0 here is RAM line DRAW_H-1 on the
 * SSD-type path (which counts Y down), byte for byte, so with the 180 degree turn of the
 * SSD-type path a row runs from layout column DRAW_W-1 down to 0, MSB first.
 */
#if !EPD_ROTATE_180
#error "the transposed UC-type path assumes the SSD-type path's EPD_ROTATE_180"
#endif
#define UC_STRIP_ROWS   100u
#if DRAW_H % UC_STRIP_ROWS
#error "DRAW_H must be a multiple of UC_STRIP_ROWS"
#endif
static uint8_t __xdata uc_strip[UC_STRIP_ROWS * RAM_X_BYTES];

/* Four pixels (bits 3..0, 1 = white) as two bits each, the first in bits 7..6 */
static uint8_t uc_2bpp(uint8_t n)
{
    uint8_t r = 0;
    if (n & 0x08) r |= 0xC0;
    if (n & 0x04) r |= 0x30;
    if (n & 0x02) r |= 0x0C;
    if (n & 0x01) r |= 0x03;
    return r;
}

static void uc_stream(draw_scene_t *s)
{
    uint16_t x;
    uint8_t y0, y, p, m, b;
    uint8_t __xdata *row;

    cmd(UC_DTM1);
    for (y0 = 0; y0 < DRAW_H; y0 += UC_STRIP_ROWS) {
        for (row = uc_strip; row != uc_strip + sizeof(uc_strip); row++)
            *row = 0;
        if (s)
            draw_begin(s);
        for (x = 0; x < DRAW_W; x++) {
            column(s, x);
            p = (uint8_t)(DRAW_W - 1 - x);
            m = (uint8_t)(0x80 >> (p & 7));
            row = uc_strip + (p >> 3);
            for (y = y0; y < y0 + UC_STRIP_ROWS; y++, row += RAM_X_BYTES)
                if (col_black[y >> 3] & (uint8_t)(0x80 >> (y & 7)))
                    *row |= m;
            if ((x & 31) == 0)
                hal_wdt_kick();
        }
        hal_epd_dc(1);
        hal_epd_cs(0);
        for (row = uc_strip; row != uc_strip + sizeof(uc_strip); row++) {
            b = (uint8_t)~*row;            /* 1 = white */
            spi_byte(uc_2bpp((uint8_t)(b >> 4)));
            spi_byte(uc_2bpp((uint8_t)(b & 0x0F)));
        }
        hal_epd_cs(1);
    }
}
#else
/* One layout column per gate line: the image goes into both RAMs as it is */
static void uc_stream(draw_scene_t *s)
{
    stream_plane_cmd(s, 0, UC_DTM1);
    stream_plane_cmd(s, 0, UC_DTM2);
}
#endif

/* Power on can take seconds on the 1.54" made in 2017 */
#define UC_PON_MS       10000u

static bool update_uc(draw_scene_t *s)
{
    bool ok = false;
    uint8_t i = 0, n;
    uint8_t t;

    /* Out of reset BUSY goes high (idle) on most boards. On the 1.54" made in 2017 it stays
       low until power on, so after a few reset pulses the init goes ahead anyway; power on
       is where the controller has to answer. */
    for (t = 0; t < 3; t++) {
        hw_reset();
        if (uc_wait_ready(100))
            break;
    }
    while (i < sizeof(uc_init)) {
        cmd(uc_init[i++]);
        n = uc_init[i++];
        if (!n && !uc_wait_ready(UC_PON_MS)) {   /* power on */
            sleep_any();
            return false;
        }
        for (; n; n--)
            data(uc_init[i++]);
    }
    uc_stream(s);
    cmd(UC_DRF);
    /* The refresh pulls BUSY low: a line that stays high is no display at all */
    for (t = 0; t < 200 && hal_epd_busy(); t++)
        hal_delay_ms(1);
    if (t < 200)
        ok = uc_wait_ready(EPD_BUSY_TIMEOUT_MS);

    cmd(UC_VDCS);
    data(0x00);
    cmd(UC_POF);
    uc_wait_ready(1000);
    sleep_any();                       /* must always be in deep sleep, or the battery drains */
    return ok;
}

/*
 * After hw_reset(): an SSD-type controller holds BUSY low (idle) once its reset is done. A
 * UC-type one holds it high when idle, but on some boards (a 1.54" made in 2017) it is low
 * at first and goes high later. BUSY high for UC_HIGH_MS in a row within UC_DETECT_MS
 * means UC-type (or no display); the full wait is paid only until an SSD-type draw has
 * worked once.
 */
#define UC_DETECT_MS    300u
#define UC_HIGH_MS      50u
static bool ssd_panel;         /* an SSD-type draw has worked */

static bool busy_says_uc(void)
{
    uint16_t t;
    uint8_t high = 0;
    for (t = 0; t < UC_DETECT_MS; t++) {
        if (hal_epd_busy()) {
            if (++high >= UC_HIGH_MS)
                return true;
        } else {
            high = 0;
            if (ssd_panel)
                return false;
        }
        hal_delay_ms(1);
    }
    return false;
}
#endif

static bool update_ssd(draw_scene_t *s);

/* s = 0: the image in the SPI flash */
static bool update(draw_scene_t *s)
{
    bool ok;

#ifdef EPD_UC_INIT
    if (uc_panel)
        return update_uc(s);
#endif
    hw_reset();
#ifdef EPD_UC_INIT
    if (busy_says_uc()) {
        uc_panel = update_uc(s);
        return uc_panel;
    }
#endif
    ok = update_ssd(s);
#ifdef EPD_UC_INIT
    /* An SSD-type draw that fails before it has ever worked: try the UC-type path once */
    if (ok)
        ssd_panel = true;
    else if (!ssd_panel)
        ok = uc_panel = update_uc(s);
#endif
    return ok;
}

/* The SSD-type path, after hw_reset() */
static bool update_ssd(draw_scene_t *s)
{
    bool ok;

    cmd(CMD_SW_RESET);
    if (!wait_busy(1000)) {
        sleep_any();
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
    ok = wait_refresh();

#ifdef EPD_SLEEP_CTRL1
    cmd(CMD_UPDATE_CTRL1);
    data(EPD_SLEEP_CTRL1);
#endif
    sleep_any();                       /* must always be in deep sleep, or the battery drains */
    return ok;
}

bool epd_draw(draw_scene_t *s)
{
    return update(s);
}

bool epd_draw_image(void)
{
    return update((draw_scene_t *)0);
}
