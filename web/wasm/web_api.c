/*
 * web_api.c - the label renderer for the web page (web/), built to WebAssembly with
 * Emscripten by web/build.py, once per tag model (-DMODEL=29 or 154).
 *
 * It links the same code the previews use: the AP's layout compiler
 * (ap/components/esl_core/esl_layout_compile.c), the tag's file check (layout_file_install)
 * and the tag's drawing (layout.c, draw.c, the fonts). What the page shows is therefore
 * what a tag of that model draws, and a layout file the page accepts is one the AP accepts.
 *
 *   web_load_layouts(json, len)  compiles a layout file (docs/protokoll.md part 3) for this
 *                                model and installs it like a finished download. 1 = ok,
 *                                0 = rejected, web_error() says why (the AP's 422 message)
 *   web_clear_layouts()          back to only the built-in layouts
 *   web_render(layout, flags, n, f0, f1, f2, f3)
 *                                draws CONTENT with fields in UTF-8 (converted to Latin-1 as
 *                                the AP does) into the frame buffer. layout 1 = price,
 *                                2 = text, 16 + n = layout n of the file. 1 = ok, 0 = error
 *   web_fb()                     DRAW_H rows of DRAW_W bytes, 0 = white, 1 = black
 */
#include <stdio.h>
#include <string.h>
#include <emscripten/emscripten.h>
#include "fake.h"
#include "helpers.h"
#include "layout.h"
#include "esl_layout.h"

static char err[ESL_LAYOUT_ERR_MAX];
static esl_layout_bin_t bin;

EMSCRIPTEN_KEEPALIVE int web_width(void) { return DRAW_W; }
EMSCRIPTEN_KEEPALIVE int web_height(void) { return DRAW_H; }
EMSCRIPTEN_KEEPALIVE int web_model_code(void) { return MODEL_CODE; }
EMSCRIPTEN_KEEPALIVE const char *web_error(void) { return err; }
EMSCRIPTEN_KEEPALIVE uint8_t *web_fb(void) { return &fake_fb[0][0]; }
EMSCRIPTEN_KEEPALIVE int web_max_text(void) { return ESL_MAX_TEXT_BYTES; }

/* The limits the compiler checks, so the page can show how close a layout is to them:
 * 0 lines of text per screen, 1 rectangles per screen, 2 bytes in the file, 3 border of an
 * inverted field, 4 lines per text element, 5..8 height of the fonts small, medium, mid, large,
 * 9 barcodes per screen, 10 lowest barcode, 11 widest barcode module, 12 bytes per fixed text. */
EMSCRIPTEN_KEEPALIVE int web_limit(int what)
{
    static const int v[] = { ESL_DRAW_MAX_RUNS, ESL_DRAW_MAX_RECTS, ESL_LBIN_MAX, ESL_INVERT_PAD,
                             ESL_LE_MAX_LINES, ESL_FONT_SMALL_H, ESL_FONT_MEDIUM_H, ESL_FONT_MID_H,
                             ESL_FONT_LARGE_H, ESL_DRAW_MAX_BARCODES, ESL_BC_MIN_H, ESL_BC_MODULE_MAX,
                             ESL_LBIN_TEXT_LEN_MAX };
    return what >= 0 && what < (int)(sizeof(v) / sizeof(v[0])) ? v[what] : 0;
}

/* The compiled file: what the tags download. Valid after web_load_layouts() returned 1. */
EMSCRIPTEN_KEEPALIVE const uint8_t *web_layout_bin(void) { return layout_file; }
EMSCRIPTEN_KEEPALIVE int web_layout_size(void) { return layout_file_ver ? layout_file_len : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t web_layout_version(void) { return layout_file_ver; }

EMSCRIPTEN_KEEPALIVE int web_load_layouts(const char *json, int len)
{
    uint32_t crc;

    err[0] = 0;
    layout_file_clear();
    if (len < 0 || len > ESL_LAYOUT_MAX_SIZE) {
        snprintf(err, sizeof(err), "the file is %d bytes, the AP takes at most %d", len, ESL_LAYOUT_MAX_SIZE);
        return 0;
    }
    if (!esl_layout_compile(json, (size_t)len, MODEL_CODE, &bin, err, sizeof(err)))
        return 0;
    if (bin.size > ESL_LBIN_MAX) {
        snprintf(err, sizeof(err), "compiled file is %u bytes, the tag takes at most %d", (unsigned)bin.size, ESL_LBIN_MAX);
        return 0;
    }
    memcpy(layout_file, bin.bin, bin.size);
    crc = proto_crc32(0, layout_file, bin.size);
    if (!layout_file_install(bin.size, crc ? crc : 1)) {
        snprintf(err, sizeof(err), "the AP accepted the file, but the tag's check rejects it");
        return 0;
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE void web_clear_layouts(void)
{
    layout_file_clear();
}

/* Latin-1 bytes of one UTF-8 field as the AP sends it (255 if it is too long to count). */
EMSCRIPTEN_KEEPALIVE int web_text_bytes(const char *utf8)
{
    static uint8_t tmp[256 * 3];
    if (strlen(utf8) > 255)
        return 255;
    return to_latin1(tmp, utf8);
}

EMSCRIPTEN_KEEPALIVE int web_render(int layout, int flags, int n, const char *f0, const char *f1,
                                    const char *f2, const char *f3)
{
    static draw_scene_t sc;
    static uint8_t buf[ESL_MAX_PAYLOAD + 300 * ESL_MAX_FIELDS];
    const char *const f[ESL_MAX_FIELDS] = { f0, f1, f2, f3 };
    proto_reply_t r;
    int i, total = 0;
    uint8_t len;

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
    if (total > ESL_MAX_TEXT_BYTES) {
        snprintf(err, sizeof(err), "%d bytes of text, the AP takes at most %d", total, ESL_MAX_TEXT_BYTES);
        return 0;
    }
    len = make_content(buf, 0, 1, (uint8_t)layout, (uint8_t)flags, n, f);
    if (len > ESL_MAX_PAYLOAD || proto_parse_reply(buf, len, &r) != PROTO_OK) {
        snprintf(err, sizeof(err), "invalid CONTENT packet (%d bytes)", len);
        return 0;
    }
    layout_build(&sc, &r, buf);
    fake_render(&sc);
    return 1;
}
