/* /api/layouts and /api/fonts: layout files and font files (esl_font.h, #177). */
#include "esl_api_int.h"

_Static_assert(ESL_API_MAX_BODY > ESL_LAYOUT_MAX_SIZE && ESL_API_RESP_SIZE(0) > ESL_LAYOUT_MAX_SIZE,
               "a layout file must fit in a request and a response");

/* "same_as": the model whose file a model uses (#181), null for its own; "used_by": the
   models whose tags are drawn with the file a model uses, in code order */
static void write_layout_sharing(esl_sb_t *out, uint8_t model)
{
    uint8_t file = esl_model_layout_model(model);
    if (file != model)
        esl_sb_printf(out, "\"same_as\":\"%s\",\"used_by\":[", esl_model_name(file));
    else
        esl_sb_puts(out, "\"same_as\":null,\"used_by\":[");
    for (uint8_t m = 1, n = 0; m < ESL_MODEL_COUNT; m++)
        if (esl_model_layout_model(m) == file)
            esl_sb_printf(out, "%s\"%s\"", n++ ? "," : "", esl_model_name(m));
    esl_sb_puts(out, "],");
}

/* 409 for a model whose same_as names another: its file is that model's */
static int shared_file_error(esl_sb_t *out, uint8_t model, const char *what)
{
    char msg[MSG_MAX];
    const char *file = esl_model_name(esl_model_layout_model(model));
    snprintf(msg, sizeof(msg), "model %s uses the layout file of model %s (same_as): %s /api/layouts/%s",
             esl_model_name(model), file, what, file);
    return api_error(out, 409, msg);
}

static void write_layout_info(esl_sb_t *out, esl_layouts_t *ls, uint8_t model)
{
    uint32_t version;
    size_t len, bin;
    esl_sb_printf(out, "{\"model\":\"%s\",", esl_model_name(model));
    write_layout_sharing(out, model);
    if (esl_layouts_info(ls, model, &version, &len, &bin))
        esl_sb_printf(out, "\"version\":%" PRIu32 ",\"size\":%u,\"binary_size\":%u,\"layouts\":", version,
                      (unsigned)len, (unsigned)bin);
    else
        esl_sb_puts(out, "\"version\":null,\"size\":0,\"binary_size\":0,\"layouts\":");
    esl_layouts_write_names(ls, model, out);
    esl_sb_putc(out, '}');
}

/* The tags esl_store_check_layout finds, as {"mac","error"} objects: the first
   UNFIT_LIST_MAX of them (the total is given too). */
#define UNFIT_LIST_MAX 16
typedef struct {
    esl_sb_t *out;
    unsigned n;
} unfit_list_t;

static void unfit_tag(void *ctx, uint64_t mac, const char *msg)
{
    unfit_list_t *ul = ctx;
    char m[17];
    if (ul->n++ >= UNFIT_LIST_MAX)
        return;
    esl_mac_format(mac, m);
    esl_sb_printf(ul->out, "%s{\"mac\":\"%s\",\"error\":", ul->n > 1 ? "," : "", m);
    esl_sb_json_str(ul->out, msg);
    esl_sb_putc(ul->out, '}');
}

static int put_layout(const esl_api_t *api, uint8_t model, const char *body, size_t len, esl_sb_t *out)
{
    if (esl_model_layout_model(model) != model)
        return shared_file_error(out, model, "upload it with PUT");
    if (!body || len == 0)
        return api_error(out, 400, "missing JSON body");
    if (len > ESL_LAYOUT_MAX_SIZE)
        return api_error(out, 413, "layout file too large (max 8192 bytes)");

    /* Not JSON or not an object: 400, as for the other endpoints. Every token
     * takes at least two bytes of input except the last, so len/2 + 2 tokens
     * always suffice. */
    unsigned max_tok = (unsigned)(len / 2 + 2);
    esl_jtok_t *tok = malloc(sizeof(esl_jtok_t) * max_tok);
    if (!tok)
        return api_error(out, 500, "out of memory");
    int n = esl_json_parse(body, len, tok, max_tok);
    bool object = n > 0 && tok[0].type == ESL_JSON_OBJECT;
    free(tok);
    if (n < 0)
        return api_error(out, 400, "invalid JSON");
    if (!object)
        return api_error(out, 400, "layout file must be a JSON object");

    /* The file must work on this model's tags (requirement A13 in docs/requirements.md): 422 with where and why. */
    esl_layout_bin_t *bin = malloc(sizeof(*bin));
    char err[ESL_LAYOUT_ERR_MAX];
    if (!bin)
        return api_error(out, 500, "out of memory");
    if (!esl_layout_compile(body, len, model, bin, err, sizeof(err))) {
        free(bin);
        return api_error(out, 422, err);
    }
    bool changed;
    uint32_t version = esl_layouts_set(api->layouts, model, body, len, bin, &changed);
    free(bin);
    if (!version)
        return api_error(out, 500, "could not store layout");
    esl_sb_printf(out, "{\"model\":\"%s\",", esl_model_name(model));
    write_layout_sharing(out, model);
    esl_sb_printf(out, "\"version\":%" PRIu32 ",\"size\":%u,", version, (unsigned)len);
    size_t bsize;
    esl_layouts_info(api->layouts, model, NULL, NULL, &bsize);
    esl_sb_printf(out, "\"binary_size\":%u,\"layouts\":", (unsigned)bsize);
    esl_layouts_write_names(api->layouts, model, out);
    esl_sb_printf(out, ",\"changed\":%s,\"content_errors\":[", changed ? "true" : "false");
    /* Tags whose content no longer fits (#186): held back, as content_error in GET /api/tags */
    unfit_list_t ul = {out, 0};
    unsigned bad = esl_store_check_layout(api->store, api->layouts, model, unfit_tag, &ul);
    esl_sb_printf(out, "],\"content_errors_total\":%u}", bad);
    return 200;
}

static int handle_layouts(const esl_api_t *api, const char *method, const char *uri, size_t plen,
                          const char *body, size_t body_len, esl_sb_t *out)
{
    const size_t base = sizeof(PATH_LAYOUTS) - 1;
    esl_layouts_t *ls = api->layouts;

    if (plen == base || (plen == base + 1 && uri[base] == '/')) {
        if (strcmp(method, "GET") != 0)
            return api_error(out, 405, "method not allowed");
        esl_sb_printf(out, "{\"max_size\":%u,\"max_binary_size\":%u,", (unsigned)esl_limits.json_bytes,
                      (unsigned)esl_limits.file_bytes);
        /* The board's limits, named as in the web page's renderer.limits, so the designer can
         * show this AP's instead of the default board's */
        esl_sb_printf(out,
                      "\"board\":\"%s\",\"limits\":{\"runs\":%u,\"rects\":%u,\"lines\":%u,\"icons\":%u,"
                      "\"barcodes\":%u,\"qrs\":%u,\"fileBytes\":%u,\"jsonBytes\":%u,\"fields\":%u,"
                      "\"fixedTextBytes\":%u,\"textBytes\":%u,\"shapes\":%u,\"polygonPoints\":%u},\"layouts\":[",
                      ESL_BOARD, (unsigned)esl_limits.runs, (unsigned)esl_limits.rects, (unsigned)esl_limits.lines,
                      (unsigned)esl_limits.icons, (unsigned)ESL_DRAW_MAX_BARCODES, (unsigned)ESL_DRAW_MAX_QR,
                      (unsigned)esl_limits.file_bytes, (unsigned)esl_limits.json_bytes, (unsigned)ESL_MAX_FIELDS,
                      (unsigned)ESL_LBIN_TEXT_LEN_MAX, (unsigned)ESL_CONTENT_FIELD_SPACE,
                      (unsigned)esl_limits.shapes, (unsigned)ESL_POLY_MAX);
        for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
            if (m > 1)
                esl_sb_putc(out, ',');
            write_layout_info(out, ls, m);
        }
        esl_sb_puts(out, "]}");
        return 200;
    }
    if (uri[base] != '/')
        return api_error(out, 404, "not found");

    size_t ml = plen - base - 1;
    if (ml > 0 && uri[base + ml] == '/')
        ml--;
    uint8_t model = esl_model_parse(&uri[base + 1], ml);
    if (model == ESL_MODEL_UNKNOWN)
        return api_error(out, 404, "unknown model (use 2.9 or 1.54)");

    if (strcmp(method, "PUT") == 0)
        return put_layout(api, model, body, body_len, out);
    if (strcmp(method, "GET") == 0) {
        if (!esl_layouts_write(ls, model, out))
            return api_error(out, 404, "no layout for this model");
        return 200;
    }
    if (strcmp(method, "DELETE") == 0) {
        if (esl_model_layout_model(model) != model)
            return shared_file_error(out, model, "delete it with DELETE");
        if (!esl_layouts_delete(ls, model))
            return api_error(out, 404, "no layout for this model");
        esl_store_check_layout(api->store, ls, model, NULL, NULL); /* no file: nothing is unfit */
        esl_sb_printf(out, "{\"model\":\"%s\",\"deleted\":true}", esl_model_name(model));
        return 200;
    }
    return api_error(out, 405, "method not allowed");
}

static int put_font(const esl_api_t *api, const char *name, const char *body, size_t len, esl_sb_t *out)
{
    char err[ESL_LAYOUT_ERR_MAX + 64];
    if (!body || len == 0)
        return api_error(out, 400, "missing font file (JSON: Make a font in the dashboard, or tag/tools/gen_font.py --file)");
    if (len > ESL_API_MAX_BODY) {
        snprintf(err, sizeof(err), "font file too large (max %u bytes: fewer characters or a smaller height)",
                 (unsigned)ESL_API_MAX_BODY);
        return api_error(out, 413, err);
    }
    /* Not JSON or not an object: 400, as for the other endpoints (a file with more values
       than a font has is left to esl_font_from_json, which says so) */
    esl_jtok_t *tok = malloc(sizeof(*tok) * 320);
    if (!tok)
        return api_error(out, 500, "out of memory");
    int nt = esl_json_parse(body, len, tok, 320);
    bool object = nt > 0 && tok[0].type == ESL_JSON_OBJECT;
    free(tok);
    if (nt < 0 && nt != ESL_JSON_ERR_NOMEM)
        return api_error(out, 400, "invalid JSON");
    if (nt > 0 && !object)
        return api_error(out, 400, "a font file must be a JSON object");
    esl_ufont_t *f = esl_font_from_json(body, len, name, err, sizeof(err));
    if (!f)
        return api_error(out, 422, err);
    unsigned models;
    bool replaced;
    int status = esl_layouts_font_put(api->layouts, f, &models, &replaced, err, sizeof(err));
    if (status)
        return api_error(out, status, err);
    esl_sb_puts(out, "{\"font\":");
    int s = esl_font_slot_of(name, strlen(name));
    if (s >= 0)
        esl_font_write_info(esl_font_slot((unsigned)s), out, false);
    else
        esl_sb_puts(out, "null");
    esl_sb_printf(out, ",\"replaced\":%s,\"redrawn\":[", replaced ? "true" : "false");
    unsigned n = 0;
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++)
        if (models & (1u << (m - 1)))
            esl_sb_printf(out, "%s\"%s\"", n++ ? "," : "", esl_model_name(m));
    /* A file compiled again may no longer fit some tags' content (a number now too wide) */
    esl_sb_puts(out, "],\"content_errors\":[");
    unfit_list_t ul = {out, 0};
    unsigned bad = 0;
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++)
        if (models & (1u << (m - 1)))
            bad += esl_store_check_layout(api->store, api->layouts, m, unfit_tag, &ul);
    esl_sb_printf(out, "],\"content_errors_total\":%u}", bad);
    return 200;
}

static int handle_fonts(const esl_api_t *api, const char *method, const char *uri, size_t plen,
                        const char *body, size_t body_len, esl_sb_t *out)
{
    const size_t base = sizeof(PATH_FONTS) - 1;
    if (plen == base || (plen == base + 1 && uri[base] == '/')) {
        if (strcmp(method, "GET") != 0)
            return api_error(out, 405, "method not allowed");
        esl_sb_printf(out, "{\"max_fonts\":%u,\"max_bytes\":%u,\"bytes\":%u,\"builtin\":", (unsigned)ESL_FONT_USER_MAX,
                      (unsigned)ESL_FONT_MAX_BYTES, (unsigned)esl_font_bytes());
        esl_font_write_builtin(out);
        esl_sb_puts(out, ",\"fonts\":");
        esl_layouts_font_list(api->layouts, out);
        esl_sb_putc(out, '}');
        return 200;
    }
    size_t nl = plen - base - 1;
    if (nl > 0 && uri[base + nl] == '/')
        nl--;
    const char *np = &uri[base + 1];
    if (nl > ESL_FONT_NAME_MAX || !esl_font_name_ok(np, nl))
        return esl_font_find(np, nl) >= 0
                   ? api_error(out, strcmp(method, "GET") == 0 ? 404 : 403, "a built-in font is part of the firmware")
                   : api_error(out, 404, "not a font file name (1 to 15 of a-z, 0-9, _ and -)");
    char name[ESL_FONT_NAME_MAX + 1];
    memcpy(name, np, nl);
    name[nl] = '\0';

    if (strcmp(method, "GET") == 0) {
        if (esl_font_slot_of(name, nl) < 0) /* only the API changes the fonts */
            return api_error(out, 404, "no font file by that name");
        esl_sb_stream(out, 200); /* a font file can be bigger than the reply buffer */
        esl_layouts_font_write(api->layouts, name, nl, out);
        return 200;
    }
    if (strcmp(method, "PUT") == 0)
        return put_font(api, name, body, body_len, out);
    if (strcmp(method, "DELETE") == 0) {
        char err[96];
        int status = esl_layouts_font_delete(api->layouts, name, nl, err, sizeof(err));
        if (status)
            return api_error(out, status, err);
        esl_sb_printf(out, "{\"name\":\"%s\",\"deleted\":true}", name);
        return 200;
    }
    return api_error(out, 405, "method not allowed");
}

int api_route_layouts(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    return handle_layouts(api, rq->method, rq->uri, rq->plen, rq->body, rq->body_len, out);
}

int api_route_fonts(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    return handle_fonts(api, rq->method, rq->uri, rq->plen, rq->body, rq->body_len, out);
}
