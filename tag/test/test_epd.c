/*
 * test_epd.c - runs the real epd.c against fake display pins and decodes the SPI bits.
 * Checks that both colour planes are sent completely (296 x 16 bytes), that the black plane is
 * inverted and matches draw_column, and that the display ends up in deep sleep.
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

/* ---- SPI-dekoder ---- */
static bool cs = true, dc, sck, mosi, powered;
static uint8_t shift, nbits;
static uint8_t last_cmd = 0xFF, first_cmd = 0xFF;
static uint8_t plane_bw[DRAW_W * DRAW_COL_BYTES], plane_red[DRAW_W * DRAW_COL_BYTES];
static int n_bw, n_red, n_cmds;
static int busy_polls;

static void on_byte(uint8_t b)
{
    if (!dc) {
        if (first_cmd == 0xFF)
            first_cmd = b;
        last_cmd = b;
        n_cmds++;
        return;
    }
    if (last_cmd == 0x24 && n_bw < (int)sizeof(plane_bw))
        plane_bw[n_bw++] = b;
    else if (last_cmd == 0x26 && n_red < (int)sizeof(plane_red))
        plane_red[n_red++] = b;
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
bool hal_epd_busy(void) { return ++busy_polls % 50 != 0; }   /* opptatt en stund */

static void test_epd_stream(void)
{
    static const char *const f[] = { "Lettmelk 1 L", "24,90", "kr/l 24,90" };
    static draw_scene_t sc;
    uint8_t buf[ESL_MAX_PAYLOAD], b[DRAW_COL_BYTES], r[DRAW_COL_BYTES];
    proto_reply_t rep;
    int x, i, ok_bw = 1, ok_red = 1;

    uint8_t len = make_content(buf, 0, 1, ESL_LAYOUT_PRICE, ESL_CF_PRICE_RED, 3, f);
    proto_parse_reply(buf, len, &rep);
    layout_build(&sc, &rep, buf);

    CHECK(epd_draw(&sc));
    CHECK_EQ(first_cmd, 0x12);                   /* software reset first */
    CHECK_EQ(n_bw, DRAW_W * DRAW_COL_BYTES);
    CHECK_EQ(n_red, DRAW_W * DRAW_COL_BYTES);
    CHECK_EQ(last_cmd, 0x10);                    /* deep sleep last */
    CHECK(!powered);

    draw_begin(&sc);
    for (x = 0; x < DRAW_W; x++) {
        draw_column(&sc, (uint16_t)x, b, r);
        for (i = 0; i < DRAW_COL_BYTES; i++) {
            if (plane_bw[x * DRAW_COL_BYTES + i] != (uint8_t)~b[i]) ok_bw = 0;
            if (plane_red[x * DRAW_COL_BYTES + i] != r[i]) ok_red = 0;
        }
    }
    CHECK(ok_bw);
    CHECK(ok_red);
}

int main(void)
{
    RUN(test_epd_stream);
    printf("\n%d checks, %d failures\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}
