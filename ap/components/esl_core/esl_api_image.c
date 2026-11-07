/* /api/tags/{mac}/image: a tag's picture, read back or sent as a 1-bit image. */
#include "esl_api_int.h"

/* Base64 (RFC 4648, with padding) of n bytes, as a JSON string's contents. */
static void sb_base64(esl_sb_t *out, const uint8_t *p, size_t n)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n)
            v |= p[i + 2];
        esl_sb_putc(out, abc[v >> 18]);
        esl_sb_putc(out, abc[(v >> 12) & 63]);
        esl_sb_putc(out, i + 1 < n ? abc[(v >> 6) & 63] : '=');
        esl_sb_putc(out, i + 2 < n ? abc[v & 63] : '=');
    }
}

/* GET /api/tags/{mac}/image: the screen the AP draws for the tag's content, the bytes a
   tag that shows images gets (docs/protocol.md, "Image transfer"), for the dashboard.
   Drawn on each request and not kept (a picture is read back); also for a tag that
   cannot show it (no SPI flash). */
static int tag_image(const esl_api_t *api, uint64_t mac, esl_sb_t *out)
{
    esl_tag_t t;
    uint16_t w, h;
    bool red;
    if (!esl_store_get(api->store, mac, &t))
        return api_error(out, 404, "unknown tag");
    if (!t.model || !esl_render_model(t.model, &w, &h, &red))
        return api_error(out, 404, "the tag's model is not known yet");
    esl_content_t c;
    if (!t.has_content || !esl_store_get_content(api->store, mac, &c))
        return api_error(out, 404, "the tag has no content");
    if (t.content_err == ESL_CERR_NOT_IN_FILE)
        return api_error(out, 409, esl_content_err_text(t.content_err));
    size_t raw = esl_render_size(t.model);
    size_t cap = c.layout == ESL_LAYOUT_IMAGE ? ESL_IMAGE_STORED_MAX(raw) + ESL_IMG_RLE_MARGIN(raw) : raw;
    uint8_t *img = malloc(cap);
    if (!img)
        return api_error(out, 503, "no memory to draw the image");
    size_t n = esl_image_screen(api->store, api->layouts, mac, t.model, &c, img, cap);
    if (!n) {
        free(img);
        return api_error(out, 409, "the content cannot be drawn");
    }
    uint32_t id = esl_crc32(img, n);
    char m[17];
    esl_mac_format(mac, m);
    esl_sb_printf(out,
                  "{\"mac\":\"%s\",\"model\":\"%s\",\"width\":%u,\"height\":%u,\"red\":%s,\"content_id\":%" PRIu32
                  ",\"image_id\":%" PRIu32 ",\"data\":\"",
                  m, esl_model_name(t.model), (unsigned)w, (unsigned)h, red ? "true" : "false", t.content_id,
                  id ? id : 1);
    sb_base64(out, img, n);
    esl_sb_puts(out, "\"}");
    free(img);
    return 200;
}

/* PUT /api/tags/{mac}/image: a picture as the tag's content (esl_image.h). The body is
   pulled through read, len bytes, and need not be in RAM at once; what is not read of it
   is the caller's to drop. */
typedef struct {
    bool (*read)(void *ctx, uint8_t *buf, size_t n);
    void *ctx;
    size_t left;
} body_t;

static bool body_read(void *ctx, uint8_t *buf, size_t n)
{
    body_t *b = ctx;
    if (n > b->left)
        return false;
    b->left -= n;
    return b->read(b->ctx, buf, n);
}

/* hold (may be NULL) is let go while the body is read and decoded, which can take
   as long as the client takes to send it, and taken again to store the picture. */
static int put_image(const esl_api_t *api, uint64_t mac, const char *uri, size_t len,
                     bool (*read)(void *ctx, uint8_t *buf, size_t n), void *ctx, void (*hold)(bool take),
                     esl_sb_t *out)
{
    esl_tag_t t;
    if (!esl_store_takes_images(api->store))
        return api_error(out, 501, "this AP keeps no pictures");
    if (!esl_store_get(api->store, mac, &t))
        return api_error(out, 404, "unknown tag");
    size_t raw = t.model ? esl_render_size(t.model) : 0;
    if (!raw)
        return api_error(out, 409, "the tag's model is not known yet: it has not polled");
    size_t dl;
    const char *dv = api_query_str(uri, "dither", &dl);
    bool dither = dv && ((dl == 4 && memcmp(dv, "true", 4) == 0) || (dl == 1 && dv[0] == '1'));
    if (dv && !dither && !(dl == 5 && memcmp(dv, "false", 5) == 0) && !(dl == 1 && dv[0] == '0'))
        return api_error(out, 400, "dither must be true or false");
    if (!len)
        return api_error(out, 400, "missing body: a PNG of the tag's screen size");
    if (len > ESL_API_MAX_IMAGE_BODY)
        return api_error(out, 413, "body too large");

    uint8_t *img = malloc(raw + ESL_IMAGE_STORED_MAX(raw));
    if (!img)
        return api_error(out, 503, "no memory for the picture");
    uint8_t *stored = img + raw;
    body_t body = {read, ctx, len};
    if (hold)
        hold(false);
    esl_pic_err_t err = esl_image_picture(t.model, dither, len, body_read, &body, img);
    if (hold)
        hold(true);
    int status = 200;
    if (err) {
        uint16_t w = 0, h = 0;
        bool red;
        char msg[MSG_MAX];
        esl_render_model(t.model, &w, &h, &red);
        switch (err) {
        case ESL_PIC_ERR_READ:
            status = api_error(out, 400, "the body ended early");
            break;
        case ESL_PIC_ERR_FORMAT:
            snprintf(msg, sizeof(msg), "not a PNG file (or the %u bytes of a raw image)", (unsigned)raw);
            status = api_error(out, 415, msg);
            break;
        case ESL_PIC_ERR_BAD:
            status = api_error(out, 400, "a broken PNG file");
            break;
        case ESL_PIC_ERR_UNSUPPORTED:
            status = api_error(out, 415, "this kind of PNG is not taken (interlaced, or a colour type, bit depth or critical chunk PNG does not have)");
            break;
        case ESL_PIC_ERR_SIZE:
            snprintf(msg, sizeof(msg), "the picture must be %ux%u pixels, the tag's screen", (unsigned)w, (unsigned)h);
            status = api_error(out, 422, msg);
            break;
        case ESL_PIC_ERR_BOTH:
            status = api_error(out, 400, "a raw image with a pixel set in both the black and the red plane");
            break;
        default:
            status = api_error(out, 503, "no memory for the picture");
            break;
        }
        free(img);
        return status;
    }
    uint8_t model = t.model;
    if (!esl_store_get(api->store, mac, &t) || t.model != model) {
        free(img);
        return api_error(out, 409, "the tag was deleted or changed while the picture came in");
    }
    size_t n = esl_image_pack(mac, t.model, img, stored);
    esl_content_t c;
    esl_set_result_t r;
    esl_content_init_image(&c, t.model, esl_crc32(stored, n));
    api_leave_source(api, mac); /* first: a source's run cannot overwrite the picture after this */
    esl_store_err_t serr = esl_store_set_image(api->store, mac, stored, n, &c, &r);
    free(img);
    if (serr)
        return api_error(out, 507, serr == ESL_STORE_NOT_SAVED ? "no room to save the picture: NVS is full, or nearly (some is kept for content)"
                                                               : api_store_err_text(serr));
    api_write_set_result(out, mac, &r);
    return status;
}

static bool mem_read(void *ctx, uint8_t *buf, size_t n)
{
    const uint8_t **p = ctx;
    memcpy(buf, *p, n);
    *p += n;
    return true;
}

/* /api/tags/{mac}/image[/] with or without a query: the MAC */
static bool image_uri(const char *uri, uint64_t *mac)
{
    static const char img[] = "/image";
    const size_t base = sizeof(PATH_TAGS) - 1, il = sizeof(img) - 1;
    size_t plen = strcspn(uri, "?");
    if (plen > 0 && uri[plen - 1] == '/')
        plen--;
    return plen == base + 17 + il && memcmp(uri, PATH_TAGS "/", base + 1) == 0 &&
           memcmp(uri + base + 17, img, il) == 0 && esl_mac_parse(uri + base + 1, 16, mac);
}

bool esl_api_image_put_uri(const char *method, const char *uri)
{
    uint64_t mac;
    return strcmp(method, "PUT") == 0 && image_uri(uri, &mac);
}

int esl_api_image_put(const esl_api_t *api, const char *uri, const char *auth, size_t len,
                      bool (*read)(void *ctx, uint8_t *buf, size_t n), void *ctx, void (*hold)(bool take),
                      uint64_t now_ms, esl_sb_t *out)
{
    unsigned wait_s;
    uint64_t mac;
    api_sb_reset(out);
    int auth_status = esl_api_auth_check(api, auth, now_ms, &wait_s);
    if (auth_status == 429) {
        char msg[64];
        snprintf(msg, sizeof(msg), "too many wrong tokens: locked for %u s", wait_s);
        return api_error(out, 429, msg);
    }
    if (auth_status)
        return api_error(out, 401, "unauthorized");
    if (!image_uri(uri, &mac))
        return api_error(out, 400, "mac must be 16 hex digits");
    return put_image(api, mac, uri, len, read, ctx, hold, out);
}

int api_tag_image(const esl_api_t *api, uint64_t mac, const esl_api_req_t *rq, esl_sb_t *out)
{
    if (strcmp(rq->method, "GET") == 0)
        return tag_image(api, mac, out);
    if (strcmp(rq->method, "PUT") == 0) {
        const uint8_t *p = (const uint8_t *)rq->body;
        return put_image(api, mac, rq->uri, rq->body ? rq->body_len : 0, mem_read, &p, NULL, out);
    }
    return api_error(out, 405, "method not allowed");
}
