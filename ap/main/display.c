#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esl_screen.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* AP status on a small ST7789 LCD (ESL_DISPLAY), e.g. the ESP32-C6 boards with a
 * 0.96" 160x80 screen. The text comes from esl_screen.c (host-tested); this file
 * only drives the panel. At start it shows a frame around the edge for two
 * seconds: if a side is missing or doubled, adjust ESL_LCD_GAP_X/Y. */

static const char *TAG = "display";

#define W CONFIG_ESL_LCD_WIDTH
#define H CONFIG_ESL_LCD_HEIGHT
#define BAND 16 /* rows sent per draw_bitmap */
#define SEEN_RECENT_MS (10 * 60 * 1000)

static esp_lcd_panel_handle_t panel;
static const esl_api_t *api;
static uint8_t *pix;
static uint16_t *band;

/* esl_screen_color_t -> RGB565, byte-swapped for the SPI bus */
#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define SWAP(c) (uint16_t)(((c) >> 8) | ((c) << 8))
static const uint16_t palette[] = {
    SWAP(RGB(0, 0, 0)),       SWAP(RGB(255, 255, 255)), SWAP(RGB(0, 200, 255)), SWAP(RGB(0, 220, 0)),
    SWAP(RGB(255, 190, 0)),   SWAP(RGB(255, 40, 40)),   SWAP(RGB(140, 140, 140)),
};

static void flush(void)
{
    for (int y = 0; y < H; y += BAND) {
        int rows = H - y < BAND ? H - y : BAND;
        for (int i = 0; i < rows * W; i++)
            band[i] = palette[pix[y * W + i] % (sizeof(palette) / sizeof(palette[0]))];
        esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + rows, band);
    }
}

static void gather(esl_screen_info_t *in)
{
    memset(in, 0, sizeof(*in));
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
    in->fw = api->fw_version;
    in->channel = api->channel;
    in->tags_max = api->store->max;
    in->table_full = esl_store_full(api->store);
    in->uptime_s = now_ms / 1000;
#if CONFIG_ESL_SERIAL_API
    in->serial_api = true;
#endif
    for (unsigned s = 0, n = esl_store_slots(api->store); s < n; s++) {
        esl_tag_t t;
        if (!esl_store_get_slot(api->store, s, &t))
            continue;
        in->tags++;
        if (esl_tag_pending(&t))
            in->pending++;
        if (t.seen && now_ms - t.last_seen_ms < SEEN_RECENT_MS)
            in->seen++;
    }
    if (api->wifi)
        api->wifi->status(api->wifi->ctx, &in->wifi);
}

static void display_task(void *arg)
{
    (void)arg;
    esl_screen_info_t *in = malloc(sizeof(*in));
    esl_screen_t *scr = malloc(sizeof(*scr));
    if (!in || !scr) {
        ESP_LOGE(TAG, "out of memory");
        vTaskDelete(NULL);
    }
    vTaskDelay(pdMS_TO_TICKS(2000)); /* the frame from splash(), to check the gaps */
    for (;;) {
        gather(in);
        esl_screen_make(in, scr);
        esl_screen_render(scr, pix, W, H);
        flush();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ESL_LCD_REFRESH_MS));
    }
}

static void splash(void)
{
    esl_screen_t s = {0};
    strcpy(s.text[0], "OpenESL AP");
    strncpy(s.text[1], api->fw_version ? api->fw_version : "", ESL_SCREEN_LINE_MAX - 1);
    strcpy(s.text[2], "starting");
    s.color[0] = ESL_SCREEN_TITLE;
    s.color[1] = s.color[2] = ESL_SCREEN_TEXT;
    esl_screen_render(&s, pix, W, H);
    for (int x = 0; x < W; x++)
        pix[x] = pix[(H - 1) * W + x] = ESL_SCREEN_WARN;
    for (int y = 0; y < H; y++)
        pix[y * W] = pix[y * W + W - 1] = ESL_SCREEN_WARN;
    flush();
}

esp_err_t display_start(const esl_api_t *a)
{
    api = a;
    pix = malloc(W * H);
    band = heap_caps_malloc(W * BAND * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!pix || !band)
        return ESP_ERR_NO_MEM;

#if CONFIG_ESL_LCD_PIN_BL >= 0
    gpio_config_t bl = {.pin_bit_mask = 1ULL << CONFIG_ESL_LCD_PIN_BL, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(CONFIG_ESL_LCD_PIN_BL, !CONFIG_ESL_LCD_BL_ON_LEVEL);
#endif

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_ESL_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_ESL_LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = W * BAND * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK)
        return err;

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CONFIG_ESL_LCD_PIN_DC,
        .cs_gpio_num = CONFIG_ESL_LCD_PIN_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io);
    if (err != ESP_OK)
        return err;

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = CONFIG_ESL_LCD_PIN_RST,
#if CONFIG_ESL_LCD_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &dev, &panel);
    if (err != ESP_OK)
        return err;
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
#if CONFIG_ESL_LCD_INVERT
    esp_lcd_panel_invert_color(panel, true);
#endif
#if CONFIG_ESL_LCD_ROTATE_90
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, true);
#elif CONFIG_ESL_LCD_ROTATE_270
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, true, false);
#endif
    esp_lcd_panel_set_gap(panel, CONFIG_ESL_LCD_GAP_X, CONFIG_ESL_LCD_GAP_Y);
    esp_lcd_panel_disp_on_off(panel, true);

    splash();
#if CONFIG_ESL_LCD_PIN_BL >= 0
    gpio_set_level(CONFIG_ESL_LCD_PIN_BL, CONFIG_ESL_LCD_BL_ON_LEVEL);
#endif
    ESP_LOGI(TAG, "ST7789 %dx%d", W, H);
    if (xTaskCreate(display_task, "display", 4096, NULL, 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
