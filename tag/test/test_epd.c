/*
 * test_epd.c - runs the real epd.c against fake display pins with a small model of an
 * SSD16xx controller behind them: it decodes the bit-banged SPI, follows the data entry
 * mode, RAM window and address counters, and writes the bytes into two RAM planes like
 * the datasheet describes. The image is then rebuilt from controller RAM and compared
 * pixel for pixel with draw_column, so the whole path above the HAL is checked.
 * Both panels are black/white: if epd.c writes the second RAM (0x26) at all, it must
 * write all of it with zeros.
 *
 * What the model can not check: whether the real panel shows RAM in this orientation
 * (mirroring/rotation) and the polarity; both must be confirmed on a display (phase 2).
 *
 * With an argument, the image rebuilt from controller RAM is written there as PPM.
 */
#include <string.h>
#include "test.h"
#include "fake.h"
#include "epd.h"
#include "draw.h"
#include "layout.h"
#include "helpers.h"
#include "hal/hal_epd.h"

int test_failures;
int test_checks;

/* ---- SSD16xx model ---- */
#define RAM_X   32               /* bytes along a gate line (source direction) */
#define RAM_Y   320              /* gate lines */

static bool cs = true, dc, sck, mosi, powered;
static uint8_t shift, nbits;
static uint8_t cur_cmd = 0xFF, first_cmd = 0xFF, last_cmd = 0xFF;
static uint8_t param[8], nparam;
static uint8_t entry = 0x03, xs, xe = RAM_X - 1, xc;
static uint16_t ys, ye = RAM_Y - 1, yc, gate_lines;
static uint8_t ram[2][RAM_Y][RAM_X];
static int n_written[2], n_out_of_window, busy_polls;
static bool asleep, busy_stuck;

static void ram_write(uint8_t plane, uint8_t b)
{
    if (xc >= RAM_X || yc >= RAM_Y || xc < xs || xc > xe || yc < ys || yc > ye) {
        n_out_of_window++;
        return;
    }
    ram[plane][yc][xc] = b;
    n_written[plane]++;
    /* Advance: bit0 = X up, bit1 = Y up, bit2 = 0 means X changes first */
    if (!(entry & 0x04)) {
        if (entry & 0x01) {
            if (xc == xe) { xc = xs; yc = (entry & 0x02) ? yc + 1 : yc - 1; } else xc++;
        } else {
            if (xc == xs) { xc = xe; yc = (entry & 0x02) ? yc + 1 : yc - 1; } else xc--;
        }
    } else {
        if (entry & 0x02) {
            if (yc == ye) { yc = ys; xc = (entry & 0x01) ? xc + 1 : xc - 1; } else yc++;
        } else {
            if (yc == ys) { yc = ye; xc = (entry & 0x01) ? xc + 1 : xc - 1; } else yc--;
        }
    }
}

static void on_byte(uint8_t b)
{
    if (!dc) {
        if (first_cmd == 0xFF)
            first_cmd = b;
        cur_cmd = last_cmd = b;
        nparam = 0;
        if (b == 0x12) {                 /* software reset: defaults */
            entry = 0x03;
            asleep = false;
        }
        return;
    }
    if (cur_cmd == 0x24 || cur_cmd == 0x26) {
        ram_write(cur_cmd == 0x26, b);
        return;
    }
    if (nparam < sizeof(param))
        param[nparam++] = b;
    switch (cur_cmd) {
    case 0x01: if (nparam == 2) gate_lines = (uint16_t)(param[0] | (param[1] << 8)) + 1; break;
    case 0x10: if (nparam == 1) asleep = (param[0] & 0x03) != 0; break;
    case 0x11: if (nparam == 1) entry = param[0]; break;
    case 0x44: if (nparam == 2) { xs = param[0]; xe = param[1]; } break;
    case 0x45: if (nparam == 4) { ys = (uint16_t)(param[0] | (param[1] << 8));
                                  ye = (uint16_t)(param[2] | (param[3] << 8)); } break;
    case 0x4E: if (nparam == 1) xc = param[0]; break;
    case 0x4F: if (nparam == 2) yc = (uint16_t)(param[0] | (param[1] << 8)); break;
    default: break;
    }
}

void hal_epd_power(bool on) { powered = on; }
void hal_epd_cs(bool high) { cs = high; if (high) nbits = 0; }
void hal_epd_dc(bool high) { dc = high; }
void hal_epd_rst(bool high) { (void)high; }
void hal_epd_mosi(bool high) { mosi = high; }
void hal_epd_sck(bool high)
{
    if (high && !sck && !cs) {
        shift = (uint8_t)((shift << 1) | (mosi ? 1 : 0));
        if (++nbits == 8) {
            on_byte(shift);
            nbits = 0;
        }
    }
    sck = high;
}
bool hal_epd_busy(void) { return busy_stuck || ++busy_polls % 50 != 0; }   /* busy for a while */

static void reset_model(void)
{
    memset(ram, 0, sizeof(ram));
    memset(ram[1], 0xA5, sizeof(ram[1]));     /* the second RAM holds noise after power-up */
    n_written[0] = n_written[1] = n_out_of_window = 0;
    first_cmd = last_cmd = cur_cmd = 0xFF;
    gate_lines = 0;
}

/* Pixel (x, y) of the screen as the controller holds it: 0 white, 1 black.
   Screen column x = gate line x; row y = bit y of the source direction, MSB first. */
static int ram_pixel(int x, int y)
{
#if MODEL == 154
    /* 1.54": RAM line = screen row y, bit x of the source direction (epd.c EPD_TRANSPOSE),
       with RAM (0, 0) in the bottom right corner (EPD_ROTATE_180) */
    uint8_t m;
    x = DRAW_W - 1 - x;
    y = DRAW_H - 1 - y;
    m = (uint8_t)(0x80 >> (x & 7));
    return (ram[0][y][x >> 3] & m) ? 0 : 1;
#else
    uint8_t m = (uint8_t)(0x80 >> (y & 7));
    return (ram[0][x][y >> 3] & m) ? 0 : 1;      /* black plane: 1 = white */
#endif
}

/* Bytes of the screen window in the second RAM (0x26) that are not zero */
static int ram2_nonzero(void)
{
    int x, i, n = 0;
    for (x = 0; x < DRAW_W; x++)
        for (i = 0; i < DRAW_COL_BYTES; i++)
            n += ram[1][x][i] != 0;
    return n;
}

static void save_ppm(const char *path)
{
    static const unsigned char rgb[2][3] = { { 255, 255, 255 }, { 0, 0, 0 } };
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", DRAW_W, DRAW_H);
    for (y = 0; y < DRAW_H; y++)
        for (x = 0; x < DRAW_W; x++)
            fwrite(rgb[ram_pixel(x, y)], 1, 3, f);
    fclose(f);
}

static void test_epd_stream(const char *out)
{
    static const char *const f[] = { "Lettmelk 1 L", "24.90", "kr 24,90/l" };
    static draw_scene_t sc;
    uint8_t buf[ESL_MAX_PAYLOAD], b[DRAW_COL_BYTES], r[DRAW_COL_BYTES];
    proto_reply_t rep;
    int x, y, bad = 0, ink = 0;

    uint8_t len = make_content(buf, 0, 1, ESL_LAYOUT_PRICE, ESL_CF_PRICE_HIGHLIGHT | ESL_CF_FRAME, 3, f);
    proto_parse_reply(buf, len, &rep);
    layout_build(&sc, &rep, buf);

    reset_model();
    CHECK(epd_draw(&sc));
    CHECK_EQ(first_cmd, 0x12);                   /* software reset first */
    CHECK_EQ(gate_lines, DRAW_W);
    CHECK_EQ(xs, 0);
#if MODEL == 154
    CHECK_EQ(xe, (DRAW_W + 7) / 8 - 1);
    CHECK_EQ(ys, 0);
    CHECK_EQ(ye, DRAW_H - 1);
#else
    CHECK_EQ(xe, DRAW_COL_BYTES - 1);
    CHECK_EQ(ys, 0);
    CHECK_EQ(ye, DRAW_W - 1);
#endif
    CHECK_EQ(n_out_of_window, 0);
    CHECK_EQ(n_written[0], DRAW_W * DRAW_COL_BYTES);
    CHECK(n_written[1] == 0 || n_written[1] == DRAW_W * DRAW_COL_BYTES);
    CHECK_EQ(last_cmd, 0x10);                    /* deep sleep last */
    CHECK(asleep);
    CHECK(!powered);

    /* The image in controller RAM equals what draw_column computes */
    draw_begin(&sc);
    for (x = 0; x < DRAW_W; x++) {
        draw_column(&sc, (uint16_t)x, b, r);
        for (y = 0; y < DRAW_H; y++) {
            uint8_t m = (uint8_t)(0x80 >> (y & 7));
            int want = (b[y >> 3] & m) ? 1 : 0;
            if (ram_pixel(x, y) != want)
                bad++;
            if (want)
                ink++;
        }
    }
    CHECK_EQ(bad, 0);
    CHECK(ink > 500);
    CHECK(n_written[1] == 0 || ram2_nonzero() == 0);   /* never red */
    if (out)
        save_ppm(out);
}

static void test_epd_no_busy(void)
{
    /* A display that never answers: false, and the power is switched off again */
    static draw_scene_t sc;
    draw_clear(&sc);
    reset_model();
    busy_stuck = true;
    CHECK(!epd_draw(&sc));
    CHECK(!powered);
    CHECK_EQ(n_written[0], 0);
    busy_stuck = false;
}

int main(int argc, char **argv)
{
    printf("test_epd_stream\n");
    test_epd_stream(argc > 1 ? argv[1] : NULL);
    RUN(test_epd_no_busy);
    printf("\n%d checks, %d failures\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}
