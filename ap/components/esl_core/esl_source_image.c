#include "esl_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esl_image.h"
#include "esl_render.h"

/* Data source type "image": a PNG the AP fetches from a URL every so often and shows on
 * the source's tags as their picture, as if it had been sent with PUT /api/tags/{mac}/image
 * (a chart, a camera still, a dashboard made by another program). The PNG must have the
 * screen size of each tag (tags of another size get an error); dither as in that API.
 * The answer is kept and sent again as If-Modified-Since, so a picture that did not change
 * costs the server a 304 and the tags nothing: the same picture is not stored or sent again. */

/* The values it gives: the picture's size in pixels, for GET /api/sources */
static const char *const image_keys[] = {"width", "height", NULL};
static const char *const image_fields[] = {NULL};
static const char *const image_cfg_keys[] = {"url", "dither", NULL};

static bool image_parse(const char *js, const esl_jtok_t *tok, int obj, esl_source_cfg_t *c, char *err, size_t errlen)
{
    int t = esl_json_obj_get(js, tok, obj, "url");
    int n = t >= 0 && tok[t].type == ESL_JSON_STRING
                ? esl_json_str_decode(js, &tok[t], c->image.url, sizeof(c->image.url) - 1)
                : -1;
    if (n >= 0)
        c->image.url[n] = '\0';
    if (n < 0 || !esl_source_url_ok(c->image.url)) {
        snprintf(err, errlen, "url: an http:// or https:// URL of a PNG, up to %d bytes, without spaces or a login",
                 ESL_SOURCE_URL_MAX - 1);
        return false;
    }
    c->image.dither = false;
    t = esl_json_obj_get(js, tok, obj, "dither");
    if (t >= 0) {
        if (tok[t].type != ESL_JSON_TRUE && tok[t].type != ESL_JSON_FALSE) {
            snprintf(err, errlen, "dither: true or false");
            return false;
        }
        c->image.dither = tok[t].type == ESL_JSON_TRUE;
    }
    return true;
}

static void image_write(const esl_source_cfg_t *c, esl_sb_t *out)
{
    esl_sb_puts(out, ",\"url\":");
    esl_sb_json_str(out, c->image.url);
    esl_sb_printf(out, ",\"dither\":%s", c->image.dither ? "true" : "false");
}

static size_t image_url(const esl_source_cfg_t *c, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "%s", c->image.url);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The PNG's size from its header; the picture itself is read for each tag (esl_source_apply) */
static bool image_values(const esl_source_cfg_t *c, const char *body, size_t len, uint32_t now,
                         esl_utc_offset_fn off, esl_values_t *v, uint32_t *data_at, char *err, size_t errlen)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    const uint8_t *p = (const uint8_t *)body;
    (void)c, (void)now, (void)off, (void)data_at;
    if (len < 24 || memcmp(p, sig, 8) != 0 || memcmp(p + 12, "IHDR", 4) != 0) {
        snprintf(err, errlen, "the answer is not a PNG file");
        return false;
    }
    char b[12];
    v->n = 0;
    snprintf(b, sizeof(b), "%lu", (unsigned long)be32(p + 16));
    esl_values_set(v, "width", b);
    snprintf(b, sizeof(b), "%lu", (unsigned long)be32(p + 20));
    esl_values_set(v, "height", b);
    return true;
}

typedef struct {
    const uint8_t *p;
    size_t left;
} mem_t;

static bool mem_read(void *ctx, uint8_t *buf, size_t n)
{
    mem_t *m = ctx;
    if (n > m->left)
        return false;
    memcpy(buf, m->p, n);
    m->p += n;
    m->left -= n;
    return true;
}

/* The picture for a tag of model from the PNG, in img (esl_render_size(model) bytes, in
   esl_render's format); false with a message */
bool esl_source_image_decode(const esl_source_cfg_t *c, const char *body, size_t len, uint8_t model, uint8_t *img,
                             char *err, size_t errlen)
{
    mem_t m = {(const uint8_t *)body, len};
    esl_pic_err_t e = esl_image_picture(model, c->image.dither, len, mem_read, &m, img);
    if (!e)
        return true;
    uint16_t w = 0, h = 0;
    bool red;
    esl_render_model(model, &w, &h, &red);
    if (e == ESL_PIC_ERR_SIZE && len >= 24)
        snprintf(err, errlen, "the picture is %lux%lu pixels, the screen of model %s is %ux%u",
                 (unsigned long)be32((const uint8_t *)body + 16), (unsigned long)be32((const uint8_t *)body + 20),
                 esl_model_name(model), (unsigned)w, (unsigned)h);
    else
        snprintf(err, errlen, "%s",
                 e == ESL_PIC_ERR_NOMEM         ? "no memory to read the picture"
                 : e == ESL_PIC_ERR_UNSUPPORTED ? "this kind of PNG is not taken (interlaced, or an unknown colour type or chunk)"
                                                : "a broken PNG file");
    return false;
}

const esl_source_def_t esl_source_image = {
    .name = "image",
    .type = ESL_SOURCE_IMAGE,
    .about = "a PNG from a URL, as the tags' picture",
    .keys = image_keys,
    .fields = image_fields,
    .cfg_keys = image_cfg_keys,
    .every_min = 5,
    .picture = true,
    .own_pace = true,
    .parse = image_parse,
    .write = image_write,
    .url = image_url,
    .values = image_values,
};
