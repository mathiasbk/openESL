/*
 * web_api.c - the label renderer for the web page (web/), built to WebAssembly with
 * Emscripten by web/build.py, once per tag model (-DMODEL=29 or 154).
 *
 * It links the AP's own code: the layout compiler (esl_layout_compile.c) and the drawing
 * (esl_render.c, with the tag's fonts and barcodes). What the page shows is therefore the
 * image the AP sends to a tag of that model, and a layout file the page accepts is one the
 * AP accepts.
 *
 *   web_load_layouts(json, len)  compiles a layout file (docs/protokoll.md part 3) for this
 *                                model. 1 = ok, 0 = rejected, web_error() says why (the
 *                                AP's 422 message)
 *   web_clear_layouts()          back to only the built-in layouts
 *   web_render(layout, flags, n, f0, ..., f5)
 *                                draws content with fields in UTF-8 (converted to Latin-1 as
 *                                the AP does) into the frame buffer. layout 1 = price,
 *                                2 = text, 16 + n = layout n of the file. 1 = ok, 0 = error
 *   web_fb()                     web_height() rows of web_width() bytes, 0 = white,
 *                                1 = black, 2 = red
 */
#include <stdio.h>
#include <string.h>
#include <emscripten/emscripten.h>
#include "esl_content.h"
#include "esl_crc32.h"
#include "esl_layout.h"
#include "esl_render.h"
#include "esl_text.h"
#include "model_gen.h"

static char err[ESL_LAYOUT_ERR_MAX];
static esl_layout_bin_t bin;
static uint32_t bin_ver;          /* 0 = no file */
static uint8_t img[ESL_RENDER_COL_BYTES(DRAW_H) * DRAW_W * 2];
static uint8_t fb[DRAW_H][DRAW_W];

EMSCRIPTEN_KEEPALIVE int web_width(void) { return DRAW_W; }
EMSCRIPTEN_KEEPALIVE int web_height(void) { return DRAW_H; }
EMSCRIPTEN_KEEPALIVE int web_model_code(void) { return MODEL_CODE; }
EMSCRIPTEN_KEEPALIVE const char *web_error(void) { return err; }
EMSCRIPTEN_KEEPALIVE uint8_t *web_fb(void) { return &fb[0][0]; }
/* Text and one length byte per field sent share this many bytes in CONTENT */
EMSCRIPTEN_KEEPALIVE int web_max_text(void) { return ESL_CONTENT_FIELD_SPACE; }

/* The limits the compiler checks, so the page can show how close a layout is to them:
 * 0 lines of text per screen, 1 rectangles per screen, 2 bytes in the file, 3 border of an
 * inverted field, 4 lines per text element, 5..8 height of the fonts small, medium, mid, large,
 * 9 barcodes per screen, 10 lowest barcode, 11 widest barcode module, 12 bytes per fixed text,
 * 13 fields per layout. */
EMSCRIPTEN_KEEPALIVE int web_limit(int what)
{
    static const int v[] = { ESL_DRAW_MAX_RUNS, ESL_DRAW_MAX_RECTS, ESL_LBIN_MAX, ESL_INVERT_PAD,
                             ESL_LE_MAX_LINES, ESL_FONT_SMALL_H, ESL_FONT_MEDIUM_H, ESL_FONT_MID_H,
                             ESL_FONT_LARGE_H, ESL_DRAW_MAX_BARCODES, ESL_BC_MIN_H, ESL_BC_MODULE_MAX,
                             ESL_LBIN_TEXT_LEN_MAX, ESL_MAX_FIELDS };
    return what >= 0 && what < (int)(sizeof(v) / sizeof(v[0])) ? v[what] : 0;
}

/* The compiled file, as the AP stores it. Valid after web_load_layouts() returned 1. */
EMSCRIPTEN_KEEPALIVE const uint8_t *web_layout_bin(void) { return bin.bin; }
EMSCRIPTEN_KEEPALIVE int web_layout_size(void) { return bin_ver ? (int)bin.size : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t web_layout_version(void) { return bin_ver; }

EMSCRIPTEN_KEEPALIVE int web_load_layouts(const char *json, int len)
{
    err[0] = 0;
    bin_ver = 0;
    if (len < 0 || len > ESL_LAYOUT_MAX_SIZE) {
        snprintf(err, sizeof(err), "the file is %d bytes, the AP takes at most %d", len, ESL_LAYOUT_MAX_SIZE);
        return 0;
    }
    if (!esl_layout_compile(json, (size_t)len, MODEL_CODE, &bin, err, sizeof(err)))
        return 0;
    bin_ver = esl_crc32(bin.bin, bin.size);
    if (!bin_ver)
        bin_ver = 1;
    return 1;
}

EMSCRIPTEN_KEEPALIVE void web_clear_layouts(void)
{
    bin_ver = 0;
}

/* Latin-1 bytes of one UTF-8 field as the AP sends it (255 if it is too long to count). */
EMSCRIPTEN_KEEPALIVE int web_text_bytes(const char *utf8)
{
    static uint8_t tmp[255];
    bool overflow = false;
    size_t n = esl_utf8_to_latin1(utf8, strlen(utf8), tmp, sizeof(tmp), &overflow);
    return overflow ? 255 : (int)n;
}

_Static_assert(ESL_MAX_FIELDS == 6, "web_render takes six fields");

EMSCRIPTEN_KEEPALIVE int web_render(int layout, int flags, int n, const char *f0, const char *f1,
                                    const char *f2, const char *f3, const char *f4, const char *f5)
{
    static uint8_t text[255];
    const char *const f[] = { f0, f1, f2, f3, f4, f5 };
    esl_content_t c;
    uint16_t w, h;
    bool red;
    size_t size, cb;
    int i, total = 0;

    err[0] = 0;
    if (n < 0 || n > ESL_MAX_FIELDS) {
        snprintf(err, sizeof(err), "at most %d fields", ESL_MAX_FIELDS);
        return 0;
    }
    for (i = 0; i < n; i++) {
        int b = web_text_bytes(f[i]);
        if (b > 255 - 3) {
            snprintf(err, sizeof(err), "field %d is too long", i + 1);
            return 0;
        }
        total += b;
    }
    while (n > 0 && !f[n - 1][0])
        n--; /* the AP drops empty fields at the end */
    if (total > ESL_MAX_TEXT_FOR(n)) {
        snprintf(err, sizeof(err), "%d bytes of text, the AP takes at most %d with %d fields", total,
                 ESL_MAX_TEXT_FOR(n), n);
        return 0;
    }
    if (layout < 0 || layout > 255 || esl_content_init(&c, (uint8_t)layout, (uint8_t)flags) != ESL_CONTENT_OK) {
        snprintf(err, sizeof(err), "unknown layout %d", layout);
        return 0;
    }
    if (n > esl_content_max_fields(c.layout)) {
        snprintf(err, sizeof(err), "this layout takes at most %d fields", esl_content_max_fields(c.layout));
        return 0;
    }
    for (i = 0; i < n; i++) {
        size_t len = esl_utf8_to_latin1(f[i], strlen(f[i]), text, sizeof(text), NULL);
        if (esl_content_add_field(&c, text, len) != ESL_CONTENT_OK) {
            snprintf(err, sizeof(err), "field %d does not fit", i + 1);
            return 0;
        }
    }
    size = esl_render(MODEL_CODE, bin_ver ? &bin : NULL, &c, img, sizeof(img));
    if (!size || !esl_render_model(MODEL_CODE, &w, &h, &red) || w != DRAW_W || h != DRAW_H) {
        snprintf(err, sizeof(err), "the AP cannot draw this content");
        return 0;
    }
    /* columns of bits (esl_render.h) to rows of pixels; the red plane follows the black */
    cb = ESL_RENDER_COL_BYTES(DRAW_H);
    for (int x = 0; x < DRAW_W; x++)
        for (int y = 0; y < DRAW_H; y++) {
            uint8_t m = (uint8_t)(0x80 >> (y & 7));
            size_t at = (size_t)x * cb + (size_t)(y >> 3);
            fb[y][x] = (img[at] & m) ? 1 : (red && (img[DRAW_W * cb + at] & m)) ? 2 : 0;
        }
    return 1;
}
