/* fake_epd.c - epd_draw for the PC tests: draws the scene into a whole image in memory. */
#include <stdio.h>
#include "epd.h"
#include "fake.h"
#include "proto_tag.h"

int fake_draw_count;
int fake_refresh_count;
bool fake_epd_ok = true;
uint32_t epd_shown_crc;
uint8_t epd_refreshed;
uint8_t fake_fb[DRAW_H][DRAW_W];

void fake_render(draw_scene_t *s)
{
    uint8_t black[DRAW_COL_BYTES], red[DRAW_COL_BYTES];   /* red stays empty */
    int x, y;

    draw_begin(s);
    for (x = 0; x < DRAW_W; x++) {
        draw_column(s, (uint16_t)x, black, red);
        for (y = 0; y < DRAW_H; y++) {
            uint8_t m = (uint8_t)(0x80 >> (y & 7));
            fake_fb[y][x] = (black[y >> 3] & m) ? 1 : 0;
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
    static const unsigned char rgb[3][3] = { { 255, 255, 255 }, { 0, 0, 0 } };
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

/* As epd.c: the refresh is skipped when the image is the one the panel shows. */
bool epd_draw(draw_scene_t *s)
{
    uint32_t crc = 0;
    int y;

    fake_draw_count++;
    epd_refreshed = 0;
    if (!fake_epd_ok) {
        epd_shown_crc = 0;
        return false;
    }
    fake_render(s);
    for (y = 0; y < DRAW_H; y++)
        crc = proto_crc32(crc, fake_fb[y], DRAW_W);
    if (!crc || crc != epd_shown_crc) {
        fake_refresh_count++;
        epd_refreshed = 1;
    }
    epd_shown_crc = crc;
    return true;
}
