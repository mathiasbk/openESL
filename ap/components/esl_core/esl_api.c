#include "esl_api.h"
#include "esl_text.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_TAGS "/api/tags"
#define PATH_LAYOUTS "/api/layouts"
#define PATH_CONFIG "/api/config"
#define PATH_WIFI "/api/wifi"

_Static_assert(ESL_MAX_TEXT == 92, "update the \"text too long\" message");
_Static_assert(ESL_CFG_POLL_MIN_S == 10 && ESL_CFG_POLL_MAX_S == 3600 && ESL_CFG_LOST_AP_MIN_S == 60,
               "update the config error messages");
_Static_assert(ESL_API_MAX_BODY > ESL_LAYOUT_MAX_SIZE && ESL_API_RESP_SIZE(0) > ESL_LAYOUT_MAX_SIZE,
               "a layout file must fit in a request and a response");

typedef struct {
    const char *js;
    const esl_jtok_t *tok;
} json_t;

static void sb_reset(esl_sb_t *sb)
{
    sb->len = 0;
    sb->overflow = sb->cap == 0;
    sb->streaming = false;
    sb->status = 0;
    sb->sent = 0;
    if (sb->cap)
        sb->buf[0] = '\0';
}

static int api_error_at(esl_sb_t *out, int status, const char *msg, int index)
{
    sb_reset(out);
    esl_sb_puts(out, "{\"error\":");
    esl_sb_json_str(out, msg);
    if (index >= 0)
        esl_sb_printf(out, ",\"index\":%d", index);
    esl_sb_putc(out, '}');
    return status;
}

static int api_error(esl_sb_t *out, int status, const char *msg)
{
    return api_error_at(out, status, msg, -1);
}

const char *esl_api_status_line(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 413: return "413 Payload Too Large";
    case 422: return "422 Unprocessable Entity";
    case 507: return "507 Insufficient Storage";
    default:  return "500 Internal Server Error";
    }
}

/* ---- auth ---------------------------------------------------------------- */

static bool auth_ok(const char *token, const char *auth)
{
    if (!token || !*token || !auth)
        return false;
    static const char prefix[] = "bearer ";
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) {
        char c = auth[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != prefix[i])
            return false;
    }
    const char *given = auth + sizeof(prefix) - 1;
    while (*given == ' ')
        given++;
    size_t tl = strlen(token), gl = strlen(given);
    /* Constant time with respect to the token contents. */
    unsigned diff = (unsigned)(tl ^ gl);
    for (size_t i = 0; i < gl; i++)
        diff |= (unsigned char)given[i] ^ (unsigned char)token[i % tl];
    return diff == 0;
}

/* ---- request parsing -------------------------------------------------------- */

static int parse_bool(const json_t *j, int obj, const char *key, bool *out)
{
    int v = esl_json_obj_get(j->js, j->tok, obj, key);
    *out = false;
    if (v < 0 || j->tok[v].type == ESL_JSON_NULL || j->tok[v].type == ESL_JSON_FALSE)
        return 0;
    if (j->tok[v].type == ESL_JSON_TRUE) {
        *out = true;
        return 0;
    }
    return -1;
}

#define MSG_MAX 192

/* A layout name from the layout files (krav A13, A14): it must be in the file
 * for the tag's model, or, while the model is not known yet, in some model's
 * file. *max_fields gets how many fields it takes. Returns 0 or 422 with msg. */
static int check_named(const esl_api_t *api, uint64_t mac, const char *name, size_t nl, unsigned *max_fields,
                       char *msg)
{
    char n[ESL_LAYOUT_NAME_MAX + 1];
    if (!api->layouts || !esl_layout_name_valid(name, nl)) {
        snprintf(msg, MSG_MAX, "layout must be \"price\", \"text\" or the name of a layout in a layout file");
        return 422;
    }
    memcpy(n, name, nl);
    n[nl] = '\0';

    esl_tag_t t;
    uint8_t nf = 0, model = ESL_MODEL_UNKNOWN;
    if (esl_store_get(api->store, mac, &t) && t.seen && esl_model_name(t.model))
        model = t.model;
    if (model != ESL_MODEL_UNKNOWN) {
        if (esl_layouts_find(api->layouts, model, name, nl, &nf) < 0) {
            snprintf(msg, MSG_MAX, "layout \"%s\" is not in the layout file for model %s", n, esl_model_name(model));
            return 422;
        }
        if (!t.layout_capable) {
            snprintf(msg, MSG_MAX, "the tag's firmware cannot download layout files: use \"price\" or \"text\"");
            return 422;
        }
        *max_fields = nf;
        return 0;
    }
    bool found = false;
    *max_fields = 0;
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
        if (esl_layouts_find(api->layouts, m, name, nl, &nf) >= 0) {
            found = true;
            if (nf > *max_fields)
                *max_fields = nf;
        }
    }
    if (!found) {
        snprintf(msg, MSG_MAX, "layout \"%s\" is not in any layout file", n);
        return 422;
    }
    return 0;
}

/* An optional whole number in [min, max]: missing or null gives 0. */
static int parse_uint(const json_t *j, int obj, const char *key, unsigned min, unsigned max, uint16_t *out)
{
    int v = esl_json_obj_get(j->js, j->tok, obj, key);
    *out = 0;
    if (v < 0 || j->tok[v].type == ESL_JSON_NULL)
        return 0;
    if (j->tok[v].type != ESL_JSON_NUMBER)
        return -1;
    unsigned long n = 0;
    for (int i = j->tok[v].start; i < j->tok[v].end; i++) {
        char c = j->js[i];
        if (c < '0' || c > '9' || n > max)
            return -1; /* negative, fraction or exponent, or far too large */
        n = n * 10 + (unsigned long)(c - '0');
    }
    if (n < min || n > max)
        return -1;
    *out = (uint16_t)n;
    return 0;
}

/* Fills cfg from {poll_s, max_poll_s, lost_ap_s}; missing or null = default. */
static int parse_cfg(const json_t *j, int obj, esl_tag_cfg_t *cfg, const char **msg)
{
    if (parse_uint(j, obj, "poll_s", ESL_CFG_POLL_MIN_S, ESL_CFG_POLL_MAX_S, &cfg->poll_s) < 0 ||
        parse_uint(j, obj, "max_poll_s", ESL_CFG_POLL_MIN_S, ESL_CFG_POLL_MAX_S, &cfg->max_poll_s) < 0) {
        *msg = "poll_s and max_poll_s must be null or a whole number of seconds from 10 to 3600";
        return 422;
    }
    if (parse_uint(j, obj, "lost_ap_s", ESL_CFG_LOST_AP_MIN_S, ESL_CFG_LOST_AP_NEVER, &cfg->lost_ap_s) < 0) {
        *msg = "lost_ap_s must be null or a whole number of seconds from 60 to 65535 (65535 = never)";
        return 422;
    }
    return 0;
}

/* Fills c from {layout, fields, price_highlight, frame} for the tag mac.
 * Returns 0 or an HTTP status with msg (MSG_MAX bytes) set. */
static int parse_content(const esl_api_t *api, const json_t *j, int obj, uint64_t mac, esl_content_t *c, char *msg)
{
    int lt = esl_json_obj_get(j->js, j->tok, obj, "layout");
    uint8_t layout = ESL_LAYOUT_NAMED;
    unsigned max_fields = ESL_MAX_FIELDS;
    if (lt < 0 || j->tok[lt].type != ESL_JSON_STRING) {
        snprintf(msg, MSG_MAX, "layout must be \"price\", \"text\" or the name of a layout in a layout file");
        return 422;
    }
    const char *name = &j->js[j->tok[lt].start];
    size_t nl = (size_t)(j->tok[lt].end - j->tok[lt].start);
    if (esl_json_str_eq(j->js, &j->tok[lt], "price"))
        layout = ESL_LAYOUT_PRICE;
    else if (esl_json_str_eq(j->js, &j->tok[lt], "text"))
        layout = ESL_LAYOUT_TEXT;
    else {
        int st = check_named(api, mac, name, nl, &max_fields, msg);
        if (st)
            return st;
    }

    bool price_highlight, frame;
    if (parse_bool(j, obj, "price_highlight", &price_highlight) < 0 || parse_bool(j, obj, "frame", &frame) < 0) {
        snprintf(msg, MSG_MAX, "price_highlight and frame must be true or false");
        return 422;
    }
    uint8_t flags = (uint8_t)((price_highlight ? ESL_CFLAG_PRICE_HIGHLIGHT : 0) | (frame ? ESL_CFLAG_FRAME : 0));
    if (layout == ESL_LAYOUT_NAMED)
        esl_content_init_named(c, name, nl, flags);
    else {
        esl_content_init(c, layout, flags);
        max_fields = (unsigned)esl_content_max_fields(layout);
    }

    int arr = esl_json_obj_get(j->js, j->tok, obj, "fields");
    if (arr < 0 || j->tok[arr].type != ESL_JSON_ARRAY) {
        snprintf(msg, MSG_MAX, "fields must be an array of strings");
        return 422;
    }
    if (j->tok[arr].size > max_fields) {
        if (layout == ESL_LAYOUT_NAMED)
            snprintf(msg, MSG_MAX, "layout %s takes at most %u fields", c->name, max_fields);
        else
            snprintf(msg, MSG_MAX, "layout %s takes at most %u fields", layout == ESL_LAYOUT_PRICE ? "price" : "text",
                     max_fields);
        return 422;
    }

    int i = arr + 1;
    for (unsigned k = 0; k < j->tok[arr].size; k++) {
        const esl_jtok_t *t = &j->tok[i];
        char utf8[ESL_MAX_TEXT * 4 + 4];
        uint8_t latin1[ESL_MAX_TEXT];
        size_t n = 0;
        bool overflow = false;

        if (t->type == ESL_JSON_STRING) {
            int ul = esl_json_str_decode(j->js, t, utf8, sizeof(utf8));
            if (ul == -1) {
                snprintf(msg, MSG_MAX, "invalid string escape");
                return 400;
            }
            if (ul >= 0)
                n = esl_utf8_to_latin1(utf8, (size_t)ul, latin1, sizeof(latin1), &overflow);
            else
                overflow = true;
        } else if (t->type != ESL_JSON_NULL) {
            snprintf(msg, MSG_MAX, "fields must be an array of strings");
            return 422;
        }
        if (overflow || esl_content_add_field(c, latin1, n) != ESL_CONTENT_OK) {
            snprintf(msg, MSG_MAX, "text too long (max 92 bytes after Latin-1 conversion)");
            return 422;
        }
        i = t->next;
    }
    esl_content_trim(c);

    /* A barcode field must be one the tag can draw: for the tag's model, or for every
       model with this layout while the model is not known */
    if (layout == ESL_LAYOUT_NAMED) {
        esl_tag_t t;
        uint8_t model = ESL_MODEL_UNKNOWN;
        if (esl_store_get(api->store, mac, &t) && t.seen && esl_model_name(t.model))
            model = t.model;
        for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++)
            if ((model == ESL_MODEL_UNKNOWN || m == model) &&
                !esl_layouts_check_barcode(api->layouts, m, c, msg, MSG_MAX))
                return 422;
    }
    return 0;
}

static int parse_json(const char *body, size_t len, esl_jtok_t **tok, esl_sb_t *out)
{
    if (!body || len == 0)
        return api_error(out, 400, "missing JSON body");
    if (len > ESL_API_MAX_BODY)
        return api_error(out, 413, "body too large");
    *tok = malloc(sizeof(esl_jtok_t) * ESL_API_MAX_TOKENS);
    if (!*tok)
        return api_error(out, 500, "out of memory");
    int n = esl_json_parse(body, len, *tok, ESL_API_MAX_TOKENS);
    if (n == ESL_JSON_ERR_NOMEM || n == ESL_JSON_ERR_TOO_BIG)
        return api_error(out, 413, "body too large");
    if (n < 0)
        return api_error(out, 400, "invalid JSON");
    if ((*tok)[0].type != ESL_JSON_OBJECT)
        return api_error(out, 400, "body must be a JSON object");
    return 0;
}

/* ---- response writing -------------------------------------------------------- */

static void write_set_result(esl_sb_t *out, uint64_t mac, const esl_set_result_t *r)
{
    char m[17];
    esl_mac_format(mac, m);
    esl_sb_printf(out, "{\"mac\":\"%s\",\"content_id\":%" PRIu32 ",\"changed\":%s,\"pending\":%s}",
                  m, r->content_id, r->changed ? "true" : "false", r->pending ? "true" : "false");
}

static void write_opt_u16(esl_sb_t *out, const char *key, uint16_t v)
{
    if (v)
        esl_sb_printf(out, "\"%s\":%u", key, (unsigned)v);
    else
        esl_sb_printf(out, "\"%s\":null", key);
}

/* "poll_s":<effective>,"config":{<the tag's own settings>},"config_pending":<bool> */
static void write_tag_cfg(esl_sb_t *out, const esl_tag_t *t, uint16_t default_poll_s)
{
    esl_tag_cfg_t eff;
    esl_tag_effective_cfg(t, default_poll_s, &eff);
    write_opt_u16(out, "poll_s", eff.poll_s);
    esl_sb_puts(out, ",\"config\":{");
    write_opt_u16(out, "poll_s", t->cfg.poll_s);
    esl_sb_putc(out, ',');
    write_opt_u16(out, "max_poll_s", t->cfg.max_poll_s);
    esl_sb_putc(out, ',');
    write_opt_u16(out, "lost_ap_s", t->cfg.lost_ap_s);
    esl_sb_printf(out, "},\"config_pending\":%s", esl_tag_cfg_pending(t, default_poll_s) ? "true" : "false");
}

static const char *layout_name(const esl_content_t *c)
{
    if (c->layout == ESL_LAYOUT_NAMED)
        return c->name;
    return c->layout == ESL_LAYOUT_PRICE ? "price" : "text";
}

static void write_tag(const esl_api_t *api, esl_sb_t *out, const esl_tag_t *t, uint64_t now_ms,
                      uint16_t default_poll_s, bool with_content)
{
    char m[17];
    esl_mac_format(t->mac, m);
    esl_sb_printf(out, "{\"mac\":\"%s\",", m);
    if (t->seen)
        esl_sb_printf(out, "\"seen_s_ago\":%" PRIu64 ",\"battery_mv\":%u,\"rssi\":%d,\"fw\":%u,",
                      (now_ms - t->last_seen_ms) / 1000, (unsigned)t->battery_mv, (int)t->rssi, (unsigned)t->fw);
    else
        esl_sb_puts(out, "\"seen_s_ago\":null,\"battery_mv\":null,\"rssi\":null,\"fw\":null,");
    /* model: null until the tag has polled, and for firmware that does not
     * send it; draw_failed: the tag's last POLL said its last drawing failed */
    const char *model = t->seen ? esl_model_name(t->model) : NULL;
    if (model)
        esl_sb_printf(out, "\"model\":\"%s\",", model);
    else
        esl_sb_puts(out, "\"model\":null,");
    esl_sb_printf(out, "\"draw_failed\":%s,", (t->seen && (t->poll_flags & ESL_POLL_FLAG_DRAW_FAILED)) ? "true" : "false");
    esl_sb_printf(out, "\"content_id\":%" PRIu32 ",", t->has_content ? t->content_id : 0);
    if (t->seen)
        esl_sb_printf(out, "\"shown_content_id\":%" PRIu32 ",", t->shown_content_id);
    else
        esl_sb_puts(out, "\"shown_content_id\":null,");
    esl_sb_printf(out, "\"pending\":%s,", esl_tag_pending(t) ? "true" : "false");
    write_tag_cfg(out, t, default_poll_s);

    /* layout file: the version the tag reported (null for firmware without
     * layout files), whether it still has to download the AP's file for its
     * model, and whether it rejected that file */
    uint32_t v = 0;
    if (model && api->layouts)
        esl_layouts_info(api->layouts, t->model, &v, NULL, NULL);
    bool capable = t->seen && t->layout_capable;
    if (capable)
        esl_sb_printf(out, ",\"layout_version\":%" PRIu32 ",", t->layout_ver);
    else
        esl_sb_puts(out, ",\"layout_version\":null,");
    esl_sb_printf(out, "\"layout_pending\":%s,\"layout_failed\":%s,\"content_error\":",
                  capable && t->layout_ver != v ? "true" : "false",
                  capable && v && t->layout_failed == v ? "true" : "false");
    const char *err = t->has_content && esl_tag_pending(t) ? esl_content_err_text(t->content_err) : NULL;
    if (err)
        esl_sb_json_str(out, err);
    else
        esl_sb_puts(out, "null");

    if (with_content && t->has_content) {
        esl_sb_puts(out, ",\"layout\":");
        esl_sb_json_str(out, layout_name(&t->content));
        esl_sb_puts(out, ",\"fields\":[");
        for (unsigned i = 0; i < t->content.nfields; i++) {
            uint8_t len;
            const uint8_t *f = esl_content_field(&t->content, i, &len);
            if (i)
                esl_sb_putc(out, ',');
            esl_sb_json_latin1(out, f, len);
        }
        esl_sb_printf(out, "],\"price_highlight\":%s,\"frame\":%s",
                      (t->content.flags & ESL_CFLAG_PRICE_HIGHLIGHT) ? "true" : "false",
                      (t->content.flags & ESL_CFLAG_FRAME) ? "true" : "false");
    }
    esl_sb_putc(out, '}');
}

/* ---- handlers ------------------------------------------------------------------ */

static int put_one(const esl_api_t *api, uint64_t mac, const char *body, size_t len, esl_sb_t *out)
{
    esl_jtok_t *tok = NULL;
    int status = parse_json(body, len, &tok, out);
    if (status == 0) {
        json_t j = {body, tok};
        esl_content_t c;
        char msg[MSG_MAX];
        esl_set_result_t r;
        status = parse_content(api, &j, 0, mac, &c, msg);
        if (status)
            api_error(out, status, msg);
        else if (esl_store_set(api->store, mac, &c, &r) != ESL_STORE_OK)
            status = api_error(out, 507, "tag table full");
        else {
            write_set_result(out, mac, &r);
            status = 200;
        }
    }
    free(tok);
    return status;
}

static int put_batch(const esl_api_t *api, const char *body, size_t len, esl_sb_t *out)
{
    esl_jtok_t *tok = NULL;
    uint64_t *macs = NULL;
    esl_content_t *contents = NULL;
    int status = parse_json(body, len, &tok, out);
    if (status)
        goto done;

    json_t j = {body, tok};
    int arr = esl_json_obj_get(body, tok, 0, "tags");
    if (arr < 0 || tok[arr].type != ESL_JSON_ARRAY) {
        status = api_error(out, 422, "tags must be an array");
        goto done;
    }
    unsigned n = tok[arr].size;
    if (n > ESL_API_MAX_BATCH) {
        status = api_error(out, 422, "at most 50 tags per request");
        goto done;
    }
    macs = malloc(sizeof(*macs) * (n ? n : 1));
    contents = malloc(sizeof(*contents) * (n ? n : 1));
    if (!macs || !contents) {
        status = api_error(out, 500, "out of memory");
        goto done;
    }

    /* Validate everything first so a bad entry changes nothing. */
    int i = arr + 1;
    unsigned new_macs = 0;
    for (unsigned k = 0; k < n; k++) {
        char msg[MSG_MAX];
        int mt = esl_json_obj_get(body, tok, i, "mac");
        if (tok[i].type != ESL_JSON_OBJECT || mt < 0 || tok[mt].type != ESL_JSON_STRING ||
            !esl_mac_parse(&body[tok[mt].start], (size_t)(tok[mt].end - tok[mt].start), &macs[k])) {
            status = api_error_at(out, 422, "mac must be 16 hex digits", (int)k);
            goto done;
        }
        int st = parse_content(api, &j, i, macs[k], &contents[k], msg);
        if (st) {
            status = api_error_at(out, st, msg, (int)k);
            goto done;
        }
        esl_tag_t existing;
        bool dup = false;
        for (unsigned d = 0; d < k; d++)
            dup |= macs[d] == macs[k];
        if (!dup && (!esl_store_get(api->store, macs[k], &existing) || !esl_tag_holds_data(&existing)))
            new_macs++;
        i = tok[i].next;
    }
    if (new_macs > esl_store_available(api->store)) {
        status = api_error(out, 507, "tag table full");
        goto done;
    }

    esl_sb_puts(out, "{\"tags\":[");
    for (unsigned k = 0; k < n; k++) {
        esl_set_result_t r;
        if (k)
            esl_sb_putc(out, ',');
        if (esl_store_set(api->store, macs[k], &contents[k], &r) == ESL_STORE_OK) {
            write_set_result(out, macs[k], &r);
        } else {
            char m[17];
            esl_mac_format(macs[k], m);
            esl_sb_printf(out, "{\"mac\":\"%s\",\"error\":\"tag table full\"}", m);
        }
    }
    esl_sb_puts(out, "]}");
    status = 200;

done:
    free(tok);
    free(macs);
    free(contents);
    return status;
}

static int get_all(const esl_api_t *api, uint64_t now_ms, esl_sb_t *out)
{
    /* The list can be far larger than the buffer (ESL_API_RESP_SIZE): a
       glue with a sink gets it in pieces. Nothing below can fail. */
    esl_sb_stream(out, 200);
    esl_sb_puts(out, "{\"ap\":{\"fw\":");
    esl_sb_json_str(out, api->fw_version ? api->fw_version : "");
    uint16_t def_poll = esl_store_get_next_poll(api->store);
    esl_sb_printf(out, ",\"uptime_s\":%" PRIu64 ",\"channel\":%u,\"max_tags\":%u,\"poll_s\":%u",
                  now_ms / 1000, (unsigned)api->channel, api->store->cap, (unsigned)def_poll);
    if (api->reset_reason) {
        esl_sb_puts(out, ",\"reset\":");
        esl_sb_json_str(out, api->reset_reason);
    }
    esl_sb_puts(out, "},\"tags\":[");
    bool first = true;
    for (unsigned s = 0; s < api->store->cap; s++) {
        esl_tag_t t;
        if (!esl_store_get_slot(api->store, s, &t))
            continue;
        if (!first)
            esl_sb_putc(out, ',');
        first = false;
        write_tag(api, out, &t, now_ms, def_poll, false);
    }
    esl_sb_puts(out, "]}");
    return 200;
}

/* ---- settings ------------------------------------------------------------------ */

/* PUT/GET /api/tags/{mac}/config */
static int tag_config(const esl_api_t *api, const char *method, uint64_t mac, const char *body, size_t len,
                      esl_sb_t *out)
{
    char m[17];
    esl_tag_t t;
    esl_mac_format(mac, m);
    if (strcmp(method, "GET") == 0) {
        if (!esl_store_get(api->store, mac, &t))
            return api_error(out, 404, "unknown tag");
        esl_sb_printf(out, "{\"mac\":\"%s\",", m);
        write_tag_cfg(out, &t, esl_store_get_next_poll(api->store));
        esl_sb_putc(out, '}');
        return 200;
    }
    if (strcmp(method, "PUT") != 0)
        return api_error(out, 405, "method not allowed");

    esl_jtok_t *tok = NULL;
    int status = parse_json(body, len, &tok, out);
    if (status == 0) {
        json_t j = {body, tok};
        esl_tag_cfg_t cfg;
        esl_cfg_result_t r;
        const char *msg;
        status = parse_cfg(&j, 0, &cfg, &msg);
        if (status)
            api_error(out, status, msg);
        else if (esl_store_set_cfg(api->store, mac, &cfg, &r) != ESL_STORE_OK)
            status = api_error(out, 507, "tag table full");
        else if (!esl_store_get(api->store, mac, &t))
            status = api_error(out, 500, "tag vanished");
        else {
            esl_sb_printf(out, "{\"mac\":\"%s\",", m);
            write_tag_cfg(out, &t, esl_store_get_next_poll(api->store));
            esl_sb_printf(out, ",\"changed\":%s}", r.changed ? "true" : "false");
            status = 200;
        }
    }
    free(tok);
    return status;
}

/* PUT/GET /api/config: AP-wide default interval. poll_s 0 = tags keep their
 * built-in interval, null = back to the AP's built-in default (menuconfig). */
static int ap_config(const esl_api_t *api, const char *method, const char *body, size_t len, esl_sb_t *out)
{
    bool put = strcmp(method, "PUT") == 0;
    if (put) {
        esl_jtok_t *tok = NULL;
        int status = parse_json(body, len, &tok, out);
        if (status == 0) {
            json_t j = {body, tok};
            int v = esl_json_obj_get(body, tok, 0, "poll_s");
            uint16_t poll_s;
            if (v < 0)
                status = api_error(out, 422, "poll_s is required (null resets it to the built-in default)");
            else if (tok[v].type == ESL_JSON_NULL)
                esl_store_reset_next_poll(api->store);
            else if (parse_uint(&j, 0, "poll_s", 0, ESL_CFG_POLL_MAX_S, &poll_s) < 0 ||
                     (poll_s && poll_s < ESL_CFG_POLL_MIN_S))
                status = api_error(out, 422, "poll_s must be null, 0 or a whole number of seconds from 10 to 3600");
            else
                esl_store_set_next_poll(api->store, poll_s);
        }
        free(tok);
        if (status)
            return status;
    } else if (strcmp(method, "GET") != 0) {
        return api_error(out, 405, "method not allowed");
    }
    esl_sb_printf(out, "{\"poll_s\":%u,\"builtin_poll_s\":%u}", (unsigned)esl_store_get_next_poll(api->store),
                  (unsigned)api->store->next_poll_builtin);
    return 200;
}

/* GET/PUT /api/wifi: the network the AP joins (krav A1). The password is never
 * shown. ssid "" or null forgets the network, and the AP opens its setup
 * network again. */
static int ap_wifi(const esl_api_t *api, const char *method, const char *body, size_t len, esl_sb_t *out)
{
    esl_wifi_status_t ws;
    if (strcmp(method, "PUT") == 0) {
        esl_jtok_t *tok = NULL;
        int status = parse_json(body, len, &tok, out);
        if (status == 0) {
            char ssid[ESL_WIFI_SSID_MAX + 1] = "", pass[ESL_WIFI_PASS_MAX + 1] = "";
            int s = esl_json_obj_get(body, tok, 0, "ssid");
            int p = esl_json_obj_get(body, tok, 0, "password");
            int sl = 0, pl = 0;
            const char *err = NULL;
            if (s < 0)
                err = "ssid is required (null or \"\" forgets the saved network)";
            else if (tok[s].type == ESL_JSON_STRING)
                sl = esl_json_str_decode(body, &tok[s], ssid, sizeof(ssid) - 1);
            else if (tok[s].type != ESL_JSON_NULL)
                err = "ssid must be a string or null";
            if (!err && p >= 0 && tok[p].type == ESL_JSON_STRING)
                pl = esl_json_str_decode(body, &tok[p], pass, sizeof(pass) - 1);
            else if (!err && p >= 0 && tok[p].type != ESL_JSON_NULL)
                err = "password must be a string";
            if (!err && sl == -2)
                err = "ssid is longer than 32 bytes";
            else if (!err && pl == -2)
                err = "password must be empty (open network) or 8 to 63 characters";
            else if (!err && (sl < 0 || pl < 0))
                err = "bad escape in ssid or password";
            if (!err) {
                ssid[sl] = 0;
                pass[pl] = 0;
                err = esl_wifi_check(ssid, (size_t)sl, pass, (size_t)pl);
            }
            if (err)
                status = api_error(out, 422, err);
            else if (api->wifi->set(api->wifi->ctx, ssid, pass) != 0)
                status = api_error(out, 500, "could not save the network");
        }
        free(tok);
        if (status)
            return status;
    } else if (strcmp(method, "GET") != 0) {
        return api_error(out, 405, "method not allowed");
    }
    api->wifi->status(api->wifi->ctx, &ws);
    esl_wifi_write_json(out, &ws);
    return 200;
}

/* ---- layouts ------------------------------------------------------------------- */

static void write_layout_info(esl_sb_t *out, esl_layouts_t *ls, uint8_t model)
{
    uint32_t version;
    size_t len, bin;
    esl_sb_printf(out, "{\"model\":\"%s\",", esl_model_name(model));
    if (esl_layouts_info(ls, model, &version, &len, &bin))
        esl_sb_printf(out, "\"version\":%" PRIu32 ",\"size\":%u,\"binary_size\":%u,\"layouts\":", version,
                      (unsigned)len, (unsigned)bin);
    else
        esl_sb_puts(out, "\"version\":null,\"size\":0,\"binary_size\":0,\"layouts\":");
    esl_layouts_write_names(ls, model, out);
    esl_sb_putc(out, '}');
}

static int put_layout(const esl_api_t *api, uint8_t model, const char *body, size_t len, esl_sb_t *out)
{
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

    /* The file must work on this model's tags (krav A13): 422 with where and why. */
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
    esl_sb_printf(out, "{\"model\":\"%s\",\"version\":%" PRIu32 ",\"size\":%u,", esl_model_name(model), version,
                  (unsigned)len);
    size_t bsize;
    esl_layouts_info(api->layouts, model, NULL, NULL, &bsize);
    esl_sb_printf(out, "\"binary_size\":%u,\"layouts\":", (unsigned)bsize);
    esl_layouts_write_names(api->layouts, model, out);
    esl_sb_printf(out, ",\"changed\":%s}", changed ? "true" : "false");
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
        esl_sb_printf(out, "{\"max_size\":%u,\"max_binary_size\":%u,\"layouts\":[", (unsigned)ESL_LAYOUT_MAX_SIZE,
                      (unsigned)ESL_LBIN_MAX);
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
        if (!esl_layouts_delete(ls, model))
            return api_error(out, 404, "no layout for this model");
        esl_sb_printf(out, "{\"model\":\"%s\",\"deleted\":true}", esl_model_name(model));
        return 200;
    }
    return api_error(out, 405, "method not allowed");
}

/* ---- status page ------------------------------------------------------------------ */

/* Latin-1 to UTF-8, escaped for HTML text and attribute values. */
static void html_latin1(esl_sb_t *out, const uint8_t *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = s[i];
        if (c == '&')
            esl_sb_puts(out, "&amp;");
        else if (c == '<')
            esl_sb_puts(out, "&lt;");
        else if (c == '>')
            esl_sb_puts(out, "&gt;");
        else if (c == '"')
            esl_sb_puts(out, "&#34;");
        else if (c >= 0x80) {
            esl_sb_putc(out, (char)(0xC0 | (c >> 6)));
            esl_sb_putc(out, (char)(0x80 | (c & 0x3F)));
        } else
            esl_sb_putc(out, (char)c);
    }
}

static void html_ago(esl_sb_t *out, uint64_t s)
{
    if (s < 120)
        esl_sb_printf(out, "%" PRIu64 " s", s);
    else if (s < 7200)
        esl_sb_printf(out, "%" PRIu64 " min", s / 60);
    else if (s < 172800)
        esl_sb_printf(out, "%" PRIu64 " h", s / 3600);
    else
        esl_sb_printf(out, "%" PRIu64 " d", s / 86400);
}

int esl_api_status_page(const esl_api_t *api, uint64_t now_ms, esl_sb_t *out)
{
    sb_reset(out);
    esl_sb_stream(out, 200); /* as in get_all: one row per tag, far more than the buffer */
    esl_sb_puts(out,
                "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                "<meta http-equiv=\"refresh\" content=\"30\"><title>OpenESL AP</title><style>"
                "body{font-family:sans-serif;margin:16px}table{border-collapse:collapse}"
                "td,th{border:1px solid #999;padding:4px 8px;text-align:left}"
                ".p{color:#b00}</style></head><body><h1>OpenESL AP</h1><p>Firmware ");
    html_latin1(out, (const uint8_t *)(api->fw_version ? api->fw_version : ""),
                api->fw_version ? strlen(api->fw_version) : 0);
    esl_sb_puts(out, ", up ");
    html_ago(out, now_ms / 1000);
    esl_sb_printf(out, ", channel %u, %u of %u tags.</p>", (unsigned)api->channel,
                  esl_store_count(api->store), api->store->cap);
    if (api->wifi)
        esl_sb_puts(out, "<p><a href=\"/setup\">WiFi and API token setup</a></p>");
    if (api->layouts) {
        esl_sb_puts(out, "<p>Layout files:");
        for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
            uint32_t v;
            size_t bin;
            esl_sb_printf(out, "%s %s ", m > 1 ? ";" : "", esl_model_name(m));
            if (esl_layouts_info(api->layouts, m, &v, NULL, &bin)) {
                esl_sb_printf(out, "version %" PRIu32 " (%u bytes for the tags): ", v, (unsigned)bin);
                esl_layouts_write_names(api->layouts, m, out); /* plain ASCII names */
            } else {
                esl_sb_puts(out, "none (built-in price and text only)");
            }
        }
        esl_sb_puts(out, ".</p>");
    }
    esl_sb_puts(out, "<table><tr><th>MAC</th><th>Model</th><th>Seen</th><th>Battery</th><th>RSSI</th>"
                     "<th>FW</th><th>Content</th><th>State</th></tr>");

    for (unsigned s = 0; s < api->store->cap; s++) {
        esl_tag_t t;
        char m[17];
        if (!esl_store_get_slot(api->store, s, &t))
            continue;
        esl_mac_format(t.mac, m);
        const char *model = t.seen ? esl_model_name(t.model) : NULL;
        esl_sb_printf(out, "<tr><td>%s</td><td>%s</td><td>", m, model ? model : "-");
        if (t.seen) {
            html_ago(out, (now_ms - t.last_seen_ms) / 1000);
            esl_sb_printf(out, " ago</td><td>%u mV</td><td>%d</td><td>%u</td><td>", (unsigned)t.battery_mv,
                          (int)t.rssi, (unsigned)t.fw);
        } else {
            esl_sb_puts(out, "-</td><td>-</td><td>-</td><td>-</td><td>");
        }
        if (t.has_content) {
            esl_sb_puts(out, layout_name(&t.content));
            esl_sb_puts(out, ": ");
            for (unsigned i = 0; i < t.content.nfields; i++) {
                uint8_t len;
                const uint8_t *f = esl_content_field(&t.content, i, &len);
                if (i)
                    esl_sb_puts(out, " | ");
                html_latin1(out, f, len);
            }
        } else {
            esl_sb_puts(out, "-");
        }
        if (esl_tag_pending(&t))
            esl_sb_puts(out, "</td><td class=\"p\">pending");
        else if (t.has_content)
            esl_sb_puts(out, "</td><td>shown");
        else
            esl_sb_puts(out, "</td><td>no content");
        if (t.seen && (t.poll_flags & ESL_POLL_FLAG_DRAW_FAILED))
            esl_sb_puts(out, ", draw failed");
        const char *err = esl_tag_pending(&t) ? esl_content_err_text(t.content_err) : NULL;
        if (err)
            esl_sb_printf(out, " (%s)", err);
        if (t.seen && t.layout_capable && api->layouts) {
            uint32_t v;
            esl_layouts_info(api->layouts, t.model, &v, NULL, NULL);
            if (v && t.layout_failed == v)
                esl_sb_puts(out, ", layout file rejected");
            else if (t.layout_ver != v)
                esl_sb_puts(out, ", layout file pending");
        }
        esl_sb_puts(out, "</td></tr>");
    }
    esl_sb_puts(out, "</table></body></html>");
    if (out->overflow) {
        sb_reset(out);
        esl_sb_puts(out, "status page too large");
        return 500;
    }
    return 200;
}

int esl_api_handle(const esl_api_t *api, const char *method, const char *uri, const char *auth,
                   const char *body, size_t body_len, uint64_t now_ms, esl_sb_t *out)
{
    sb_reset(out);
    size_t plen = strcspn(uri, "?");
    const size_t base = sizeof(PATH_TAGS) - 1;

    if (plen < 5 || memcmp(uri, "/api/", 5) != 0)
        return api_error(out, 404, "not found");
    if (!auth_ok(api->token, auth))
        return api_error(out, 401, "unauthorized");

    const size_t lbase = sizeof(PATH_LAYOUTS) - 1;
    if (api->layouts && plen >= lbase && memcmp(uri, PATH_LAYOUTS, lbase) == 0 &&
        (plen == lbase || uri[lbase] == '/')) {
        int status = handle_layouts(api, method, uri, plen, body, body_len, out);
        if (out->overflow)
            return api_error(out, 500, "response too large");
        return status;
    }

    const size_t cbase = sizeof(PATH_CONFIG) - 1;
    if ((plen == cbase || (plen == cbase + 1 && uri[cbase] == '/')) && memcmp(uri, PATH_CONFIG, cbase) == 0) {
        int status = ap_config(api, method, body, body_len, out);
        return out->overflow ? api_error(out, 500, "response too large") : status;
    }

    const size_t wbase = sizeof(PATH_WIFI) - 1;
    if (api->wifi && (plen == wbase || (plen == wbase + 1 && uri[wbase] == '/')) &&
        memcmp(uri, PATH_WIFI, wbase) == 0) {
        int status = ap_wifi(api, method, body, body_len, out);
        return out->overflow ? api_error(out, 500, "response too large") : status;
    }

    bool collection = false, item = false, item_cfg = false;
    uint64_t mac = 0;
    if (plen >= base && memcmp(uri, PATH_TAGS, base) == 0) {
        if (plen == base || (plen == base + 1 && uri[base] == '/'))
            collection = true;
        else if (uri[base] == '/') {
            static const char cfg_suffix[] = "/config";
            const size_t sl = sizeof(cfg_suffix) - 1;
            size_t ml = plen - base - 1;
            if (ml == 17 && uri[base + 17] == '/')
                ml = 16;
            else if ((ml == 16 + sl || ml == 17 + sl) && memcmp(&uri[base + 17], cfg_suffix, sl) == 0 &&
                     (ml == 16 + sl || uri[base + 17 + sl] == '/')) {
                ml = 16;
                item_cfg = true;
            }
            if (!esl_mac_parse(&uri[base + 1], ml, &mac))
                return api_error(out, 400, "mac must be 16 hex digits");
            item = !item_cfg;
        }
    }

    int status;
    if (item_cfg)
        status = tag_config(api, method, mac, body, body_len, out);
    else if (collection && strcmp(method, "GET") == 0)
        status = get_all(api, now_ms, out);
    else if (collection && strcmp(method, "PUT") == 0)
        status = put_batch(api, body, body_len, out);
    else if (item && strcmp(method, "PUT") == 0)
        status = put_one(api, mac, body, body_len, out);
    else if (item && strcmp(method, "GET") == 0) {
        esl_tag_t t;
        if (!esl_store_get(api->store, mac, &t))
            return api_error(out, 404, "unknown tag");
        write_tag(api, out, &t, now_ms, esl_store_get_next_poll(api->store), true);
        status = 200;
    } else if (item && strcmp(method, "DELETE") == 0) {
        char m[17];
        if (!esl_store_delete(api->store, mac))
            return api_error(out, 404, "unknown tag");
        esl_mac_format(mac, m);
        esl_sb_printf(out, "{\"mac\":\"%s\",\"deleted\":true}", m);
        status = 200;
    } else if (collection || item)
        return api_error(out, 405, "method not allowed");
    else
        return api_error(out, 404, "not found");

    if (out->overflow)
        return api_error(out, 500, "response too large");
    return status;
}
