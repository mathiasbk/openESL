/*
 * preview.c - draws example content to PPM images (296 x 128) so the layouts can
 * checked on the PC before the display works. Used by 'python tools/build.py preview'.
 */
#include <stdio.h>
#include "fake.h"
#include "helpers.h"
#include "layout.h"

static void one(const char *path, uint8_t layout, uint8_t flags, int n, const char *const *f)
{
    static draw_scene_t sc;
    static uint8_t buf[ESL_MAX_PAYLOAD];
    proto_reply_t r;
    uint8_t len = make_content(buf, 0, 1, layout, flags, n, f);
    if (proto_parse_reply(buf, len, &r) != PROTO_OK) {
        printf("%s: ugyldig pakke\n", path);
        return;
    }
    layout_build(&sc, &r, buf);
    fake_render(&sc);
    fake_fb_save_ppm(path);
    printf("%s\n", path);
}

int main(int argc, char **argv)
{
    char p[512];
    const char *dir = argc > 1 ? argv[1] : ".";
    static const char *const a[] = { "Lettmelk 1 L", "24,90", "kr/l 24,90" };
    static const char *const b[] = { "Minced beef 14 % fat, extra large pack 1.2 kg", "129,90", "per kg 108,25" };
    static const char *const c[] = { "Oranges", "2 for 30", "Valid until Sunday" };
    static const char *const d[] = { "Welcome", "Open 9-21", "Saturday 9-18", "Sunday closed" };
    static const char *const e[] = { "Brunost Gudbrandsdalsost", "89,-", "kr/kg 178,00" };

    snprintf(p, sizeof(p), "%s/pris_rod.ppm", dir);       one(p, ESL_LAYOUT_PRICE, ESL_CF_PRICE_RED, 3, a);
    snprintf(p, sizeof(p), "%s/pris_langt_navn.ppm", dir); one(p, ESL_LAYOUT_PRICE, 0, 3, b);
    snprintf(p, sizeof(p), "%s/pris_tekst.ppm", dir);     one(p, ESL_LAYOUT_PRICE, ESL_CF_RED_FRAME, 3, c);
    snprintf(p, sizeof(p), "%s/tekst.ppm", dir);          one(p, ESL_LAYOUT_TEXT, 0, 4, d);
    snprintf(p, sizeof(p), "%s/pris_ramme.ppm", dir);     one(p, ESL_LAYOUT_PRICE, ESL_CF_PRICE_RED | ESL_CF_RED_FRAME, 3, e);
    return 0;
}
