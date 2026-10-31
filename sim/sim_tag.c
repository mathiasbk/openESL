/* sim_tag.c - hal.h for the simulation: the radio goes to the AP side (sim_link), flash is
 * RAM that survives sim_tag_boot(), and every wait moves the simulated clock.
 * The display is tag/test/fake_epd.c, which draws into an image in memory. */
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "draw.h"
#include "hal/hal.h"
#include "fake.h"
#include "model.h"
#include "sim.h"
#include "boot_core.h"
#include "otaslot.h"
#include "image.h"
#include "proto.h"

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
bool hal_doze_1s(void) { sim_now_ms += 1000; return true; }
/* the radio's RSSI byte and LQI for every answer (not a measured scale) */
uint8_t radio_rx_rssi(void) { return 0xC8; }
uint8_t radio_rx_lqi(void) { return 0x6E; }

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

static uint8_t radio_channel;
void radio_init(uint8_t channel) { radio_channel = channel; }
uint8_t sim_tag_channel(void) { return radio_channel; }
void radio_on(void) { radio_is_on = true; }
void radio_off(void) { radio_is_on = false; }

bool radio_tx(const uint8_t *frame, uint8_t len)
{
    if (!radio_is_on)
        return false;
    rx_len = sim_link(radio_channel, frame, len, rx_frame);
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

static int restarts;

void sim_tag_flash_erase(void)
{
    flash_rec_len = 0;
    fake_xflash_clear(true);   /* the SPI flash is there, so it takes firmware updates */
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
    if (fake_resets) {
        /* ota_install() restarted the tag: the bootloader, then the application anew */
        fake_resets = 0;
        restarts++;
        if (sim_verbose)
            printf("  [%7.1f s] tag restarts for a firmware update\n", sim_now_ms / 1000.0);
        boot_core_run();
        sim_tag_boot();
        return;
    }
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
bool sim_tag_marked(void) { return image.mark != 0; }
uint16_t sim_tag_fw_version(void) { return FW_VERSION; }
uint8_t sim_tag_model_code(void) { return MODEL_CODE; }
int sim_tag_restarts(void) { return restarts; }
bool sim_tag_app_is(const uint8_t *img, size_t len)
{
    return memcmp(fake_code + ESL_FW_APP_BASE, img, len) == 0;
}
bool sim_tag_ota_confirmed(void)
{
    return fake_xflash[OTA_STATE + OTA_ST_CONFIRMED] == 0;
}
int sim_tag_draws(void) { return fake_draw_count; }
uint16_t sim_tag_interval_s(void) { return app.interval_s; }
uint16_t sim_tag_last_sleep_s(void) { return last_sleep_s; }
uint8_t sim_tag_misses(void) { return app.misses; }
uint64_t sim_tag_next_wake_ms(void) { return next_wake_ms; }
uint64_t sim_tag_last_draw_ms(void) { return last_draw_ms; }

/* ---- the tag's screen, for the comparison with esl_render ---- */

/* fake_fb as columns: DRAW_COL_BYTES per column, MSB = row 0, black plane then (with red) the red plane */
size_t sim_tag_screen(uint8_t *out, size_t cap)
{
    size_t plane = (size_t)DRAW_W * DRAW_COL_BYTES, size = plane * (MODEL_HAS_RED ? 2 : 1);
    if (cap < size)
        return 0;
    memset(out, 0, size);
    for (int x = 0; x < DRAW_W; x++)
        for (int y = 0; y < DRAW_H; y++) {
            uint8_t v = fake_fb[y][x];
            if (v == 1)
                out[(size_t)x * DRAW_COL_BYTES + y / 8] |= (uint8_t)(0x80 >> (y & 7));
            else if (v == 2 && MODEL_HAS_RED)
                out[plane + (size_t)x * DRAW_COL_BYTES + y / 8] |= (uint8_t)(0x80 >> (y & 7));
        }
    return size;
}

bool sim_tag_save_png(const char *path)
{
    enum { S = 2 };   /* scale: the 1.54" is only 200 x 200 */
    static uint8_t pix[DRAW_H * S][DRAW_W * S];
    for (int y = 0; y < DRAW_H * S; y++)
        for (int x = 0; x < DRAW_W * S; x++)
            pix[y][x] = fake_fb[y / S][x / S] == 1 ? 0 : fake_fb[y / S][x / S] == 2 ? 128 : 255;
    return png_write_gray(path, &pix[0][0], DRAW_W * S, DRAW_H * S);
}
