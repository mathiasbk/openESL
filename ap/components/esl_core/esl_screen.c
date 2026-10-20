#include "esl_screen.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esl_text.h"
#include "font.h" /* tag/src: the tag's own bitmap fonts, font_small from tag/fonts */

static void put_ssid(char *out, size_t cap, const char *prefix, const char *ssid)
{
    size_t n = strlen(prefix);
    memcpy(out, prefix, n);
    n += esl_utf8_to_latin1(ssid, strlen(ssid), (uint8_t *)out + n, cap - 1 - n, NULL);
    out[n] = 0;
}

static void put_uptime(char *out, size_t cap, uint64_t s, unsigned seen)
{
    int n;
    if (s < 120)
        n = snprintf(out, cap, "up %u s", (unsigned)s);
    else if (s < 7200)
        n = snprintf(out, cap, "up %u min", (unsigned)(s / 60));
    else if (s < 2 * 86400)
        n = snprintf(out, cap, "up %u h %u min", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
    else
        n = snprintf(out, cap, "up %" PRIu64 " d %u h", s / 86400, (unsigned)(s / 3600 % 24));
    if (n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - (size_t)n, ", %u seen", seen);
}

void esl_screen_make(const esl_screen_info_t *in, esl_screen_t *out)
{
    memset(out, 0, sizeof(*out));
    char (*t)[ESL_SCREEN_LINE_MAX] = out->text;
    const esl_wifi_status_t *w = &in->wifi;

    /* '\t' puts the rest at the right edge */
    snprintf(t[0], sizeof(t[0]), "OpenESL %s\tch %u", in->fw ? in->fw : "", (unsigned)in->channel);
    out->color[0] = ESL_SCREEN_TITLE;

    if (!in->tags)
        snprintf(t[1], sizeof(t[1]), "No tags yet");
    else if (in->pending)
        snprintf(t[1], sizeof(t[1]), "%u tag%s, %u pending", in->tags, in->tags == 1 ? "" : "s", in->pending);
    else
        snprintf(t[1], sizeof(t[1]), "%u tag%s, all shown", in->tags, in->tags == 1 ? "" : "s");
    out->color[1] = in->pending ? ESL_SCREEN_WARN : ESL_SCREEN_TEXT;
    if (in->table_full || (in->tags >= in->tags_max && in->tags_max)) {
        snprintf(t[1], sizeof(t[1]), "%u tags: table full", in->tags);
        out->color[1] = ESL_SCREEN_BAD;
    }

    out->color[3] = ESL_SCREEN_DIM;
    put_uptime(t[3], sizeof(t[3]), in->uptime_s, in->seen);
    if (w->mode == ESL_WIFI_OFF) {
        snprintf(t[2], sizeof(t[2]), in->serial_api ? "WiFi off, API on USB" : "WiFi off");
        out->color[2] = ESL_SCREEN_BAD;
    } else if (w->connected) {
        snprintf(t[2], sizeof(t[2]), "%s", w->ip);
        out->color[2] = ESL_SCREEN_OK;
    } else if (w->setup_ssid[0]) {
        put_ssid(t[2], sizeof(t[2]), "Setup: ", w->setup_ssid);
        snprintf(t[3], sizeof(t[3]), "%s/setup", w->setup_ip[0] ? w->setup_ip : "192.168.4.1");
        out->color[2] = out->color[3] = ESL_SCREEN_WARN;
    } else {
        put_ssid(t[2], sizeof(t[2]), "Joining ", w->ssid);
        out->color[2] = ESL_SCREEN_WARN;
    }
}

/* ---- rendering --------------------------------------------------------------- */

static uint8_t glyph(uint8_t ch)
{
    const font_t *f = &font_small;
    if (ch >= f->first && ch <= f->last && f->widths[ch - f->first])
        return ch;
    return '?';
}

static unsigned text_width(const uint8_t *s, size_t n)
{
    unsigned w = 0;
    for (size_t i = 0; i < n; i++)
        w += font_small.widths[glyph(s[i]) - font_small.first];
    return w;
}

/* Draws s[0..n) from x, cutting at x_end. Returns the x after the last glyph. */
static unsigned draw_text(const uint8_t *s, size_t n, unsigned x, unsigned x_end, unsigned y, uint8_t color,
                          uint8_t *pix, unsigned w, unsigned h)
{
    const font_t *f = &font_small;
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = glyph(s[i]);
        unsigned gw = f->widths[ch - f->first];
        if (x + gw > x_end)
            break;
        const uint8_t *bm = f->bitmap + f->offsets[ch - f->first];
        for (unsigned c = 0; c < gw; c++, bm += f->bytes_per_col) {
            for (unsigned r = 0; r < f->height && y + r < h; r++)
                if (bm[r / 8] & (0x80 >> (r % 8)))
                    pix[(y + r) * w + x + c] = color;
        }
        x += gw;
    }
    return x;
}

void esl_screen_render(const esl_screen_t *s, uint8_t *pix, unsigned w, unsigned h)
{
    const unsigned margin = 2;
    memset(pix, ESL_SCREEN_BG, (size_t)w * h);
    const unsigned fh = font_small.height;
    unsigned pitch = h / ESL_SCREEN_LINES;
    if (pitch > fh + 4)
        pitch = fh + 4;
    unsigned top = pitch > fh ? (h - pitch * ESL_SCREEN_LINES + (pitch - fh)) / 2 : 0;
    for (unsigned i = 0; i < ESL_SCREEN_LINES; i++) {
        const uint8_t *line = (const uint8_t *)s->text[i];
        size_t len = strlen(s->text[i]);
        const uint8_t *tab = memchr(line, '\t', len);
        size_t left = tab ? (size_t)(tab - line) : len;
        unsigned y = top + i * pitch, end = w - margin;
        if (tab) {
            size_t rn = len - left - 1;
            unsigned rw = text_width(tab + 1, rn);
            if (rw + margin < end) {
                draw_text(tab + 1, rn, end - rw, end, y, s->color[i], pix, w, h);
                end -= rw + font_small.widths[' ' - font_small.first];
            }
        }
        draw_text(line, left, margin, end, y, s->color[i], pix, w, h);
    }
}
