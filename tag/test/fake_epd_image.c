/* fake_epd_image.c - epd_draw_image for the PC tests: the image in the (fake) SPI flash
 * slot into fake_fb. Apart from fake_epd.c, so builds without image.c (the web preview)
 * do not need it. */
#include "epd.h"
#include "image.h"
#include "fake.h"

bool epd_draw_image(void)
{
    uint8_t black[DRAW_COL_BYTES], red[DRAW_COL_BYTES];
    int x, y;

    fake_draw_count++;
    for (x = 0; x < DRAW_W; x++) {
        image_column((uint16_t)x, black, red);
        for (y = 0; y < DRAW_H; y++) {
            uint8_t m = (uint8_t)(0x80 >> (y & 7));
            fake_fb[y][x] = (red[y >> 3] & m) ? 2 : (black[y >> 3] & m) ? 1 : 0;
        }
    }
    return fake_epd_ok;
}
