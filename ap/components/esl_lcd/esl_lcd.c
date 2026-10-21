#include "esl_lcd.h"
#include "sdkconfig.h"

#if CONFIG_ESL_DISPLAY
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

/* A small ST7789 LCD, e.g. on the ESP32-C6 boards with a 0.96" 160x80 screen.
 * The text comes from esl_screen.c (host-tested); this file only drives the
 * panel. */

static const char *TAG = "lcd";

#define W CONFIG_ESL_LCD_WIDTH
#define H CONFIG_ESL_LCD_HEIGHT
#define BAND 16 /* rows sent per draw_bitmap */

static esp_lcd_panel_handle_t panel;
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

esp_err_t esl_lcd_show(const esl_screen_t *s)
{
    if (!panel)
        return ESP_ERR_INVALID_STATE;
    esl_screen_render(s, pix, W, H);
    flush();
    return ESP_OK;
}

esp_err_t esl_lcd_start(const esl_screen_t *s)
{
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

    /* the first screen with a frame, to check the gaps */
    esl_screen_render(s, pix, W, H);
    for (int x = 0; x < W; x++)
        pix[x] = pix[(H - 1) * W + x] = ESL_SCREEN_WARN;
    for (int y = 0; y < H; y++)
        pix[y * W] = pix[y * W + W - 1] = ESL_SCREEN_WARN;
    flush();
#if CONFIG_ESL_LCD_PIN_BL >= 0
    gpio_set_level(CONFIG_ESL_LCD_PIN_BL, CONFIG_ESL_LCD_BL_ON_LEVEL);
#endif
    ESP_LOGI(TAG, "ST7789 %dx%d", W, H);
    return ESP_OK;
}

#else

esp_err_t esl_lcd_start(const esl_screen_t *s)
{
    (void)s;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esl_lcd_show(const esl_screen_t *s)
{
    (void)s;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
