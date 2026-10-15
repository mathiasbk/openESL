/* sim_tag.c - hal.h for the simulation: the radio goes to the AP side (sim_link), flash is
 * RAM that survives sim_tag_boot(), and every wait moves the simulated clock.
 * The display is tag/test/fake_epd.c, which draws into an image in memory. */
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "draw.h"
#include "hal/hal.h"
#include "fake.h"
#include "layout.h"
#include "model.h"
#include "sim.h"

uint64_t sim_now_ms;
uint64_t sim_tag_mac = 0x00000123456789A8ull;
const char *sim_tag_model = MODEL_NAME;
bool sim_verbose;
void (*sim_on_draw)(void);

#define AIR_MS 2   /* one frame out and the reply back, roughly as measured on the jig */

static bool radio_is_on;
static uint8_t rx_frame[128];
static uint8_t rx_len;          /* reply waiting for radio_rx, 0 = none */
static uint64_t next_wake_ms;
static uint16_t last_sleep_s;
static uint64_t last_draw_ms;

static uint8_t flash_rec[HAL_STORE_MAX];
static uint8_t flash_rec_len;
static uint8_t flash_lay[HAL_LAYOUT_STORE_MAX];

/* ---- hal.h ---- */

void hal_init(void) {}
void hal_delay_ms(uint8_t ms) { sim_now_ms += ms; }
void hal_wdt_kick(void) {}
void hal_uart_putc(char c) { (void)c; }
void hal_uart_puts(const char *s) { (void)s; }

void hal_mac_read(uint8_t *mac8)
{
    for (int i = 0; i < 8; i++)
        mac8[i] = (uint8_t)(sim_tag_mac >> (8 * i));   /* least significant byte first, as on air */
}

uint16_t battery_read_mv(void) { return 2950; }
int8_t hal_temp_c(void) { return 20; }
void power_sleep_s(uint16_t s) { sim_now_ms += (uint64_t)s * 1000; }

bool hal_store_write(const uint8_t *data, uint8_t len)
{
    if (len > HAL_STORE_MAX || radio_is_on)
        return false;
    memcpy(flash_rec, data, len);
    flash_rec_len = len;
    return true;
}

uint8_t hal_store_read(uint8_t *data, uint8_t maxlen)
{
    if (!flash_rec_len || flash_rec_len > maxlen)
        return 0;
    memcpy(data, flash_rec, flash_rec_len);
    return flash_rec_len;
}

bool hal_layout_store_write(const uint8_t *data, uint16_t len)
{
    if (len == 0 || len > HAL_LAYOUT_STORE_MAX || radio_is_on)
        return false;
    memset(flash_lay, 0xFF, sizeof(flash_lay));
    memcpy(flash_lay, data, len);
    return true;
}

void hal_layout_store_read(uint8_t *data, uint16_t len)
{
    memcpy(data, flash_lay, len);
}

void radio_init(uint8_t channel) { (void)channel; }
void radio_on(void) { radio_is_on = true; }
void radio_off(void) { radio_is_on = false; }

bool radio_tx(const uint8_t *frame, uint8_t len)
{
    if (!radio_is_on)
        return false;
    rx_len = sim_link(frame, len, rx_frame);
    return true;
}

int8_t radio_rx(uint8_t *buf, uint8_t maxlen, uint8_t timeout_ms)
{
    uint8_t n = rx_len;
    if (!radio_is_on || !n) {
        sim_now_ms += timeout_ms;
        return -1;
    }
    sim_now_ms += AIR_MS;
    rx_len = 0;
    if (n > maxlen)
        n = maxlen;
    memcpy(buf, rx_frame, n);
    return (int8_t)n;
}

/* ---- sim.h ---- */

void sim_tag_flash_erase(void)
{
    flash_rec_len = 0;
    memset(flash_lay, 0xFF, sizeof(flash_lay));
}

void sim_tag_boot(void)
{
    int before = fake_draw_count;
    radio_is_on = false;
    rx_len = 0;
    hal_init();
    app_init();
    if (fake_draw_count != before) {
        last_draw_ms = sim_now_ms;
        if (sim_on_draw)
            sim_on_draw();
    }
    next_wake_ms = sim_now_ms;
}

static void wake(void)
{
    int before = fake_draw_count;
    sim_now_ms = next_wake_ms;
    last_sleep_s = app_poll_once();
    if (fake_draw_count != before) {
        last_draw_ms = sim_now_ms;
        if (sim_on_draw)
            sim_on_draw();
    }
    if (sim_verbose)
        printf("  [%7.1f s] tag sleeps %u s (misses %u)\n", sim_now_ms / 1000.0, last_sleep_s,
               (unsigned)app.misses);
    next_wake_ms = sim_now_ms + (uint64_t)last_sleep_s * 1000;
}

void sim_tag_run_until(uint64_t t_ms)
{
    while (next_wake_ms <= t_ms)
        wake();
    if (sim_now_ms < t_ms)
        sim_now_ms = t_ms;
}

bool sim_tag_run_until_draw(uint64_t t_ms)
{
    int before = fake_draw_count;
    while (next_wake_ms <= t_ms) {
        wake();
        if (fake_draw_count != before)
            return true;
    }
    if (sim_now_ms < t_ms)
        sim_now_ms = t_ms;
    return false;
}

uint32_t sim_tag_shown_id(void) { return app.shown_id; }
int sim_tag_draws(void) { return fake_draw_count; }
uint16_t sim_tag_interval_s(void) { return app.interval_s; }
uint16_t sim_tag_last_sleep_s(void) { return last_sleep_s; }
uint8_t sim_tag_misses(void) { return app.misses; }
uint64_t sim_tag_next_wake_ms(void) { return next_wake_ms; }
uint64_t sim_tag_last_draw_ms(void) { return last_draw_ms; }

uint32_t sim_tag_layout_ver(void) { return layout_file_ver; }

bool sim_tag_save_png(const char *path)
{
    enum { S = 2 };   /* scale: the 1.54" is only 200 x 200 */
    static uint8_t pix[DRAW_H * S][DRAW_W * S];
    for (int y = 0; y < DRAW_H * S; y++)
        for (int x = 0; x < DRAW_W * S; x++)
            pix[y][x] = fake_fb[y / S][x / S] ? 0 : 255;
    return png_write_gray(path, &pix[0][0], DRAW_W * S, DRAW_H * S);
}
