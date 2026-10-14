/* fake_epd.c - epd_draw for the PC tests: draws the scene into a whole image in memory. */
#include <stdio.h>
#include "epd.h"
#include "fake.h"

int fake_draw_count;
bool fake_epd_ok = true;
uint8_t fake_fb[DRAW_H][DRAW_W];

void fake_render(draw_scene_t *s)
{
    uint8_t black[DRAW_COL_BYTES], red[DRAW_COL_BYTES];
    int x, y;

    draw_begin(s);
    for (x = 0; x < DRAW_W; x++) {
        draw_column(s, (uint16_t)x, black, red);
        for (y = 0; y < DRAW_H; y++) {
            uint8_t m = (uint8_t)(0x80 >> (y & 7));
            fake_fb[y][x] = (red[y >> 3] & m) ? 2 : (black[y >> 3] & m) ? 1 : 0;
        }
    }
}

int fake_ink_count(int color)
{
    int x, y, n = 0;
    for (y = 0; y < DRAW_H; y++)
        for (x = 0; x < DRAW_W; x++)
            if (fake_fb[y][x] == color)
                n++;
    return n;
}

bool fake_fb_save_ppm(const char *path)
{
    static const unsigned char rgb[3][3] = { { 255, 255, 255 }, { 0, 0, 0 }, { 200, 0, 0 } };
    FILE *f = fopen(path, "wb");
    int x, y;
    if (!f)
        return false;
    fprintf(f, "P6\n%d %d\n255\n", DRAW_W, DRAW_H);
    for (y = 0; y < DRAW_H; y++)
        for (x = 0; x < DRAW_W; x++)
            fwrite(rgb[fake_fb[y][x]], 1, 3, f);
    fclose(f);
    return true;
}

bool epd_draw(draw_scene_t *s)
{
    fake_draw_count++;
    fake_render(s);
    return fake_epd_ok;
}
