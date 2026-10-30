/*
 * main.c - native test driver for web_api.c (web/build.py --native), so the test can
 * compare the WebAssembly build against the same code built with the host compiler.
 *
 *   web_render <layouts.json | -> <layout> <flags> [field ...]
 *
 * Prints "ok <width> <height>" and then one line of 0/1 per pixel row, or
 * "error <message>". "-" means no layout file (built-in layouts only). ESL_TODAY in the
 * environment is today for date fields (days since 1970-01-01; else unknown).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esl_layout.h"

int web_width(void);
int web_height(void);
const char *web_error(void);
unsigned char *web_fb(void);
int web_load_layouts(const char *json, int len);
void web_set_today(int days);
int web_render(int layout, int flags, int n, const char *f0, const char *f1, const char *f2, const char *f3,
               const char *f4, const char *f5);

int main(int argc, char **argv)
{
    static char js[ESL_LAYOUT_MAX_SIZE + 1];
    const char *f[6] = { "", "", "", "", "", "" };
    int i, x, y, n;

    if (argc < 4 || argc > 10) {
        fprintf(stderr, "usage: web_render <layouts.json | -> <layout> <flags> [field ...]\n");
        return 2;
    }
    if (strcmp(argv[1], "-") != 0) {
        FILE *fp = fopen(argv[1], "rb");
        size_t len;
        if (!fp) {
            printf("error cannot read %s\n", argv[1]);
            return 1;
        }
        len = fread(js, 1, sizeof(js), fp);
        fclose(fp);
        if (!web_load_layouts(js, (int)len)) {
            printf("error %s\n", web_error());
            return 1;
        }
    }
    web_set_today(getenv("ESL_TODAY") && *getenv("ESL_TODAY") ? atoi(getenv("ESL_TODAY")) : -1);
    n = argc - 4;
    for (i = 0; i < n; i++)
        f[i] = argv[4 + i];
    if (!web_render(atoi(argv[2]), (int)strtol(argv[3], NULL, 0), n, f[0], f[1], f[2], f[3], f[4], f[5])) {
        printf("error %s\n", web_error());
        return 1;
    }
    printf("ok %d %d\n", web_width(), web_height());
    for (y = 0; y < web_height(); y++) {
        for (x = 0; x < web_width(); x++)
            putchar('0' + web_fb()[y * web_width() + x]);
        putchar('\n');
    }
    return 0;
}
