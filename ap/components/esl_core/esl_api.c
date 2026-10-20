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
#define PATH_TIME "/api/time"
#define PATH_SCHEDULE "/api/schedule"
#define PATH_HEALTH "/api/health"
#define PATH_OTA "/api/ota"
#define PATH_TAGFW "/api/tag-firmware"

_Static_assert(ESL_CONTENT_FIELD_SPACE == 96 && ESL_MAX_FIELDS == 6, "update the \"text too long\" message");
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
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 422: return "422 Unprocessable Entity";
    case 429: return "429 Too Many Requests";
    case 501: return "501 Not Implemented";
    case 507: return "507 Insufficient Storage";
    default:  return "500 Internal Server Error";
    }
}

/* ---- auth ---------------------------------------------------------------- */

bool esl_api_auth_ok(const char *token, const char *auth)
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

unsigned esl_api_lock_wait(const esl_api_lock_t *lock, uint64_t now_ms)
{
    if (!lock || now_ms >= lock->locked_until_ms)
        return 0;
    return (unsigned)((lock->locked_until_ms - now_ms + 999) / 1000);
}

void esl_api_lock_note(esl_api_lock_t *lock, bool ok, uint64_t now_ms)
{
    if (!lock)
        return;
    if (ok) {
        lock->fails = 0;
    } else if (++lock->fails >= ESL_API_AUTH_TRIES) {
        lock->fails = 0;
        lock->locked_until_ms = now_ms + ESL_API_AUTH_LOCK_S * 1000u;
    }
}

int esl_api_auth_check(const esl_api_t *api, const char *auth, uint64_t now_ms, unsigned *wait_s)
{
    *wait_s = esl_api_lock_wait(api->lock, now_ms);
    if (*wait_s)
        return 429;
    bool ok = esl_api_auth_ok(api->token, auth);
    esl_api_lock_note(api->lock, ok, now_ms);
    return ok ? 0 : 401;
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

/* "at": when scheduled content takes effect, Unix seconds (UTC). Missing or
 * null gives 0 (now). Returns 0 or 422 with msg set. */
static int parse_at(const esl_api_t *api, const json_t *j, int obj, uint32_t *at, char *msg)
{
    int v = esl_json_obj_get(j->js, j->tok, obj, "at");
    *at = 0;
    if (v < 0 || j->tok[v].type == ESL_JSON_NULL)
        return 0;
    uint64_t n = 0;
    bool ok = j->tok[v].type == ESL_JSON_NUMBER;
    for (int i = j->tok[v].start; ok && i < j->tok[v].end; i++) {
        char c = j->js[i];
        ok = c >= '0' && c <= '9' && n <= UINT32_MAX;
        n = n * 10 + (uint64_t)(c - '0');
    }
    if (!ok || n < ESL_SCHED_MIN_UNIX || n > UINT32_MAX) {
        snprintf(msg, MSG_MAX, "at must be null or a time in whole seconds since 1970 (UTC), after 2020");
        return 422;
    }
    if (!api->sched) {
        snprintf(msg, MSG_MAX, "this AP has no room for scheduled content (ESL_MAX_SCHEDULED is 0)");
        return 422;
    }
    *at = (uint32_t)n;
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

static int text_too_long(char *msg)
{
    snprintf(msg, MSG_MAX, "text too long: the fields share 96 bytes, one per field plus the text after "
                           "Latin-1 conversion (92 bytes of text with four fields, 90 with six)");
    return 422;
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

    /* the fields as Latin-1, back to back; empty fields at the end are dropped
       before they go into c, so they cost no length bytes */
    uint8_t text[ESL_MAX_TEXT];
    size_t len[ESL_MAX_FIELDS] = {0}, used = 0;
    unsigned nf = (unsigned)j->tok[arr].size;
    int i = arr + 1;
    for (unsigned k = 0; k < nf; k++) {
        const esl_jtok_t *t = &j->tok[i];
        char utf8[ESL_MAX_TEXT * 4 + 4];
        size_t n = 0;
        bool overflow = false;

        if (t->type == ESL_JSON_STRING) {
            int ul = esl_json_str_decode(j->js, t, utf8, sizeof(utf8));
            if (ul == -1) {
                snprintf(msg, MSG_MAX, "invalid string escape");
                return 400;
            }
            if (ul >= 0)
                n = esl_utf8_to_latin1(utf8, (size_t)ul, text + used, sizeof(text) - used, &overflow);
            else
                overflow = true;
        } else if (t->type != ESL_JSON_NULL) {
            snprintf(msg, MSG_MAX, "fields must be an array of strings");
            return 422;
        }
        if (overflow)
            return text_too_long(msg);
        len[k] = n;
        used += n;
        i = t->next;
    }
    while (nf > 0 && len[nf - 1] == 0)
        nf--;
    used = 0;
    for (unsigned k = 0; k < nf; used += len[k++])
        if (esl_content_add_field(c, text + used, len[k]) != ESL_CONTENT_OK)
            return text_too_long(msg);

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

static void write_sched_result(esl_sb_t *out, uint64_t mac, uint32_t at)
{
    char m[17];
    esl_mac_format(mac, m);
    esl_sb_printf(out, "{\"mac\":\"%s\",\"scheduled\":true,\"at\":%" PRIu32 "}", m, at);
}

#define NO_TIME_MSG "the AP does not know the time yet: set it with PUT /api/time, or let it reach an NTP server"

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

/* ,"layout":...,"fields":[...],"price_highlight":...,"frame":... */
static void write_content(esl_sb_t *out, const esl_content_t *c)
{
    esl_sb_puts(out, ",\"layout\":");
    esl_sb_json_str(out, layout_name(c));
    esl_sb_puts(out, ",\"fields\":[");
    for (unsigned i = 0; i < c->nfields; i++) {
        uint8_t len;
        const uint8_t *f = esl_content_field(c, i, &len);
        if (i)
            esl_sb_putc(out, ',');
        esl_sb_json_latin1(out, f, len);
    }
    esl_sb_printf(out, "],\"price_highlight\":%s,\"frame\":%s",
                  (c->flags & ESL_CFLAG_PRICE_HIGHLIGHT) ? "true" : "false",
                  (c->flags & ESL_CFLAG_FRAME) ? "true" : "false");
}

/* ?key=value in the query string: the value and its length, or NULL if absent */
static const char *query_str(const char *uri, const char *key, size_t *len)
{
    const char *q = strchr(uri, '?');
    size_t kl = strlen(key);
    while (q) {
        q++;
        if (strncmp(q, key, kl) == 0 && q[kl] == '=') {
            const char *v = q + kl + 1;
            *len = strcspn(v, "&");
            return v;
        }
        q = strchr(q, '&');
    }
    return NULL;
}

/* ?to=: 0 for "all" (or absent), the MAC otherwise; false if it is neither */
static bool query_to(const char *uri, uint64_t *mac)
{
    size_t len;
    const char *v = query_str(uri, "to", &len);
    *mac = 0;
    if (!v || (len == 3 && strncmp(v, "all", 3) == 0))
        return true;
    return esl_mac_parse(v, len, mac) && *mac;
}

/* Where one tag is with the model's image: a tag it is not for, or that cannot or
   will not take it, is not updating. Bytes it has so far in *got. */
typedef enum { FWU_NONE, FWU_UPDATED, FWU_NO_OTA, FWU_FAILED, FWU_NOT_CHOSEN, FWU_UPDATING } fwu_t;

static fwu_t tag_fw_update(const esl_api_t *api, const esl_tag_t *t, const esl_tagfw_img_t *img, uint16_t *got)
{
    *got = 0;
    if (!img->ver)
        return FWU_NONE;
    if (t->fw == img->ver)
        return FWU_UPDATED;
    if (!(t->poll_flags & ESL_POLL_F_OTA))
        return FWU_NO_OTA;
    if (t->fw_failed == img->ver)
        return FWU_FAILED;
    if (!esl_tagfw_for(api->tagfw, t->model, t->mac))
        return FWU_NOT_CHOSEN;
    if (t->fw_got_ver == img->ver)
        *got = t->fw_got < img->size ? t->fw_got : img->size;
    return FWU_UPDATING;
}

static void write_tag(const esl_api_t *api, esl_sb_t *out, const esl_tag_t *t, uint64_t now_ms,
                      uint16_t default_poll_s, bool with_content)
{
    char m[17];
    esl_mac_format(t->mac, m);
    esl_sb_printf(out, "{\"mac\":\"%s\",", m);
    /* A tag not heard since this boot, from NVS: what it said last, and when
     * (wall clock) if both that and the time now are known */
    uint32_t unix_now = (!t->seen && t->seen_unix && api->sched) ? esl_sched_now(api->sched, now_ms, NULL) : 0;
    bool known = t->seen || t->fw; /* fw 0: nothing stored from a POLL */
    if (t->seen)
        esl_sb_printf(out, "\"seen_s_ago\":%" PRIu64 ",", (now_ms - t->last_seen_ms) / 1000);
    else if (unix_now >= t->seen_unix && unix_now)
        esl_sb_printf(out, "\"seen_s_ago\":%" PRIu32 ",", unix_now - t->seen_unix);
    else
        esl_sb_puts(out, "\"seen_s_ago\":null,");
    if (known)
        esl_sb_printf(out, "\"battery_mv\":%u,\"rssi\":%d,\"fw\":%u,", (unsigned)t->battery_mv, (int)t->rssi,
                      (unsigned)t->fw);
    else
        esl_sb_puts(out, "\"battery_mv\":null,\"rssi\":null,\"fw\":null,");
    /* model: null until the tag has polled (now or before a restart), and for
     * firmware that does not send it; draw_failed: the tag's last POLL said its
     * last drawing failed */
    const char *model = known ? esl_model_name(t->model) : NULL;
    if (model)
        esl_sb_printf(out, "\"model\":\"%s\",", model);
    else
        esl_sb_puts(out, "\"model\":null,");
    esl_sb_printf(out, "\"draw_failed\":%s,", (t->seen && (t->poll_flags & ESL_POLL_FLAG_DRAW_FAILED)) ? "true" : "false");
    esl_sb_printf(out, "\"content_id\":%" PRIu32 ",", t->has_content ? t->content_id : 0);
    if (t->seen) {
        /* A tag that shows images reports the image id: the content it was drawn from
           when that is its content's image, so clients compare content ids as before,
           else 0 (an image id is not a content id; which content it was is not kept) */
        uint32_t shown = t->shown_content_id;
        if (t->image_capable)
            shown = t->has_content && t->image_id && shown == t->image_id ? t->content_id : 0;
        esl_sb_printf(out, "\"shown_content_id\":%" PRIu32 ",", shown);
    } else
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
    bool needs_file = capable && !t->image_capable; /* a tag that shows images needs no file */
    if (capable)
        esl_sb_printf(out, ",\"layout_version\":%" PRIu32 ",", t->layout_ver);
    else
        esl_sb_puts(out, ",\"layout_version\":null,");
    esl_sb_printf(out, "\"layout_pending\":%s,\"layout_failed\":%s,\"content_error\":",
                  needs_file && t->layout_ver != v ? "true" : "false",
                  needs_file && v && t->layout_failed == v ? "true" : "false");
    const char *err = t->has_content && esl_tag_pending(t) ? esl_content_err_text(t->content_err) : NULL;
    if (err)
        esl_sb_json_str(out, err);
    else
        esl_sb_puts(out, "null");
    /* the tag takes firmware over the radio (docs/tag-ota.md) */
    esl_sb_printf(out, ",\"ota\":%s", t->seen && (t->poll_flags & ESL_POLL_F_OTA) ? "true" : "false");
    /* the tag shows images the AP draws (protokoll.md "Image transfer"): the id of its
       content's image once drawn, 0 before, and the image id the tag reports */
    esl_sb_printf(out, ",\"image\":%s", t->seen && t->image_capable ? "true" : "false");
    if (t->has_content)
        esl_sb_printf(out, ",\"image_id\":%" PRIu32, t->image_id);
    else
        esl_sb_puts(out, ",\"image_id\":null");
    if (t->seen && t->image_capable)
        esl_sb_printf(out, ",\"shown_image_id\":%" PRIu32, t->shown_content_id);
    else
        esl_sb_puts(out, ",\"shown_image_id\":null");
    /* the firmware it is fetching from the AP: how far it is (null: none for it) */
    esl_tagfw_img_t img;
    uint16_t got;
    if (t->seen && esl_tagfw_get(api->tagfw, t->model, &img) && tag_fw_update(api, t, &img, &got) == FWU_UPDATING) {
        esl_sb_puts(out, ",\"fw_update\":{\"version\":");
        esl_sb_printf(out, "\"%u.%u.%u\"", img.ver / 10000u, img.ver / 100u % 100u, img.ver % 100u);
        esl_sb_printf(out, ",\"received\":%u,\"size\":%u}", (unsigned)got, (unsigned)img.size);
    } else {
        esl_sb_puts(out, ",\"fw_update\":null");
    }

    if (with_content && t->has_content)
        write_content(out, &t->content);
    if (with_content && api->sched) {
        /* when this tag's scheduled content takes effect */
        esl_sb_puts(out, ",\"scheduled\":[");
        bool first = true;
        for (unsigned s = 0; s < api->sched->cap; s++) {
            uint64_t m;
            uint32_t at;
            if (!esl_sched_get_slot(api->sched, s, &m, &at, NULL) || m != t->mac)
                continue;
            esl_sb_printf(out, "%s%" PRIu32, first ? "" : ",", at);
            first = false;
        }
        esl_sb_putc(out, ']');
    }
    esl_sb_putc(out, '}');
}

/* ---- handlers ------------------------------------------------------------------ */

/* When content with "at" takes effect: 0 = now (no "at", or a time that has
 * passed), else the time to schedule it for. Returns 0 or 409 with msg set. */
static int when(const esl_api_t *api, uint32_t at, uint64_t now_ms, uint32_t *sched_at, char *msg)
{
    *sched_at = 0;
    if (!at)
        return 0;
    uint32_t now = esl_sched_now(api->sched, now_ms, NULL);
    if (!now) {
        snprintf(msg, MSG_MAX, NO_TIME_MSG);
        return 409;
    }
    if (at > now)
        *sched_at = at;
    return 0;
}

static int put_one(const esl_api_t *api, uint64_t mac, const char *body, size_t len, uint64_t now_ms,
                   esl_sb_t *out)
{
    esl_jtok_t *tok = NULL;
    int status = parse_json(body, len, &tok, out);
    if (status == 0) {
        json_t j = {body, tok};
        esl_content_t c;
        char msg[MSG_MAX];
        esl_set_result_t r;
        uint32_t at = 0;
        status = parse_content(api, &j, 0, mac, &c, msg);
        if (!status)
            status = parse_at(api, &j, 0, &at, msg);
        if (!status)
            status = when(api, at, now_ms, &at, msg);
        if (status)
            api_error(out, status, msg);
        else if (at) {
            if (esl_sched_add(api->sched, mac, at, &c) != ESL_SCHED_OK)
                status = api_error(out, 507, "schedule full");
            else {
                write_sched_result(out, mac, at);
                status = 200;
            }
        } else if (esl_store_set(api->store, mac, &c, &r) != ESL_STORE_OK)
            status = api_error(out, 507, "tag table full");
        else {
            write_set_result(out, mac, &r);
            status = 200;
        }
    }
    free(tok);
    return status;
}

static int put_batch(const esl_api_t *api, const char *body, size_t len, uint64_t now_ms, esl_sb_t *out)
{
    esl_jtok_t *tok = NULL;
    uint64_t *macs = NULL;
    uint32_t *ats = NULL;
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
    ats = malloc(sizeof(*ats) * (n ? n : 1));
    contents = malloc(sizeof(*contents) * (n ? n : 1));
    if (!macs || !ats || !contents) {
        status = api_error(out, 500, "out of memory");
        goto done;
    }

    /* Validate everything first so a bad entry changes nothing. */
    int i = arr + 1;
    unsigned new_macs = 0, new_sched = 0;
    for (unsigned k = 0; k < n; k++) {
        char msg[MSG_MAX];
        int mt = esl_json_obj_get(body, tok, i, "mac");
        if (tok[i].type != ESL_JSON_OBJECT || mt < 0 || tok[mt].type != ESL_JSON_STRING ||
            !esl_mac_parse(&body[tok[mt].start], (size_t)(tok[mt].end - tok[mt].start), &macs[k])) {
            status = api_error_at(out, 422, "mac must be 16 hex digits", (int)k);
            goto done;
        }
        int st = parse_content(api, &j, i, macs[k], &contents[k], msg);
        if (!st)
            st = parse_at(api, &j, i, &ats[k], msg);
        if (!st)
            st = when(api, ats[k], now_ms, &ats[k], msg);
        if (st) {
            status = api_error_at(out, st, msg, (int)k);
            goto done;
        }
        bool dup = false;
        for (unsigned d = 0; d < k; d++)
            dup |= macs[d] == macs[k] && (ats[k] ? ats[d] == ats[k] : !ats[d]);
        esl_tag_t existing;
        if (!dup && ats[k])
            new_sched += !esl_sched_has(api->sched, macs[k], ats[k]);
        else if (!dup && !ats[k] && (!esl_store_get(api->store, macs[k], &existing) || !esl_tag_holds_data(&existing)))
            new_macs++;
        i = tok[i].next;
    }
    if (new_macs > esl_store_available(api->store)) {
        status = api_error(out, 507, "tag table full");
        goto done;
    }
    if (new_sched && new_sched > esl_sched_free(api->sched)) {
        status = api_error(out, 507, "schedule full");
        goto done;
    }

    esl_sb_puts(out, "{\"tags\":[");
    for (unsigned k = 0; k < n; k++) {
        esl_set_result_t r;
        if (k)
            esl_sb_putc(out, ',');
        if (ats[k] && esl_sched_add(api->sched, macs[k], ats[k], &contents[k]) == ESL_SCHED_OK) {
            write_sched_result(out, macs[k], ats[k]);
        } else if (!ats[k] && esl_store_set(api->store, macs[k], &contents[k], &r) == ESL_STORE_OK) {
            write_set_result(out, macs[k], &r);
        } else {
            char m[17];
            esl_mac_format(macs[k], m);
            esl_sb_printf(out, "{\"mac\":\"%s\",\"error\":\"%s\"}", m,
                          ats[k] ? "schedule full" : "tag table full");
        }
    }
    esl_sb_puts(out, "]}");
    status = 200;

done:
    free(tok);
    free(macs);
    free(ats);
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
    if (api->sched) {
        uint32_t t = esl_sched_now(api->sched, now_ms, NULL);
        if (t)
            esl_sb_printf(out, ",\"time\":%" PRIu32, t);
        else
            esl_sb_puts(out, ",\"time\":null");
        esl_sb_printf(out, ",\"scheduled\":%u,\"max_scheduled\":%u", esl_sched_count(api->sched),
                      api->sched->cap);
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

/* ---- clock and schedule --------------------------------------------------------- */

static void write_time(const esl_api_t *api, uint64_t now_ms, esl_sb_t *out)
{
    uint8_t src;
    uint32_t t = esl_sched_now(api->sched, now_ms, &src);
    if (t)
        esl_sb_printf(out, "{\"time\":%" PRIu32 ",\"source\":\"%s\"}", t, esl_time_src_name(src));
    else
        esl_sb_puts(out, "{\"time\":null,\"source\":null}");
}

/* GET/PUT /api/time */
static int ap_time(const esl_api_t *api, const char *method, const char *body, size_t len, uint64_t now_ms,
                   esl_sb_t *out)
{
    if (strcmp(method, "PUT") == 0) {
        esl_jtok_t *tok = NULL;
        int status = parse_json(body, len, &tok, out);
        if (status) {
            free(tok);
            return status;
        }
        json_t j = {body, tok};
        int v = esl_json_obj_get(body, tok, 0, "time");
        uint64_t n = 0;
        bool ok = v >= 0 && tok[v].type == ESL_JSON_NUMBER;
        for (int i = ok ? tok[v].start : 0; ok && i < tok[v].end; i++) {
            ok = body[i] >= '0' && body[i] <= '9' && n <= UINT32_MAX;
            n = n * 10 + (uint64_t)(body[i] - '0');
        }
        (void)j;
        free(tok);
        if (!ok || n < ESL_SCHED_MIN_UNIX || n > UINT32_MAX)
            return api_error(out, 422, "time must be whole seconds since 1970 (UTC), after 2020");
        esl_sched_set_time(api->sched, (uint32_t)n, now_ms, ESL_TIME_API);
    } else if (strcmp(method, "GET") != 0)
        return api_error(out, 405, "method not allowed");
    write_time(api, now_ms, out);
    return 200;
}

/* GET/DELETE /api/schedule, DELETE /api/schedule/{mac} */
static int ap_schedule(const esl_api_t *api, const char *method, bool has_mac, uint64_t mac, uint64_t now_ms,
                       esl_sb_t *out)
{
    if (strcmp(method, "DELETE") == 0) {
        unsigned n = esl_sched_remove(api->sched, mac, !has_mac);
        if (!n)
            return api_error(out, 404, has_mac ? "nothing scheduled for this tag" : "nothing scheduled");
        esl_sb_printf(out, "{\"deleted\":%u}", n);
        return 200;
    }
    if (strcmp(method, "GET") != 0 || has_mac)
        return api_error(out, 405, "method not allowed");
    /* Up to ESL_MAX_SCHEDULED entries with content: streamed like GET /api/tags */
    esl_sb_stream(out, 200);
    uint32_t t = esl_sched_now(api->sched, now_ms, NULL);
    if (t)
        esl_sb_printf(out, "{\"time\":%" PRIu32, t);
    else
        esl_sb_puts(out, "{\"time\":null");
    esl_sb_printf(out, ",\"max_scheduled\":%u,\"scheduled\":[", api->sched->cap);
    bool first = true;
    for (unsigned s = 0; s < api->sched->cap; s++) {
        uint64_t m;
        uint32_t at;
        esl_content_t c;
        char ms[17];
        if (!esl_sched_get_slot(api->sched, s, &m, &at, &c))
            continue;
        esl_mac_format(m, ms);
        esl_sb_printf(out, "%s{\"mac\":\"%s\",\"at\":%" PRIu32, first ? "" : ",", ms, at);
        write_content(out, &c);
        esl_sb_putc(out, '}');
        first = false;
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

/* PUT/GET /api/config: AP-wide default interval and the webhook. poll_s 0 =
 * tags keep their built-in interval, null = back to the AP's built-in default
 * (menuconfig). webhook null or "" = off. Keys left out stay as they are;
 * one of them must be there. */
static int ap_config(const esl_api_t *api, const char *method, const char *body, size_t len, esl_sb_t *out)
{
    bool put = strcmp(method, "PUT") == 0;
    if (put) {
        esl_jtok_t *tok = NULL;
        int status = parse_json(body, len, &tok, out);
        if (status == 0) {
            json_t j = {body, tok};
            int v = esl_json_obj_get(body, tok, 0, "poll_s");
            int w = esl_json_obj_get(body, tok, 0, "webhook");
            uint16_t poll_s;
            char url[ESL_WEBHOOK_URL_MAX + 1];
            int ul = 0;
            if (v < 0 && w < 0)
                status = api_error(out, 422, "poll_s or webhook is required (null resets it)");
            else if (v >= 0 && tok[v].type != ESL_JSON_NULL &&
                     (parse_uint(&j, 0, "poll_s", 0, ESL_CFG_POLL_MAX_S, &poll_s) < 0 ||
                      (poll_s && poll_s < ESL_CFG_POLL_MIN_S)))
                status = api_error(out, 422, "poll_s must be null, 0 or a whole number of seconds from 10 to 3600");
            else if (w >= 0 && !api->notify)
                status = api_error(out, 422, "this AP has no webhook");
            else if (w >= 0 && tok[w].type != ESL_JSON_NULL &&
                     (tok[w].type != ESL_JSON_STRING ||
                      (ul = esl_json_str_decode(body, &tok[w], url, sizeof(url) - 1)) < 0 ||
                      (ul && !esl_webhook_url_ok(url, (size_t)ul))))
                status = api_error(out, 422, "webhook must be null or an http://host[:port][/path] URL of at most 127 characters");
            else {
                if (v >= 0 && tok[v].type == ESL_JSON_NULL)
                    esl_store_reset_next_poll(api->store);
                else if (v >= 0)
                    esl_store_set_next_poll(api->store, poll_s);
                if (w >= 0) {
                    url[ul] = '\0';
                    strcpy(api->notify->url, url);
                    if (api->notify->persist)
                        api->notify->persist(api->notify->ctx, url);
                }
            }
        }
        free(tok);
        if (status)
            return status;
    } else if (strcmp(method, "GET") != 0) {
        return api_error(out, 405, "method not allowed");
    }
    esl_sb_printf(out, "{\"poll_s\":%u,\"builtin_poll_s\":%u,\"webhook\":", (unsigned)esl_store_get_next_poll(api->store),
                  (unsigned)api->store->next_poll_builtin);
    if (api->notify && api->notify->url[0])
        esl_sb_json_str(out, api->notify->url);
    else
        esl_sb_puts(out, "null");
    esl_sb_putc(out, '}');
    return 200;
}

/* ---- health ---------------------------------------------------------------- */

/* A number from the query string (?silent_s=600&battery_mv=2400); 0 if absent. */
static uint16_t query_u16(const char *uri, const char *key)
{
    const char *q = strchr(uri, '?');
    size_t kl = strlen(key);
    while (q) {
        q++;
        if (strncmp(q, key, kl) == 0 && q[kl] == '=') {
            unsigned long v = strtoul(q + kl + 1, NULL, 10);
            return v > 65535 ? 65535 : (uint16_t)v;
        }
        q = strchr(q, '&');
    }
    return 0;
}

/* The ESL_HEALTH_* flags of one tag in the table. */
static uint8_t tag_health(const esl_api_t *api, const esl_tag_t *t, uint64_t now_ms, const esl_health_cfg_t *cfg)
{
    uint32_t v = 0;
    if (t->seen && api->layouts)
        esl_layouts_info(api->layouts, t->model, &v, NULL, NULL);
    return esl_health_flags(t, esl_store_tag_poll_s(api->store, t->mac), v, now_ms, cfg);
}

int esl_api_health(const esl_api_t *api, uint64_t now_ms, const esl_health_cfg_t *cfg, uint8_t *prev,
                   esl_sb_t *out)
{
    unsigned counts[8] = {0}, tags = 0, problems = 0, changed = 0;
    esl_sb_stream(out, 200); /* one row per tag with a problem: can be larger than the buffer */
    esl_sb_puts(out, "{\"problems\":[");
    for (unsigned s = 0; s < api->store->cap; s++) {
        esl_tag_t t;
        if (!esl_store_get_slot(api->store, s, &t)) {
            if (prev)
                prev[s] = 0;
            continue;
        }
        uint8_t f = tag_health(api, &t, now_ms, cfg);
        tags++;
        if (prev) {
            if (prev[s] != f)
                changed++;
            prev[s] = f;
        }
        if (!f)
            continue;
        char m[17];
        esl_mac_format(t.mac, m);
        esl_sb_printf(out, "%s{\"mac\":\"%s\",\"problems\":[", problems ? "," : "", m);
        bool first = true;
        for (unsigned b = 0; b < 8; b++) {
            if (!(f & (1u << b)))
                continue;
            counts[b]++;
            esl_sb_printf(out, "%s\"%s\"", first ? "" : ",", esl_health_flag_name((uint8_t)(1u << b)));
            first = false;
        }
        if (t.seen)
            esl_sb_printf(out, "],\"seen_s_ago\":%" PRIu64 ",\"battery_mv\":%u}", (now_ms - t.last_seen_ms) / 1000,
                          (unsigned)t.battery_mv);
        else
            esl_sb_puts(out, "],\"seen_s_ago\":null,\"battery_mv\":null}");
        problems++;
    }
    esl_sb_printf(out, "],\"ok\":%s,\"tags\":%u,\"with_problems\":%u", problems ? "false" : "true", tags, problems);
    for (unsigned b = 0; b < 8; b++) {
        uint8_t flag = (uint8_t)(1u << b);
        if (strcmp(esl_health_flag_name(flag), "?") != 0)
            esl_sb_printf(out, ",\"%s\":%u", esl_health_flag_name(flag), counts[b]);
    }
    if (prev)
        esl_sb_printf(out, ",\"changed\":%u", changed);
    esl_sb_putc(out, '}');
    return 200;
}

static int get_health(const esl_api_t *api, const char *method, const char *uri, uint64_t now_ms, esl_sb_t *out)
{
    if (strcmp(method, "GET") != 0)
        return api_error(out, 405, "method not allowed");
    esl_health_cfg_t cfg = {query_u16(uri, "silent_s"), query_u16(uri, "battery_mv")};
    return esl_api_health(api, now_ms, &cfg, NULL, out);
}

/* GET/PUT /api/wifi: the network the AP joins (krav A1). The password is never
 * shown. ssid "" or null forgets the network, and the AP opens its setup
 * network again. "token" sets a new API token (the page sets both right after
 * flashing, over the USB port); "hostname" is the AP's name on the network
 * (DHCP and mDNS). With a token or a hostname, ssid may be left out to keep
 * the network. Everything is checked before anything is saved. */
static int ap_wifi(const esl_api_t *api, const char *method, const char *body, size_t len, esl_sb_t *out)
{
    esl_wifi_status_t ws;
    if (strcmp(method, "PUT") == 0) {
        esl_jtok_t *tok = NULL;
        int status = parse_json(body, len, &tok, out);
        if (status == 0) {
            char ssid[ESL_WIFI_SSID_MAX + 1] = "", pass[ESL_WIFI_PASS_MAX + 1] = "";
            char token[ESL_WIFI_TOKEN_MAX + 1] = "", host[ESL_WIFI_HOSTNAME_MAX + 1] = "";
            int s = esl_json_obj_get(body, tok, 0, "ssid");
            int h = esl_json_obj_get(body, tok, 0, "hostname");
            int p = esl_json_obj_get(body, tok, 0, "password");
            int t = esl_json_obj_get(body, tok, 0, "token");
            int sl = 0, pl = 0, tl = 0, hl = 0;
            const char *err = NULL;
            if (t >= 0 && tok[t].type == ESL_JSON_STRING)
                tl = esl_json_str_decode(body, &tok[t], token, sizeof(token) - 1);
            else if (t >= 0 && tok[t].type != ESL_JSON_NULL)
                err = "token must be a string";
            if (!err && h >= 0 && tok[h].type == ESL_JSON_STRING)
                hl = esl_json_str_decode(body, &tok[h], host, sizeof(host) - 1);
            else if (!err && h >= 0 && tok[h].type != ESL_JSON_NULL)
                err = "hostname must be a string";
            if (!err && s < 0 && t < 0 && h < 0)
                err = "ssid is required (null or \"\" forgets the saved network)";
            else if (!err && s >= 0 && tok[s].type == ESL_JSON_STRING)
                sl = esl_json_str_decode(body, &tok[s], ssid, sizeof(ssid) - 1);
            else if (!err && s >= 0 && tok[s].type != ESL_JSON_NULL)
                err = "ssid must be a string or null";
            if (!err && p >= 0 && tok[p].type == ESL_JSON_STRING)
                pl = esl_json_str_decode(body, &tok[p], pass, sizeof(pass) - 1);
            else if (!err && p >= 0 && tok[p].type != ESL_JSON_NULL)
                err = "password must be a string";
            if (!err && sl == -2)
                err = "ssid is longer than 32 bytes";
            else if (!err && pl == -2)
                err = "password must be empty (open network) or 8 to 63 characters";
            else if (!err && tl == -2)
                err = "token must be 8 to 128 characters";
            else if (!err && hl == -2)
                err = "hostname must be 1 to 32 characters";
            else if (!err && (sl < 0 || pl < 0 || tl < 0 || hl < 0))
                err = "bad escape in ssid, password, token or hostname";
            if (!err) {
                ssid[sl] = 0;
                pass[pl] = 0;
                token[tl] = 0;
                host[hl] = 0;
                err = esl_wifi_check(ssid, (size_t)sl, pass, (size_t)pl);
            }
            if (!err && h >= 0 && tok[h].type == ESL_JSON_STRING)
                err = esl_wifi_check_hostname(host, (size_t)hl);
            if (!err && hl && !api->wifi->set_hostname)
                err = "this AP cannot change its hostname";
            if (!err && tl)
                err = esl_wifi_check_token(token, (size_t)tl);
            if (!err && tl && !api->wifi->set_token)
                err = "this AP cannot change its token here";
            if (err)
                status = api_error(out, 422, err);
            else if (tl && api->wifi->set_token(api->wifi->ctx, token) != 0)
                status = api_error(out, 500, "could not save the token");
            else if (hl && api->wifi->set_hostname(api->wifi->ctx, host) != 0)
                status = api_error(out, 500, "could not save the hostname");
            else if (s >= 0 && api->wifi->set(api->wifi->ctx, ssid, pass) != 0)
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

/* ---- tag firmware (docs/tag-ota.md) ------------------------------------------- */

/* ?key=N in the query string; false if absent or not a number. */
static bool query_size(const char *uri, const char *key, size_t *v)
{
    const char *q = strchr(uri, '?');
    size_t kl = strlen(key);
    while (q) {
        q++;
        if (strncmp(q, key, kl) == 0 && q[kl] == '=' && q[kl + 1] >= '0' && q[kl + 1] <= '9') {
            *v = (size_t)strtoul(q + kl + 1, NULL, 10);
            return true;
        }
        q = strchr(q, '&');
    }
    return false;
}

static void write_tagfw_model(const esl_api_t *api, esl_sb_t *out, uint8_t model)
{
    esl_tagfw_img_t img;
    esl_tagfw_to_t to;
    if (!esl_tagfw_get(api->tagfw, model, &img) || !esl_tagfw_get_to(api->tagfw, model, &to)) {
        esl_sb_printf(out, "{\"model\":\"%s\",\"version\":null}", esl_model_name(model));
        return;
    }
    /* the seen tags of the model; progress counts the bytes of those it is for */
    unsigned n[FWU_UPDATING + 1] = {0};
    uint32_t received = 0, total = 0;
    for (unsigned s = 0; s < api->store->cap; s++) {
        esl_tag_t t;
        uint16_t got;
        if (!esl_store_get_slot(api->store, s, &t) || !t.seen || t.model != model)
            continue;
        fwu_t u = tag_fw_update(api, &t, &img, &got);
        n[u]++;
        if (u == FWU_UPDATED && esl_tagfw_for(api->tagfw, model, t.mac)) {
            received += img.size;
            total += img.size;
        } else if (u == FWU_UPDATING) {
            received += got;
            total += img.size;
        }
    }
    esl_sb_putc(out, '{');
    esl_tagfw_write_fields(out, model, &img);
    esl_sb_puts(out, ",\"to\":");
    if (to.all) {
        esl_sb_puts(out, "\"all\"");
    } else {
        esl_sb_putc(out, '[');
        for (unsigned i = 0; i < to.n; i++) {
            char m[17];
            esl_mac_format(to.mac[i], m);
            esl_sb_printf(out, "%s\"%s\"", i ? "," : "", m);
        }
        esl_sb_putc(out, ']');
    }
    esl_sb_printf(out,
                  ",\"tags\":{\"updated\":%u,\"pending\":%u,\"failed\":%u,\"no_ota\":%u,\"not_chosen\":%u},"
                  "\"progress\":{\"received\":%" PRIu32 ",\"total\":%" PRIu32 "}}",
                  n[FWU_UPDATED], n[FWU_UPDATING], n[FWU_FAILED], n[FWU_NO_OTA], n[FWU_NOT_CHOSEN], received, total);
}

static int handle_tagfw(const esl_api_t *api, const char *method, const char *uri, size_t plen,
                        const char *body, size_t body_len, esl_sb_t *out)
{
    const size_t base = sizeof(PATH_TAGFW) - 1;

    if (plen == base || (plen == base + 1 && uri[base] == '/')) {
        if (strcmp(method, "GET") == 0) {
            esl_sb_printf(out, "{\"max_size\":%u,\"part_max\":%u,\"images\":[", (unsigned)(ESL_FWF_HDR_LEN + ESL_FW_MAX),
                          (unsigned)ESL_API_MAX_BODY);
            for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
                if (m > 1)
                    esl_sb_putc(out, ',');
                write_tagfw_model(api, out, m);
            }
            esl_sb_puts(out, "]}");
            return 200;
        }
        if (strcmp(method, "PUT") != 0)
            return api_error(out, 405, "method not allowed");
        size_t offset = 0, total = body_len;
        bool has_off = query_size(uri, "offset", &offset), has_total = query_size(uri, "total", &total);
        if (has_off != has_total)
            return api_error(out, 400, "give both offset and total, or neither");
        if (!body || body_len == 0)
            return api_error(out, 400, "missing body: the tag firmware file (openesl-tag-<model>-<version>-ota.bin)");
        if (total > ESL_FWF_HDR_LEN + ESL_FW_MAX)
            return api_error(out, 413, "the file is larger than a tag image can be");
        uint64_t only;
        if (!query_to(uri, &only))
            return api_error(out, 400, "to must be all or a tag's MAC (16 hex digits)");
        return esl_tagfw_part(api->tagfw, offset, total, (const uint8_t *)body, body_len, only, out);
    }
    if (uri[base] != '/')
        return api_error(out, 404, "not found");
    size_t ml = plen - base - 1;
    if (ml > 0 && uri[base + ml] == '/')
        ml--;
    uint8_t model = esl_model_parse(&uri[base + 1], ml);
    if (model == ESL_MODEL_UNKNOWN)
        return api_error(out, 404, "unknown model (use 2.9 or 1.54)");
    if (strcmp(method, "GET") == 0) {
        if (!esl_tagfw_get(api->tagfw, model, NULL))
            return api_error(out, 404, "no tag firmware for this model");
        write_tagfw_model(api, out, model);
        return 200;
    }
    if (strcmp(method, "POST") == 0) {
        uint64_t mac;
        size_t tl;
        if (!query_str(uri, "to", &tl) || !query_to(uri, &mac))
            return api_error(out, 400, "give to=all or to= a tag's MAC (16 hex digits)");
        int status = esl_tagfw_choose(api->tagfw, model, mac);
        if (status == 404)
            return api_error(out, 404, "no tag firmware for this model");
        if (status == 409)
            return api_error(out, 409, "too many tags chosen one by one: give it to all of them (to=all)");
        write_tagfw_model(api, out, model);
        return 200;
    }
    if (strcmp(method, "DELETE") == 0) {
        if (!esl_tagfw_delete(api->tagfw, model))
            return api_error(out, 404, "no tag firmware for this model");
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
    esl_sb_printf(out, ", channel %u, %u of %u tags", (unsigned)api->channel,
                  esl_store_count(api->store), api->store->cap);
    unsigned attention = 0;
    for (unsigned s = 0; s < api->store->cap; s++) {
        esl_tag_t t;
        if (esl_store_get_slot(api->store, s, &t) && tag_health(api, &t, now_ms, NULL))
            attention++;
    }
    if (attention)
        esl_sb_printf(out, ", <span class=\"p\">%u need attention</span>", attention);
    esl_sb_puts(out, ".</p>");
    if (api->dashboard)
        esl_sb_puts(out, "<p><a href=\"/dashboard\">Dashboard</a> (needs the API token)</p>");
    if (api->wifi)
        esl_sb_puts(out, "<p><a href=\"/setup\">WiFi and API token setup</a></p>");
    if (api->notify && api->notify->url[0]) {
        esl_sb_puts(out, "<p>Health report posted to ");
        html_latin1(out, (const uint8_t *)api->notify->url, strlen(api->notify->url));
        esl_sb_puts(out, " when it changes.</p>");
    }
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
                     "<th>FW</th><th>Content</th><th>State</th><th>Attention</th></tr>");

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
            if (t.battery_mv)
                esl_sb_printf(out, " ago</td><td>%u mV</td>", (unsigned)t.battery_mv);
            else
                esl_sb_puts(out, " ago</td><td>-</td>");
            esl_sb_printf(out, "<td>%d</td><td>%u</td><td>", (int)t.rssi, (unsigned)t.fw);
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
        if (t.seen && t.layout_capable && !t.image_capable && api->layouts) {
            uint32_t v;
            esl_layouts_info(api->layouts, t.model, &v, NULL, NULL);
            if (v && t.layout_failed == v)
                esl_sb_puts(out, ", layout file rejected");
            else if (t.layout_ver != v)
                esl_sb_puts(out, ", layout file pending");
        }
        /* the same flags as GET /api/health, with spaces for the page */
        uint8_t f = tag_health(api, &t, now_ms, NULL);
        esl_sb_puts(out, f ? "</td><td class=\"p\">" : "</td><td>");
        bool first = true;
        for (unsigned b = 0; b < 8; b++) {
            if (!(f & (1u << b)))
                continue;
            if (!first)
                esl_sb_puts(out, ", ");
            for (const char *n = esl_health_flag_name((uint8_t)(1u << b)); *n; n++)
                esl_sb_putc(out, *n == '_' ? ' ' : *n);
            first = false;
        }
        esl_sb_puts(out, f ? "</td></tr>" : "ok</td></tr>");
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
    unsigned wait_s;
    int auth_status = esl_api_auth_check(api, auth, now_ms, &wait_s);
    if (auth_status == 429) {
        char msg[64];
        snprintf(msg, sizeof(msg), "too many wrong tokens: locked for %u s", wait_s);
        return api_error(out, 429, msg);
    }
    if (auth_status)
        return api_error(out, 401, "unauthorized");

    const size_t lbase = sizeof(PATH_LAYOUTS) - 1;
    if (api->layouts && plen >= lbase && memcmp(uri, PATH_LAYOUTS, lbase) == 0 &&
        (plen == lbase || uri[lbase] == '/')) {
        int status = handle_layouts(api, method, uri, plen, body, body_len, out);
        if (out->overflow)
            return api_error(out, 500, "response too large");
        return status;
    }

    const size_t fbase = sizeof(PATH_TAGFW) - 1;
    if (api->tagfw && plen >= fbase && memcmp(uri, PATH_TAGFW, fbase) == 0 && (plen == fbase || uri[fbase] == '/')) {
        int status = handle_tagfw(api, method, uri, plen, body, body_len, out);
        return out->overflow ? api_error(out, 500, "response too large") : status;
    }

    const size_t cbase = sizeof(PATH_CONFIG) - 1;
    if ((plen == cbase || (plen == cbase + 1 && uri[cbase] == '/')) && memcmp(uri, PATH_CONFIG, cbase) == 0) {
        int status = ap_config(api, method, body, body_len, out);
        return out->overflow ? api_error(out, 500, "response too large") : status;
    }

    /* OTA is handled by the transport (main/ota.c) before the request gets
       here, since the body is a firmware image; this answers where it is not */
    const size_t obase = sizeof(PATH_OTA) - 1;
    if ((plen == obase || (plen == obase + 1 && uri[obase] == '/')) && memcmp(uri, PATH_OTA, obase) == 0) {
        if (strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0)
            return api_error(out, 501, "no OTA on this transport or build");
        return api_error(out, 405, "method not allowed");
    }

    const size_t hbase = sizeof(PATH_HEALTH) - 1;
    if ((plen == hbase || (plen == hbase + 1 && uri[hbase] == '/')) && memcmp(uri, PATH_HEALTH, hbase) == 0) {
        int status = get_health(api, method, uri, now_ms, out);
        return out->overflow ? api_error(out, 500, "response too large") : status;
    }

    const size_t wbase = sizeof(PATH_WIFI) - 1;
    if (api->wifi && (plen == wbase || (plen == wbase + 1 && uri[wbase] == '/')) &&
        memcmp(uri, PATH_WIFI, wbase) == 0) {
        int status = ap_wifi(api, method, body, body_len, out);
        return out->overflow ? api_error(out, 500, "response too large") : status;
    }

    const size_t tbase = sizeof(PATH_TIME) - 1;
    if (api->sched && (plen == tbase || (plen == tbase + 1 && uri[tbase] == '/')) &&
        memcmp(uri, PATH_TIME, tbase) == 0)
        return ap_time(api, method, body, body_len, now_ms, out);

    const size_t sbase = sizeof(PATH_SCHEDULE) - 1;
    if (api->sched && plen >= sbase && memcmp(uri, PATH_SCHEDULE, sbase) == 0 &&
        (plen == sbase || uri[sbase] == '/')) {
        size_t ml = plen > sbase ? plen - sbase - 1 : 0;
        if (ml == 17 && uri[sbase + 17] == '/')
            ml = 16;
        uint64_t m = 0;
        if (ml && !esl_mac_parse(&uri[sbase + 1], ml, &m))
            return api_error(out, 400, "mac must be 16 hex digits");
        int status = ap_schedule(api, method, ml != 0, m, now_ms, out);
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
        status = put_batch(api, body, body_len, now_ms, out);
    else if (item && strcmp(method, "PUT") == 0)
        status = put_one(api, mac, body, body_len, now_ms, out);
    else if (item && strcmp(method, "GET") == 0) {
        esl_tag_t t;
        if (!esl_store_get(api->store, mac, &t))
            return api_error(out, 404, "unknown tag");
        write_tag(api, out, &t, now_ms, esl_store_get_next_poll(api->store), true);
        status = 200;
    } else if (item && strcmp(method, "DELETE") == 0) {
        char m[17];
        /* its scheduled content goes too, or it would come back */
        unsigned removed = api->sched ? esl_sched_remove(api->sched, mac, false) : 0;
        if (!esl_store_delete(api->store, mac) && !removed)
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
