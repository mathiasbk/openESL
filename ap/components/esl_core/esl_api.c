#include "esl_api.h"
#include "esl_text.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define PATH_TAGS "/api/tags"

typedef struct {
    const char *js;
    const esl_jtok_t *tok;
} json_t;

static void sb_reset(esl_sb_t *sb)
{
    sb->len = 0;
    sb->overflow = sb->cap == 0;
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

/* Fills c from {layout, fields, price_red, frame_red}. Returns 0 or an HTTP
 * status with *msg set. */
static int parse_content(const json_t *j, int obj, esl_content_t *c, const char **msg)
{
    int lt = esl_json_obj_get(j->js, j->tok, obj, "layout");
    uint8_t layout;
    if (lt >= 0 && esl_json_str_eq(j->js, &j->tok[lt], "price"))
        layout = ESL_LAYOUT_PRICE;
    else if (lt >= 0 && esl_json_str_eq(j->js, &j->tok[lt], "text"))
        layout = ESL_LAYOUT_TEXT;
    else {
        *msg = "layout must be \"price\" or \"text\"";
        return 422;
    }

    bool price_red, frame_red;
    if (parse_bool(j, obj, "price_red", &price_red) < 0 || parse_bool(j, obj, "frame_red", &frame_red) < 0) {
        *msg = "price_red and frame_red must be true or false";
        return 422;
    }
    esl_content_init(c, layout, (uint8_t)((price_red ? ESL_CFLAG_PRICE_RED : 0) | (frame_red ? ESL_CFLAG_FRAME_RED : 0)));

    int arr = esl_json_obj_get(j->js, j->tok, obj, "fields");
    if (arr < 0 || j->tok[arr].type != ESL_JSON_ARRAY) {
        *msg = "fields must be an array of strings";
        return 422;
    }
    if (j->tok[arr].size > esl_content_max_fields(layout)) {
        *msg = layout == ESL_LAYOUT_PRICE ? "layout price takes at most 3 fields" : "layout text takes at most 4 fields";
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
                *msg = "invalid string escape";
                return 400;
            }
            if (ul >= 0)
                n = esl_utf8_to_latin1(utf8, (size_t)ul, latin1, sizeof(latin1), &overflow);
            else
                overflow = true;
        } else if (t->type != ESL_JSON_NULL) {
            *msg = "fields must be an array of strings";
            return 422;
        }
        if (overflow || esl_content_add_field(c, latin1, n) != ESL_CONTENT_OK) {
            *msg = "text too long (max 90 bytes after Latin-1 conversion)";
            return 422;
        }
        i = t->next;
    }
    esl_content_trim(c);
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

static void write_tag(esl_sb_t *out, const esl_tag_t *t, uint64_t now_ms, bool with_content)
{
    char m[17];
    esl_mac_format(t->mac, m);
    esl_sb_printf(out, "{\"mac\":\"%s\",", m);
    if (t->seen)
        esl_sb_printf(out, "\"seen_s_ago\":%" PRIu64 ",\"battery_mv\":%u,\"rssi\":%d,\"fw\":%u,",
                      (now_ms - t->last_seen_ms) / 1000, (unsigned)t->battery_mv, (int)t->rssi, (unsigned)t->fw);
    else
        esl_sb_puts(out, "\"seen_s_ago\":null,\"battery_mv\":null,\"rssi\":null,\"fw\":null,");
    esl_sb_printf(out, "\"content_id\":%" PRIu32 ",", t->has_content ? t->content_id : 0);
    if (t->seen)
        esl_sb_printf(out, "\"shown_content_id\":%" PRIu32 ",", t->shown_content_id);
    else
        esl_sb_puts(out, "\"shown_content_id\":null,");
    esl_sb_printf(out, "\"pending\":%s", esl_tag_pending(t) ? "true" : "false");

    if (with_content && t->has_content) {
        esl_sb_printf(out, ",\"layout\":\"%s\",\"fields\":[", t->content.layout == ESL_LAYOUT_PRICE ? "price" : "text");
        for (unsigned i = 0; i < t->content.nfields; i++) {
            uint8_t len;
            const uint8_t *f = esl_content_field(&t->content, i, &len);
            if (i)
                esl_sb_putc(out, ',');
            esl_sb_json_latin1(out, f, len);
        }
        esl_sb_printf(out, "],\"price_red\":%s,\"frame_red\":%s",
                      (t->content.flags & ESL_CFLAG_PRICE_RED) ? "true" : "false",
                      (t->content.flags & ESL_CFLAG_FRAME_RED) ? "true" : "false");
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
        const char *msg;
        esl_set_result_t r;
        status = parse_content(&j, 0, &c, &msg);
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
        const char *msg;
        int mt = esl_json_obj_get(body, tok, i, "mac");
        if (tok[i].type != ESL_JSON_OBJECT || mt < 0 || tok[mt].type != ESL_JSON_STRING ||
            !esl_mac_parse(&body[tok[mt].start], (size_t)(tok[mt].end - tok[mt].start), &macs[k])) {
            status = api_error_at(out, 422, "mac must be 16 hex digits", (int)k);
            goto done;
        }
        int st = parse_content(&j, i, &contents[k], &msg);
        if (st) {
            status = api_error_at(out, st, msg, (int)k);
            goto done;
        }
        esl_tag_t existing;
        bool dup = false;
        for (unsigned d = 0; d < k; d++)
            dup |= macs[d] == macs[k];
        if (!dup && (!esl_store_get(api->store, macs[k], &existing) || !existing.has_content))
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
    esl_sb_puts(out, "{\"ap\":{\"fw\":");
    esl_sb_json_str(out, api->fw_version ? api->fw_version : "");
    esl_sb_printf(out, ",\"uptime_s\":%" PRIu64 ",\"channel\":%u,\"max_tags\":%u},\"tags\":[",
                  now_ms / 1000, (unsigned)api->channel, api->store->cap);
    bool first = true;
    for (unsigned s = 0; s < api->store->cap; s++) {
        esl_tag_t t;
        if (!esl_store_get_slot(api->store, s, &t))
            continue;
        if (!first)
            esl_sb_putc(out, ',');
        first = false;
        write_tag(out, &t, now_ms, false);
    }
    esl_sb_puts(out, "]}");
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

    bool collection = false, item = false;
    uint64_t mac = 0;
    if (plen >= base && memcmp(uri, PATH_TAGS, base) == 0) {
        if (plen == base || (plen == base + 1 && uri[base] == '/'))
            collection = true;
        else if (uri[base] == '/') {
            size_t ml = plen - base - 1;
            if (ml == 17 && uri[base + 17] == '/')
                ml = 16;
            if (!esl_mac_parse(&uri[base + 1], ml, &mac))
                return api_error(out, 400, "mac must be 16 hex digits");
            item = true;
        }
    }

    int status;
    if (collection && strcmp(method, "GET") == 0)
        status = get_all(api, now_ms, out);
    else if (collection && strcmp(method, "PUT") == 0)
        status = put_batch(api, body, body_len, out);
    else if (item && strcmp(method, "PUT") == 0)
        status = put_one(api, mac, body, body_len, out);
    else if (item && strcmp(method, "GET") == 0) {
        esl_tag_t t;
        if (!esl_store_get(api->store, mac, &t))
            return api_error(out, 404, "unknown tag");
        write_tag(out, &t, now_ms, true);
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
