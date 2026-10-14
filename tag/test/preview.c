/*
 * preview.c - draws content to PPM images (DRAW_W x DRAW_H of the model it is built for)
 * so the layouts can be checked on the PC before the display works.
 *
 *   preview <dir>                                   the example set below, into <dir>
 *   preview -o <file.ppm> <layout> <flags> [field ...]
 *                                                   one image; layout 1 = price, 2 = text,
 *                                                   flags as in CONTENT, fields in UTF-8
 *                                                   (converted to Latin-1 like the AP does)
 *   preview -L <layouts.bin> -o <file.ppm> <layout> <flags> [field ...]
 *                                                   the same with a downloaded layout file
 *                                                   (layout 16 + n = layout n of the file),
 *                                                   installed with the tag's own check
 *
 * Used by 'python tools/build.py preview' and 'python tools/render_host.py'.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fake.h"
#include "helpers.h"
#include "layout.h"

static int one(const char *path, uint8_t layout, uint8_t flags, int n, const char *const *f)
{
    static draw_scene_t sc;
    static uint8_t buf[ESL_MAX_PAYLOAD + 64];
    proto_reply_t r;
    uint8_t len;
    int i, total = 0;

    for (i = 0; i < n; i++)
        total += 1 + (int)strlen(f[i]);
    if (n > ESL_MAX_FIELDS || ESL_CONTENT_HDR_LEN + total > ESL_MAX_PAYLOAD + 32) {
        printf("%s: too much text\n", path);
        return 1;
    }
    len = make_content(buf, 0, 1, layout, flags, n, f);
    if (len > ESL_MAX_PAYLOAD || proto_parse_reply(buf, len, &r) != PROTO_OK) {
        printf("%s: invalid packet (%d bytes, max %d)\n", path, len, ESL_MAX_PAYLOAD);
        return 1;
    }
    layout_build(&sc, &r, buf);
    fake_render(&sc);
    if (!fake_fb_save_ppm(path)) {
        printf("%s: cannot write\n", path);
        return 1;
    }
    printf("%s\n", path);
    return 0;
}

/* The start screen the tag shows before it has content (app.c draw_status). */
static int status(const char *path)
{
    static draw_scene_t sc;
    static const char *const s[] = { "OpenESL", "MAC 0000000197E5CB3B", "FW 0.1.0", "Waiting for content" };
    const uint8_t *t[4];
    uint8_t len[4];
    int i;
    for (i = 0; i < 4; i++)
        t[i] = L(s[i], &len[i]);
    layout_build_lines(&sc, t, len, 4);
    fake_render(&sc);
    printf("%s\n", path);
    return fake_fb_save_ppm(path) ? 0 : 1;
}

#define P ESL_LAYOUT_PRICE
#define T ESL_LAYOUT_TEXT
#define HL ESL_CF_PRICE_HIGHLIGHT
#define FRAME ESL_CF_FRAME

static const struct {
    const char *file;
    uint8_t layout, flags;
    const char *f[4];
} examples[] = {
    { "pris_framhevet",  P, HL,          { "Lettmelk 1 L", "24.90", "kr 24,90/l" } },
    { "pris_komma",      P, 0,           { "Brunost Gudbrandsdalsost", "89,-", "kr/kg 178,00" } },
    { "pris_langt_navn", P, 0,           { "Kjøttdeig av storfe 14 % fett, ekstra stor familiepakning", "129,90", "kr/kg 108,25" } },
    { "pris_for_langt",  P, HL,          { "Grovbrød med solsikkekjerner, havre og knekt rug, oppskåret og pakket i papirpose 750 g", "39,90", "" } },
    { "pris_bred",       P, HL,          { "Kaffemaskin", "12 499,00", "Spar kr 2 500" } },
    { "pris_tekst",      P, FRAME,       { "Appelsiner", "3 for 30", "Gjelder til søndag" } },
    { "pris_ramme",      P, HL | FRAME,  { "Rømme lett 18 % Tine", "19,90", "Ærlig økologisk – kr 66,33/l" } },
    { "tekst",           T, 0,           { "Åpningstider", "Man–fre 9–21", "Lørdag 9–18", "Søndag stengt" } },
    { "tekst_lang",      T, FRAME,       { "Velkommen!", "Denne linjen er altfor lang til å få plass", "", "ÆØÅ æøå é ü ß ©" } },
};

/* Loads a layout file the way the tag stores a finished download. */
static int load_layout_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    uint32_t crc;

    if (!f) {
        printf("%s: cannot read\n", path);
        return 1;
    }
    n = fread(layout_file, 1, ESL_LBIN_MAX + 1, f);
    fclose(f);
    crc = proto_crc32(0, layout_file, (uint16_t)n);
    if (n > ESL_LBIN_MAX || !layout_file_install((uint16_t)n, crc ? crc : 1)) {
        printf("%s: the tag would reject this layout file\n", path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char p[512];
    const char *dir = ".";
    unsigned i;
    int err = 0;

    if (argc >= 3 && strcmp(argv[1], "-L") == 0) {
        if (load_layout_file(argv[2]))
            return 1;
        argc -= 2;
        argv += 2;
    }
    if (argc >= 5 && strcmp(argv[1], "-o") == 0)
        return one(argv[2], (uint8_t)atoi(argv[3]), (uint8_t)strtol(argv[4], NULL, 0),
                   argc - 5, (const char *const *)(argv + 5));
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        printf("usage: preview <dir>\n       preview -o <file.ppm> <layout> <flags> [field ...]\n");
        return 2;
    }
    if (argc > 1)
        dir = argv[1];
    for (i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        int n = examples[i].layout == T ? 4 : 3;
        snprintf(p, sizeof(p), "%s/%s.ppm", dir, examples[i].file);
        err |= one(p, examples[i].layout, examples[i].flags, n, examples[i].f);
    }
    snprintf(p, sizeof(p), "%s/status.ppm", dir);
    err |= status(p);
    return err;
}
