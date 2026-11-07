/* PUT /api/tags/{mac}/draw: a one-off design for one tag, without a layout file.
 *
 * The body holds one layout as a layout file has it (elements, field names), its icons and
 * the fields' text. The AP compiles it for the tag's model with the same compiler and
 * limits as a layout file (esl_layout_compile), draws it at once (esl_render) and keeps
 * the screen as the tag's picture, as PUT /api/tags/{mac}/image does. So nothing new is
 * stored and an older AP's storage is not touched: the tag shows a picture ("image" in
 * GET /api/tags), and date fields are filled in when it is drawn, not every day. */
#include "esl_api_int.h"

/* The name the layout gets when the body gives none; never seen by a client except in
   error messages */
#define DRAW_NAME "draw"

/* {"format":"esl-layouts","version":1,"model":"<m>","layouts":[ ... ]} around the layout */
static const char file_head[] = "{\"format\":\"esl-layouts\",\"version\":1,\"model\":\"";
static const char file_layouts[] = "\",\"layouts\":[";
static const char name_key[] = "\"name\":\"" DRAW_NAME "\",";

typedef struct {
    const json_t *j;
    int names; /* the layout's "fields" token (a list of names), -1 = none */
} names_t;

/* A field name's index in the layout's "fields", or -1 with msg set */
static int name_index(void *ctx, const char *key, size_t klen, char *msg)
{
    const names_t *nm = ctx;
    const json_t *j = nm->j;
    if (nm->names >= 0 && j->tok[nm->names].type == ESL_JSON_ARRAY)
        for (unsigned k = 0, i = (unsigned)nm->names + 1; k < j->tok[nm->names].size; k++, i = j->tok[i].next) {
            const esl_jtok_t *t = &j->tok[i];
            if (t->type == ESL_JSON_STRING && (size_t)(t->end - t->start) == klen &&
                memcmp(&j->js[t->start], key, klen) == 0)
                return (int)k;
        }
    snprintf(msg, MSG_MAX, "the layout has no field \"%.*s\"", (int)(klen > 32 ? 32 : klen), key);
    return -1;
}

/* Builds the layout file for model from the body's layout and icons tokens into a new
   string (malloc), its length in *len. NULL when out of memory. */
static char *make_file(const json_t *j, int lay, int icons, uint8_t model, size_t *len)
{
    const esl_jtok_t *l = &j->tok[lay];
    const char *mname = esl_model_name(model);
    bool named = esl_json_obj_get(j->js, j->tok, lay, "name") >= 0;
    size_t ll = (size_t)(l->end - l->start), il = icons >= 0 ? (size_t)(j->tok[icons].end - j->tok[icons].start) : 0;
    size_t n = sizeof(file_head) - 1 + strlen(mname) + sizeof(file_layouts) - 1 + ll + (named ? 0 : sizeof(name_key) - 1) +
               2 + (icons >= 0 ? sizeof(",\"icons\":") - 1 + il : 0) + 1;
    char *f = malloc(n + 1);
    if (!f)
        return NULL;
    size_t p = 0;
#define PUT(s, k) (memcpy(f + p, (s), (k)), p += (k))
    PUT(file_head, sizeof(file_head) - 1);
    PUT(mname, strlen(mname));
    PUT(file_layouts, sizeof(file_layouts) - 1);
    PUT(&j->js[l->start], 1); /* the layout's "{" */
    if (!named)
        PUT(name_key, sizeof(name_key) - 1);
    /* the rest of the object; a layout of only "{}" gets no trailing comma problem, since
       the compiler then fails it for its missing fields and elements anyway */
    if (!named && ll == 2) {
        p--; /* drop the comma after the name */
        PUT("}", 1);
    } else
        PUT(&j->js[l->start + 1], ll - 1);
    PUT("]", 1);
    if (icons >= 0) {
        PUT(",\"icons\":", sizeof(",\"icons\":") - 1);
        PUT(&j->js[j->tok[icons].start], il);
    }
    PUT("}", 1);
#undef PUT
    f[p] = '\0';
    *len = p;
    return f;
}

static int draw(const esl_api_t *api, uint64_t mac, const char *body, size_t len, esl_sb_t *out)
{
    static const char *const keys[] = {"layout", "icons", "fields", NULL};
    esl_tag_t t;
    if (!esl_store_takes_images(api->store))
        return api_error(out, 501, "this AP keeps no pictures, so it cannot keep a drawing either");
    if (!esl_store_get(api->store, mac, &t))
        return api_error(out, 404, "unknown tag");
    uint8_t model = t.model;
    size_t raw = model ? esl_render_size(model) : 0;
    if (!raw)
        return api_error(out, 409, "the tag's model is not known yet: it has not polled");
    if (len > (size_t)esl_limits.json_bytes + 2048)
        return api_error(out, 413, "body too large: the layout and its icons take at most as much as a layout file");

    esl_jtok_t *tok = NULL;
    int status = api_parse_json(body, len, &tok, out);
    if (status) {
        free(tok);
        return status;
    }
    json_t j = {body, tok};
    char msg[MSG_MAX];
    esl_content_t c;
    char *file = NULL;
    size_t flen = 0;
    if (tok[0].type != ESL_JSON_OBJECT) {
        free(tok);
        return api_error(out, 400, "the body must be a JSON object");
    }
    for (unsigned p = 0, i = 1; p < tok[0].size; p++, i = tok[i + 1].next) {
        bool known = false;
        for (unsigned k = 0; keys[k]; k++)
            known |= esl_json_str_eq(body, &tok[i], keys[k]);
        if (!known) {
            snprintf(msg, sizeof(msg), "unknown key \"%.*s\" (it takes layout, icons and fields)",
                     (int)(tok[i].end - tok[i].start > 32 ? 32 : tok[i].end - tok[i].start), &body[tok[i].start]);
            free(tok);
            return api_error(out, 422, msg);
        }
    }
    int lay = esl_json_obj_get(body, tok, 0, "layout");
    int icons = esl_json_obj_get(body, tok, 0, "icons");
    int fields = esl_json_obj_get(body, tok, 0, "fields");
    if (lay < 0 || tok[lay].type != ESL_JSON_OBJECT) {
        free(tok);
        return api_error(out, 422, "layout must be one layout as in a layout file: {\"fields\": [...], \"elements\": [...]}");
    }
    if (icons >= 0 && tok[icons].type != ESL_JSON_OBJECT) {
        free(tok);
        return api_error(out, 422, "icons must be an object, as in a layout file");
    }
    int nm = esl_json_obj_get(body, tok, lay, "name");
    if (nm >= 0) {
        if (tok[nm].type != ESL_JSON_STRING ||
            !esl_layout_name_valid(&body[tok[nm].start], (size_t)(tok[nm].end - tok[nm].start))) {
            free(tok);
            return api_error(out, 422, "the layout's name must be 1-31 of a-z 0-9 . _ - (starting with a letter or digit)");
        }
        esl_content_init_named(&c, &body[tok[nm].start], (size_t)(tok[nm].end - tok[nm].start), 0);
    } else
        esl_content_init_named(&c, DRAW_NAME, sizeof(DRAW_NAME) - 1, 0);

    /* The text first, while the body's tokens are here; then they go before the compiler
       takes its own */
    names_t names = {&j, esl_json_obj_get(body, tok, lay, "fields")};
    if (fields >= 0)
        status = api_content_fields(&j, fields, c.name, ESL_MAX_FIELDS, name_index, &names, &c, msg);
    if (!status) {
        file = make_file(&j, lay, icons, model, &flen);
        if (!file) {
            snprintf(msg, sizeof(msg), "out of memory");
            status = 503;
        }
    }
    free(tok);
    if (status) {
        free(file);
        return api_error(out, status, msg);
    }

    esl_layout_bin_t *bin = malloc(sizeof(*bin));
    if (!bin) {
        free(file);
        return api_error(out, 503, "out of memory");
    }
    char err[ESL_LAYOUT_ERR_MAX];
    bool ok = esl_layout_compile(file, flen, model, bin, err, sizeof(err));
    free(file);
    if (!ok) {
        free(bin);
        return api_error(out, 422, err);
    }
    if (c.nfields > bin->nfields[0]) {
        snprintf(msg, sizeof(msg), "the layout has %u fields, but %u are given", (unsigned)bin->nfields[0],
                 (unsigned)c.nfields);
        free(bin);
        return api_error(out, 422, msg);
    }
    ok = esl_layout_check_fields(bin, 0, model, &c, msg, sizeof(msg));
    if (ok && esl_render_number_cut(model, bin, &c)) {
        snprintf(msg, sizeof(msg), "a number does not fit in its box, not even in its smallest font");
        ok = false;
    }
    if (!ok) {
        free(bin);
        return api_error(out, 422, msg);
    }

    uint8_t *img = malloc(raw + ESL_IMAGE_STORED_MAX(raw));
    if (!img) {
        free(bin);
        return api_error(out, 503, "no memory to draw the picture");
    }
    size_t n = esl_layouts_render_bin(api->layouts, model, bin, &c, img, raw);
    free(bin);
    if (n != raw) {
        free(img);
        return api_error(out, 422, "the layout cannot be drawn");
    }
    status = api_store_picture(api, mac, model, img, out);
    free(img);
    return status;
}

int api_tag_draw(const esl_api_t *api, uint64_t mac, const esl_api_req_t *rq, esl_sb_t *out)
{
    if (strcmp(rq->method, "PUT") != 0)
        return api_error(out, 405, "method not allowed");
    return draw(api, mac, rq->body ? rq->body : "", rq->body ? rq->body_len : 0, out);
}
