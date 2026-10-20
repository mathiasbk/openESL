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
#include "boot_core.h"
#include "otaslot.h"
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

static int restarts;

void sim_tag_flash_erase(void)
{
    flash_rec_len = 0;
    memset(flash_lay, 0xFF, sizeof(flash_lay));
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

uint32_t sim_tag_layout_ver(void) { return layout_file_ver; }

/* ---- drawing with the tag's own code, for the comparison with esl_render ---- */

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

/* UTF-8 to Latin-1 the way the AP does it for the characters the tests use (esl_text.c) */
static uint8_t utf8_field(uint8_t *out, const char *s)
{
    uint8_t n = 0;
    while (*s && n < 255) {
        unsigned char c = (unsigned char)*s++;
        if (c < 0x80) {
            out[n++] = c;
        } else if ((c & 0xE0) == 0xC0 && *s) {
            unsigned cp = ((c & 0x1F) << 6) | ((unsigned char)*s++ & 0x3F);
            out[n++] = cp < 256 ? (uint8_t)cp : '?';
        } else if ((c & 0xF0) == 0xE0 && s[0] && s[1]) {
            unsigned cp = ((c & 0x0F) << 12) | (((unsigned char)s[0] & 0x3F) << 6) | ((unsigned char)s[1] & 0x3F);
            s += 2;
            out[n++] = cp == 0x2013 || cp == 0x2014 ? '-' : cp == 0x2018 || cp == 0x2019 ? '\'' :
                       cp == 0x201C || cp == 0x201D ? '"' : '?';
            if (cp == 0x2026 && n < 253) {           /* "..." */
                out[n - 1] = '.';
                out[n++] = '.';
                out[n++] = '.';
            }
        } else {
            out[n++] = '?';
            while ((*s & 0xC0) == 0x80)
                s++;
        }
    }
    return n;
}

size_t sim_tag_render(const uint8_t *bin, size_t len, uint8_t layout, uint8_t flags, int n, const char *const *fields,
                      uint8_t *out)
{
    static uint8_t buf[ESL_MAX_FIELDS * 256];
    static draw_scene_t sc;
    proto_reply_t r;
    uint8_t pos = 0;

    layout_file_clear();
    if (bin) {
        uint32_t crc;
        if (len > ESL_LBIN_MAX)
            return 0;
        memcpy(layout_file, bin, len);
        crc = proto_crc32(0, layout_file, (uint16_t)len);
        if (!layout_file_install((uint16_t)len, crc ? crc : 1))
            return 0;
    }
    memset(&r, 0, sizeof(r));
    r.layout = layout;
    r.flags = flags;
    while (n > 0 && !fields[n - 1][0])
        n--;                                        /* the AP drops empty fields at the end */
    r.nfields = (uint8_t)n;
    for (int i = 0; i < n && i < ESL_MAX_FIELDS; i++) {
        r.field_off[i] = pos;
        r.field_len[i] = utf8_field(buf + pos, fields[i]);
        pos = (uint8_t)(pos + r.field_len[i]);
    }
    layout_build(&sc, &r, buf);
    fake_render(&sc);
    return sim_tag_screen(out, (size_t)DRAW_W * DRAW_COL_BYTES * 2);
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
