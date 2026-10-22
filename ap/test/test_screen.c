#include "tinytest.h"
#include "esl_screen.h"
#include <stdlib.h>

#define W 160
#define H 80

static uint8_t pix[W * H];

/* ESL_SCREEN_DUMP=dir writes each screen as a PPM, 4x size (the README pictures). */
static void dump(const char *name)
{
    const char *dir = getenv("ESL_SCREEN_DUMP");
    if (!dir)
        return;
    static const uint8_t rgb[][3] = {
        {0, 0, 0}, {255, 255, 255}, {0, 200, 255}, {0, 220, 0}, {255, 190, 0}, {255, 40, 40}, {140, 140, 140},
    };
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6 %d %d 255\n", W * 4, H * 4);
    for (int y = 0; y < H * 4; y++)
        for (int x = 0; x < W * 4; x++)
            fwrite(rgb[pix[(y / 4) * W + x / 4]], 1, 3, f);
    fclose(f);
}

static unsigned count(uint8_t color, unsigned x0, unsigned x1, unsigned y0, unsigned y1)
{
    unsigned n = 0;
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = x0; x < x1; x++)
            n += pix[y * W + x] == color;
    return n;
}

static esl_screen_info_t base(void)
{
    esl_screen_info_t in;
    memset(&in, 0, sizeof(in));
    in.fw = "0.4.0";
    in.channel = 25;
    in.tags = 12;
    in.tags_max = 64;
    in.ok = 9;
    in.busy = 2;
    in.bad = 1;
    in.seen = 9;
    in.uptime_s = 3 * 86400 + 4 * 3600 + 5;
    in.wifi.mode = ESL_WIFI_STA;
    in.wifi.connected = true;
    strcpy(in.wifi.ssid, "Butikken");
    strcpy(in.wifi.ip, "192.168.1.40");
    in.serial_api = true;
    return in;
}

void test_screen(void)
{
    esl_screen_t s;
    esl_screen_info_t in = base();

    esl_screen_make(&in, &s);
    CHECK_STR(s.text[0], "OpenESL 0.4.0\tch 25");
    CHECK_STR(s.text[1], "\x03" "9 OK  \x04" "2 busy  \x05" "1 bad");
    CHECK_STR(s.text[2], "192.168.1.40");
    CHECK_STR(s.text[3], "up 3 d 4 h, 9 seen");
    CHECK_INT(s.color[2], ESL_SCREEN_TEXT);

    esl_screen_render(&s, pix, W, H);
    dump("connected");
    /* every line has ink, the title in its own color, "ch 25" at the right edge */
    for (unsigned i = 0; i < 4; i++)
        if (i != 1)
            CHECK(count(s.color[i], 0, W, i * 20, i * 20 + 20) > 20);
    /* the tag line: green, then yellow, then red, from left to right, and it fits */
    unsigned last_ok = 0, first_busy = W, last_busy = 0, first_bad = W, last_bad = 0;
    for (unsigned x = 0; x < W; x++) {
        if (count(ESL_SCREEN_OK, x, x + 1, 20, 40))
            last_ok = x;
        if (count(ESL_SCREEN_WARN, x, x + 1, 20, 40)) {
            first_busy = first_busy < x ? first_busy : x;
            last_busy = x;
        }
        if (count(ESL_SCREEN_BAD, x, x + 1, 20, 40)) {
            first_bad = first_bad < x ? first_bad : x;
            last_bad = x;
        }
    }
    CHECK(last_ok > 0 && last_ok < first_busy && last_busy < first_bad);
    CHECK(last_bad < W - 2);
    CHECK_INT(count(ESL_SCREEN_TEXT, 0, W, 20, 40), 0); /* no '?' for the colour bytes */
    CHECK(count(ESL_SCREEN_TITLE, W - 12, W, 0, 20) > 0);
    CHECK_INT(count(ESL_SCREEN_TITLE, W - 2, W, 0, H), 0); /* the margin stays empty */
    CHECK_INT(count(ESL_SCREEN_BG, 0, W, 0, H) + count(ESL_SCREEN_TITLE, 0, W, 0, H) +
                  count(ESL_SCREEN_TEXT, 0, W, 0, H) + count(ESL_SCREEN_OK, 0, W, 0, H) +
                  count(ESL_SCREEN_WARN, 0, W, 0, H) + count(ESL_SCREEN_BAD, 0, W, 0, H) +
                  count(ESL_SCREEN_DIM, 0, W, 0, H),
              W * H);

    /* all OK: only the green part; one tag, short uptime */
    in.busy = in.bad = 0;
    in.ok = in.tags = 1;
    in.uptime_s = 75;
    esl_screen_make(&in, &s);
    CHECK_STR(s.text[1], "\x03" "1 OK");
    in.ok = 0;
    in.bad = 1;
    esl_screen_make(&in, &s);
    CHECK_STR(s.text[1], "\x05" "1 bad");
    CHECK_STR(s.text[3], "up 75 s, 9 seen");
    in.uptime_s = 3600 + 60 * 7;
    esl_screen_make(&in, &s);
    CHECK_STR(s.text[3], "up 67 min, 9 seen");
    in.uptime_s = 5 * 3600 + 60 * 7;
    esl_screen_make(&in, &s);
    CHECK_STR(s.text[3], "up 5 h 7 min, 9 seen");

    in.tags = 0;
    esl_screen_make(&in, &s);
    CHECK_STR(s.text[1], "No tags yet");
    in.tags = 64;
    esl_screen_make(&in, &s);
    CHECK_STR(s.text[1], "64 tags: table full");
    CHECK_INT(s.color[1], ESL_SCREEN_BAD);

    /* first start: the setup network */
    in = base();
    in.wifi.mode = ESL_WIFI_SETUP;
    in.wifi.connected = false;
    in.wifi.ssid[0] = 0;
    strcpy(in.wifi.setup_ssid, "openesl-1A2B");
    strcpy(in.wifi.setup_ip, "192.168.4.1");
    esl_screen_make(&in, &s);
    CHECK_STR(s.text[2], "Setup: openesl-1A2B");
    CHECK_STR(s.text[3], "192.168.4.1/setup");
    CHECK_INT(s.color[2], ESL_SCREEN_WARN);
    esl_screen_render(&s, pix, W, H);
    dump("setup");

    /* joining, with a name in UTF-8 that the font has in Latin-1 */
    in = base();
    in.wifi.connected = false;
    strcpy(in.wifi.ssid, "Kj\xC3\xB8kken");
    esl_screen_make(&in, &s);
    CHECK_STR(s.text[2], "Joining Kj\xF8kken");
    esl_screen_render(&s, pix, W, H);
    dump("joining");

    /* WiFi dead (the Super Mini) */
    in.wifi.mode = ESL_WIFI_OFF;
    esl_screen_make(&in, &s);
    CHECK_STR(s.text[2], "WiFi off, API on USB");
    CHECK_INT(s.color[2], ESL_SCREEN_BAD);
    esl_screen_render(&s, pix, W, H);
    dump("wifi-off");

    /* a long version and a long SSID are cut, never drawn over "ch" or past the edge */
    in = base();
    in.fw = "v0.4.0-12-gabcdef0-dirty-and-more";
    in.wifi.connected = false;
    strcpy(in.wifi.ssid, "12345678901234567890123456789012");
    esl_screen_make(&in, &s);
    CHECK(strlen(s.text[2]) < ESL_SCREEN_LINE_MAX);
    esl_screen_render(&s, pix, W, H);
    CHECK_INT(count(ESL_SCREEN_TITLE, W - 2, W, 0, H), 0);
    CHECK_INT(count(ESL_SCREEN_WARN, W - 2, W, 0, H), 0);
    /* a gap of at least two columns before "ch 25" */
    unsigned ch_x = W;
    for (unsigned x = W - 1; x > 0; x--)
        if (count(ESL_SCREEN_TITLE, x, x + 1, 0, 20))
            ch_x = x;
        else if (ch_x < W && count(ESL_SCREEN_TITLE, x - 1, x + 1, 0, 20) == 0)
            break;
    CHECK(ch_x > W / 2);

    /* an odd size does not write outside the buffer (ASan) */
    static uint8_t small[61 * 30];
    esl_screen_make(&in, &s);
    esl_screen_render(&s, small, 61, 30);
}
