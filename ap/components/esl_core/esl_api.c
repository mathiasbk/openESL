#include "esl_api.h"
#include "esl_crc32.h"
#include "esl_image.h"
#include "esl_render.h"
#include "esl_text.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The AP board (ap/boards/<board>/, set by ap/CMakeLists.txt); "host" in the host tests and sim */
#ifndef ESL_BOARD
#define ESL_BOARD "host"
#endif
#ifndef ESL_BOARD_NAME
#define ESL_BOARD_NAME "host"
#endif

#define PATH_TAGS "/api/tags"
#define PATH_LAYOUTS "/api/layouts"
#define PATH_CONFIG "/api/config"
#define PATH_WIFI "/api/wifi"
#define PATH_TIME "/api/time"
#define PATH_SCHEDULE "/api/schedule"
#define PATH_HEALTH "/api/health"
#define PATH_OTA "/api/ota"
#define PATH_TAGFW "/api/tag-firmware"
#define PATH_SOURCES "/api/sources"
#define PATH_RADIOS "/api/radios"

_Static_assert(ESL_CONTENT_FIELD_SPACE == 96 && ESL_MAX_FIELDS == 6, "update the \"text too long\" message");
_Static_assert(ESL_CFG_POLL_MIN_S == 10 && ESL_CFG_POLL_MAX_S == 3600 && ESL_CFG_LOST_AP_MIN_S == 60,
               "update the config error messages");
_Static_assert(ESL_NTP_MAX == 63 && ESL_TZ_MAX == 63, "update the ntp and tz error messages");
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
    case 202: return "202 Accepted";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 415: return "415 Unsupported Media Type";
    case 422: return "422 Unprocessable Entity";
    case 429: return "429 Too Many Requests";
    case 501: return "501 Not Implemented";
    case 503: return "503 Service Unavailable";
    case 507: return "507 Insufficient Storage";
    default:  return "500 Internal Server Error";
    }
}

/* A tag given content here leaves its data source (esl_source.h), or the source's next run
   would overwrite it */
static void leave_source(const esl_api_t *api, uint64_t mac)
{
    if (api->sources && api->sources->release)
        api->sources->release(api->sources, mac);
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

/* Fills cfg from {poll_s, max_poll_s, lost_ap_s, long_poll_s, long_after_min}; missing or null = default. */
static int parse_cfg(const json_t *j, int obj, esl_tag_cfg_t *cfg, const char **msg)
{
    *cfg = (esl_tag_cfg_t){0}; /* channel is not set here, and esl_store_set_cfg() compares the whole struct */
    if (parse_uint(j, obj, "poll_s", ESL_CFG_POLL_MIN_S, ESL_CFG_POLL_MAX_S, &cfg->poll_s) < 0 ||
        parse_uint(j, obj, "max_poll_s", ESL_CFG_POLL_MIN_S, ESL_CFG_POLL_MAX_S, &cfg->max_poll_s) < 0) {
        *msg = "poll_s and max_poll_s must be null or a whole number of seconds from 10 to 3600";
        return 422;
    }
    if (parse_uint(j, obj, "lost_ap_s", ESL_CFG_LOST_AP_MIN_S, ESL_CFG_LOST_AP_NEVER, &cfg->lost_ap_s) < 0) {
        *msg = "lost_ap_s must be null or a whole number of seconds from 60 to 65535 (65535 = never)";
        return 422;
    }
    if (parse_uint(j, obj, "long_poll_s", ESL_CFG_LONG_POLL_MIN_S, 0xFFFF, &cfg->long_poll_s) < 0) {
        *msg = "long_poll_s must be null or a whole number of seconds from 60 to 65535";
        return 422;
    }
    if (parse_uint(j, obj, "long_after_min", ESL_CFG_LONG_AFTER_MIN_MIN, ESL_CFG_LONG_NEVER, &cfg->long_after_min) < 0) {
        *msg = "long_after_min must be null or a whole number of minutes from 10 to 65535 (65535 = never)";
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
                !esl_layouts_check_content(api->layouts, m, c, msg, MSG_MAX))
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
static void write_tag_cfg(esl_sb_t *out, const esl_tag_t *t, const esl_tag_cfg_t *ap)
{
    esl_tag_cfg_t eff;
    esl_tag_effective_cfg(t, ap, &eff);
    write_opt_u16(out, "poll_s", eff.poll_s);
    esl_sb_puts(out, ",\"config\":{");
    write_opt_u16(out, "poll_s", t->cfg.poll_s);
    esl_sb_putc(out, ',');
    write_opt_u16(out, "max_poll_s", t->cfg.max_poll_s);
    esl_sb_putc(out, ',');
    write_opt_u16(out, "lost_ap_s", t->cfg.lost_ap_s);
    esl_sb_putc(out, ',');
    write_opt_u16(out, "long_poll_s", t->cfg.long_poll_s);
    esl_sb_putc(out, ',');
    write_opt_u16(out, "long_after_min", t->cfg.long_after_min);
    esl_sb_printf(out, "},\"config_pending\":%s", esl_tag_cfg_pending(t, ap) ? "true" : "false");
}

static const char *layout_name(const esl_content_t *c)
{
    if (c->layout == ESL_LAYOUT_NAMED)
        return c->name;
    if (c->layout == ESL_LAYOUT_IMAGE)
        return "image";
    return c->layout == ESL_LAYOUT_PRICE ? "price" : "text";
}

/* The fields to show: a picture's one field is not text */
static unsigned text_fields(const esl_content_t *c)
{
    return c->layout == ESL_LAYOUT_IMAGE ? 0 : c->nfields;
}

/* ,"layout":...,"fields":[...],"price_highlight":...,"frame":... */
static void write_content(esl_sb_t *out, const esl_content_t *c)
{
    esl_sb_puts(out, ",\"layout\":");
    esl_sb_json_str(out, layout_name(c));
    esl_sb_puts(out, ",\"fields\":[");
    for (unsigned i = 0; i < text_fields(c); i++) {
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

/* How long ago the tag was heard: since this boot, or for a tag not heard since,
 * in wall-clock time from what NVS kept, once the AP knows the time. False: not known. */
static bool seen_s_ago(const esl_api_t *api, const esl_tag_t *t, uint64_t now_ms, uint64_t *s)
{
    if (t->seen) {
        *s = esl_tag_seen_ms_ago(t, now_ms) / 1000;
        return true;
    }
    uint32_t unix_now = t->seen_unix && api->sched ? esl_sched_now(api->sched, now_ms, NULL) : 0;
    if (!unix_now || unix_now < t->seen_unix)
        return false;
    *s = unix_now - t->seen_unix;
    return true;
}

/* "signal":{...}: the last POLLs as the AP heard them and what the tag said with them
   (#261), null for a tag not heard since this boot */
static void write_signal(esl_sb_t *out, const esl_tag_t *t)
{
    int8_t rssi[ESL_LINK_HIST];
    uint8_t info[ESL_LINK_HIST];
    unsigned n = t->seen ? esl_tag_signal(t, rssi, info) : 0;
    if (!n) {
        esl_sb_puts(out, ",\"signal\":null");
        return;
    }
    esl_sb_puts(out, ",\"signal\":{\"rssi\":[");
    for (unsigned i = 0; i < n; i++)
        esl_sb_printf(out, "%s%d", i ? "," : "", (int)rssi[i]);
    esl_sb_puts(out, "],\"lost\":[");
    for (unsigned i = 0; i < n; i++)
        esl_sb_printf(out, "%s%u", i ? "," : "", (unsigned)(info[i] & ESL_HIST_LOST));
    esl_sb_puts(out, "],\"retry\":[");
    for (unsigned i = 0; i < n; i++)
        esl_sb_printf(out, "%s%u", i ? "," : "", (info[i] & ESL_HIST_RETRY) ? 1u : 0u);
    esl_sb_printf(out, "],\"polls\":%" PRIu32 ",\"lost_total\":%u,\"retries\":%u,", t->polls,
                  (unsigned)t->lost_total, (unsigned)t->retries);
    if (t->rx_lqi || t->rx_rssi)
        esl_sb_printf(out, "\"tag_rssi\":%u,\"tag_lqi\":%u}", (unsigned)t->rx_rssi, (unsigned)t->rx_lqi);
    else
        esl_sb_puts(out, "\"tag_rssi\":null,\"tag_lqi\":null}");
}

/* The radios the AP has: links[] (radio_net.c) or the one link (radio_uart.c); NULL, 0: none */
static const esl_radio_link_t *radio_links(const esl_api_t *api, unsigned *n)
{
    if (api->links && api->n_links) {
        *n = api->n_links > 32 ? 32 : api->n_links;
        return api->links;
    }
    *n = api->link ? 1 : 0;
    return api->link;
}

/* Seconds since the radio lost contact (since boot when it never had any) */
static uint64_t radio_lost_s(const esl_radio_link_t *l, uint64_t now_ms)
{
    /* written by the radio's task without the API's lock: may be just ahead of now_ms */
    uint64_t down = l->down_ms;
    return (down < now_ms ? now_ms - down : 0) / 1000;
}

static bool radio_is_lost(const esl_radio_link_t *l, unsigned i, uint64_t now_ms)
{
    if (l->up || (i && !l->id))
        return false;
    return radio_lost_s(l, now_ms) >= ESL_RADIO_LOST_S;
}

uint32_t esl_api_radios_lost(const esl_api_t *api, uint64_t now_ms)
{
    unsigned n;
    const esl_radio_link_t *l = radio_links(api, &n);
    uint32_t lost = 0;
    for (unsigned i = 0; i < n; i++)
        if (radio_is_lost(&l[i], i, now_ms))
            lost |= 1u << i;
    return lost;
}

/* The fields of a radio's link after "connected": where it is and when it was last heard, or
   since when it is gone */
static void write_link_state(esl_sb_t *out, const esl_radio_link_t *l, unsigned i, uint64_t now_ms)
{
    esl_sb_printf(out, ",\"via\":\"%s\",\"lost\":%s", l->uart ? "uart" : "wifi",
                  radio_is_lost(l, i, now_ms) ? "true" : "false");
    if (!l->up) {
        esl_sb_printf(out, ",\"lost_s\":%" PRIu64, radio_lost_s(l, now_ms));
        return;
    }
    /* the radio's task writes seen_ms without the API's lock: a read just after
       a write can see it ahead of now_ms */
    uint64_t ago = l->seen_ms < now_ms ? now_ms - l->seen_ms : 0;
    if (l->ip)
        esl_sb_printf(out, ",\"ip\":\"%u.%u.%u.%u\"", (unsigned)(l->ip >> 24), (unsigned)((l->ip >> 16) & 0xff),
                      (unsigned)((l->ip >> 8) & 0xff), (unsigned)(l->ip & 0xff));
    esl_sb_printf(out, ",\"seen_s\":%" PRIu64 ",\"fw\":%u", ago / 1000, (unsigned)l->ver);
}

/* "[{radio, id, channel, tags, connected, ...}]": every network radio the AP knows, the one
   on its channel (0) and those with their own; channel is the AP's for radio 0 */
static void write_radios(const esl_api_t *api, esl_sb_t *out, uint8_t ap_channel, uint64_t now_ms)
{
    esl_sb_putc(out, '[');
    bool first = true;
    for (unsigned i = 0; i < api->n_links; i++) {
        const esl_radio_link_t *l = &api->links[i];
        if (!l->id)
            continue;
        char id[17];
        esl_mac_format(l->id, id);
        esl_sb_printf(out, "%s{\"radio\":%u,\"id\":\"%s\",\"channel\":%u,\"tags\":%u,\"connected\":%s",
                      first ? "" : ",", i, id, (unsigned)(l->channel ? l->channel : ap_channel), l->tags,
                      l->up ? "true" : "false");
        first = false;
        write_link_state(out, l, i, now_ms);
        esl_sb_putc(out, '}');
    }
    esl_sb_putc(out, ']');
}

/* GET /api/radios: the network radios; DELETE /api/radios/{id}: forget one that is not connected */
static int handle_radios(const esl_api_t *api, const char *method, const char *uri, size_t plen, uint64_t now_ms,
                         esl_sb_t *out)
{
    const size_t base = sizeof(PATH_RADIOS) - 1;
    size_t il = plen > base ? plen - base - 1 : 0;
    if (il && uri[base + il] == '/')
        il--;
    if (!il) {
        if (strcmp(method, "GET") != 0)
            return api_error(out, 405, "method not allowed");
        esl_channel_state_t ch;
        esl_store_channel_state(api->store, now_ms, &ch);
        esl_sb_puts(out, "{\"radios\":");
        write_radios(api, out, ch.channel, now_ms);
        esl_sb_putc(out, '}');
        return 200;
    }
    uint64_t id;
    if (il != 16 || !esl_mac_parse(&uri[base + 1], 16, &id) || !id)
        return api_error(out, 400, "the radio id must be 16 hex digits");
    if (strcmp(method, "DELETE") != 0 || !api->radio_ops)
        return api_error(out, 405, "method not allowed");
    int r = api->radio_ops->forget(api->radio_ops->ctx, id);
    if (r == 404)
        return api_error(out, 404, "unknown radio");
    if (r == 409)
        return api_error(out, 409, "the radio is connected: turn it off first, then forget it");
    char m[17];
    esl_mac_format(id, m);
    esl_sb_printf(out, "{\"forgotten\":\"%s\"}", m);
    return 200;
}

/* More than one network radio is known: each tag says which one serves it */
static bool several_radios(const esl_api_t *api)
{
    unsigned n = 0;
    for (unsigned i = 0; api->links && i < api->n_links; i++)
        n += api->links[i].id != 0;
    return n > 1;
}

/* fingerprint: for the change fingerprint (esl_api_changes_t), when the tag was last
   heard instead of how long ago, which changes every second */
static void write_tag(const esl_api_t *api, esl_sb_t *out, const esl_tag_t *t, uint64_t now_ms,
                      const esl_tag_cfg_t *ap_all, bool with_content, bool fingerprint)
{
    char m[17];
    esl_tag_cfg_t tap = esl_store_tag_ap(api->store, t, ap_all); /* its radio's channel */
    const esl_tag_cfg_t *ap = &tap;
    esl_mac_format(t->mac, m);
    esl_sb_printf(out, "{\"mac\":\"%s\",", m);
    /* A tag not heard since this boot, from NVS: what it said last, and when
     * (wall clock) if both that and the time now are known */
    bool known = t->seen || t->fw; /* fw 0: nothing stored from a POLL */
    uint64_t ago;
    if (fingerprint)
        esl_sb_printf(out, "%u,%" PRIu64 ",%" PRIu32 ",%d,", (unsigned)t->seen, t->last_seen_ms, t->seen_unix,
                      seen_s_ago(api, t, now_ms, &ago));
    else if (seen_s_ago(api, t, now_ms, &ago))
        esl_sb_printf(out, "\"seen_s_ago\":%" PRIu64 ",", ago);
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
        /* The tag reports the image id it shows: the content it was drawn from when
           that is its content's image, so clients compare content ids, else 0 (an
           image id is not a content id; which content it was is not kept) */
        uint32_t shown = t->has_content && t->image_id && t->shown_content_id == t->image_id ? t->content_id : 0;
        esl_sb_printf(out, "\"shown_content_id\":%" PRIu32 ",", shown);
    } else
        esl_sb_puts(out, "\"shown_content_id\":null,");
    esl_sb_printf(out, "\"pending\":%s,", esl_tag_pending(t) ? "true" : "false");
    if (several_radios(api))
        esl_sb_printf(out, "\"radio\":%u,", (unsigned)t->radio);
    write_tag_cfg(out, t, ap);

    esl_sb_puts(out, ",\"content_error\":");
    const char *err = esl_content_err_text(esl_tag_content_err(t));
    if (err)
        esl_sb_json_str(out, err);
    else
        esl_sb_puts(out, "null");
    /* the tag takes firmware over the radio (docs/tag-ota.md) */
    esl_sb_printf(out, ",\"ota\":%s", t->seen && (t->poll_flags & ESL_POLL_F_OTA) ? "true" : "false");
    write_signal(out, t);
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

    esl_content_t c;
    if (with_content && t->has_content && esl_store_get_content(api->store, t->mac, &c))
        write_content(out, &c);
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

static const char *store_err_text(esl_store_err_t err)
{
    return err == ESL_STORE_NOT_SAVED ? "no room to save it (NVS full)" : "tag table full";
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
        esl_store_err_t err;
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
                leave_source(api, mac);
                write_sched_result(out, mac, at);
                status = 200;
            }
        } else if ((leave_source(api, mac), err = esl_store_set(api->store, mac, &c, &r)) != ESL_STORE_OK)
            status = api_error(out, 507, store_err_text(err));
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
        esl_store_err_t err = ESL_STORE_FULL;
        if (k)
            esl_sb_putc(out, ',');
        if (ats[k] && esl_sched_add(api->sched, macs[k], ats[k], &contents[k]) == ESL_SCHED_OK) {
            leave_source(api, macs[k]);
            write_sched_result(out, macs[k], ats[k]);
        } else if (!ats[k] && (leave_source(api, macs[k]), err = esl_store_set(api->store, macs[k], &contents[k], &r)) == ESL_STORE_OK) {
            write_set_result(out, macs[k], &r);
        } else {
            char m[17];
            esl_mac_format(macs[k], m);
            esl_sb_printf(out, "{\"mac\":\"%s\",\"error\":\"%s\"}", m,
                          ats[k] ? "schedule full" : store_err_text(err));
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

/* ---- GET /api/tags?since= (esl_api_changes_t) ------------------------------------ */

void esl_api_changes_free(esl_api_changes_t *ch)
{
    for (unsigned c = 0; c < ESL_STORE_MAX_CHUNKS; c++) {
        free(ch->chunk[c]);
        ch->chunk[c] = NULL;
    }
}

/* A sink that only adds the bytes to a CRC: a tag's row is fingerprinted without room for it */
static bool crc_sink(esl_sb_t *sb)
{
    uint32_t *crc = sb->ctx;
    *crc = esl_crc32_update(*crc, sb->buf, sb->len);
    return true;
}

static uint32_t row_crc(const esl_api_t *api, const esl_tag_t *t, uint64_t now_ms, const esl_tag_cfg_t *ap)
{
    char buf[64];
    uint32_t crc = 0;
    esl_sb_t sb;
    esl_sb_init(&sb, buf, sizeof(buf));
    esl_sb_set_sink(&sb, crc_sink, &crc);
    esl_sb_stream(&sb, 200);
    write_tag(api, &sb, t, now_ms, ap, false, true);
    esl_sb_flush(&sb);
    return crc ? crc : 1; /* 0 is "no tag" */
}

/* Fingerprints every row and stamps those that changed with the next seq. False when
   there was no memory for the table: the caller sends the whole list. */
static bool changes_update(const esl_api_t *api, uint64_t now_ms, const esl_tag_cfg_t *ap)
{
    esl_api_changes_t *ch = api->changes;
    uint32_t next = ch->seq + 1;
    bool changed = false;
    for (unsigned s = 0, n = esl_store_slots(api->store); s < n; s++) {
        esl_api_change_t **chunk = &ch->chunk[s / ESL_STORE_CHUNK];
        if (!*chunk && !(*chunk = calloc(ESL_STORE_CHUNK, sizeof(esl_api_change_t))))
            return false;
        esl_api_change_t *e = &(*chunk)[s % ESL_STORE_CHUNK];
        esl_tag_t t;
        bool used = esl_store_get_slot(api->store, s, &t);
        uint32_t crc = used ? row_crc(api, &t, now_ms, ap) : 0;
        if (crc == e->crc)
            continue;
        /* gone, or deleted or evicted and its slot taken by another tag since the last read */
        if (e->crc && (!used || (uint32_t)t.mac != e->mac))
            ch->removed_seq = next;
        e->crc = crc;
        e->mac = used ? (uint32_t)t.mac : 0;
        e->seq = next;
        changed = true;
    }
    if (changed)
        ch->seq = next;
    return true;
}

/* ?since=<boot>-<seq> (8 hex digits, a number): true and the seq when it is from this
   boot, not ahead of now, and no tag has gone since */
static bool since_ok(const esl_api_changes_t *ch, const char *v, size_t len, uint32_t *seq)
{
    if (len < 10 || len > 19 || v[8] != '-')
        return false;
    uint32_t boot = 0, n = 0;
    for (size_t i = 0; i < 8; i++) {
        int c = tolower((unsigned char)v[i]);
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0)
            return false;
        boot = boot << 4 | (uint32_t)d;
    }
    for (size_t i = 9; i < len; i++) {
        if (!isdigit((unsigned char)v[i]) || n > (UINT32_MAX - 9) / 10)
            return false;
        n = n * 10 + (uint32_t)(v[i] - '0');
    }
    if (boot != ch->boot || n > ch->seq || n < ch->removed_seq)
        return false;
    *seq = n;
    return true;
}

static int get_all(const esl_api_t *api, const char *uri, uint64_t now_ms, esl_sb_t *out)
{
    /* The list can be far larger than the buffer (ESL_API_RESP_SIZE): a
       glue with a sink gets it in pieces. Nothing below can fail. */
    esl_sb_stream(out, 200);
    esl_sb_puts(out, "{\"ap\":{\"fw\":");
    esl_sb_json_str(out, api->fw_version ? api->fw_version : "");
    /* which board this is (ap/boards/<board>/board.json), so a dashboard can tell two APs apart */
    esl_sb_puts(out, ",\"board\":\"" ESL_BOARD "\",\"board_name\":\"" ESL_BOARD_NAME "\"");
    esl_tag_cfg_t ap;
    esl_store_tag_defaults(api->store, &ap);
    esl_channel_state_t ch;
    esl_store_channel_state(api->store, now_ms, &ch);
    esl_sb_printf(out, ",\"uptime_s\":%" PRIu64 ",\"channel\":%u,\"max_tags\":%u,\"table_full\":%s,\"poll_s\":%u",
                  now_ms / 1000, (unsigned)ch.channel, api->store->max,
                  esl_store_full(api->store) ? "true" : "false", (unsigned)ap.poll_s);
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
    if (api->link) {
        const esl_radio_link_t *l = api->link;
        esl_sb_printf(out, ",\"radio\":{\"connected\":%s", l->up ? "true" : "false");
        write_link_state(out, l, 0, now_ms);
        esl_sb_puts(out, "}");
    }
    if (api->links && api->n_links) {
        esl_sb_puts(out, ",\"radios\":");
        write_radios(api, out, ch.channel, now_ms);
    }
    if (api->radio_stats) {
        /* written by the radio task without the API's lock: a counter may be one behind */
        const esl_link_stats_t *r = api->radio_stats;
        esl_sb_printf(out,
                      ",\"replies\":{\"polls\":%" PRIu32 ",\"blocks\":%" PRIu32 ",\"tx_failed\":%" PRIu32
                      ",\"dropped\":%" PRIu32 ",\"slow\":%" PRIu32 ",\"worst_us\":%" PRId64 "}",
                      r->polls, r->blocks, r->tx_failed, r->dropped, r->slow, r->worst_us);
    }
    esl_sb_putc(out, '}');
    /* ?since=: only the tags that changed since that read ("full":false), or all of them
       when it cannot be told ("full":true); without since the reply is as before */
    size_t sl;
    const char *since = query_str(uri, "since", &sl);
    bool partial = false;
    uint32_t from = 0;
    if (since && api->changes && changes_update(api, now_ms, &ap)) {
        partial = since_ok(api->changes, since, sl, &from);
        esl_sb_printf(out, ",\"since\":\"%08" PRIx32 "-%" PRIu32 "\",\"full\":%s", api->changes->boot,
                      api->changes->seq, partial ? "false" : "true");
    }
    esl_sb_puts(out, ",\"tags\":[");
    bool first = true;
    for (unsigned s = 0, n = esl_store_slots(api->store); s < n; s++) {
        esl_tag_t t;
        /* a chunk the radio's task added a tag to after changes_update: sent */
        const esl_api_change_t *chunk = partial ? api->changes->chunk[s / ESL_STORE_CHUNK] : NULL;
        if (chunk && chunk[s % ESL_STORE_CHUNK].seq <= from)
            continue;
        if (!esl_store_get_slot(api->store, s, &t))
            continue;
        if (!first)
            esl_sb_putc(out, ',');
        first = false;
        write_tag(api, out, &t, now_ms, &ap, false, false);
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
    esl_tag_cfg_t ap;
    esl_mac_format(mac, m);
    if (strcmp(method, "GET") == 0) {
        esl_store_tag_defaults(api->store, &ap);
        if (!esl_store_get(api->store, mac, &t))
            return api_error(out, 404, "unknown tag");
        ap = esl_store_tag_ap(api->store, &t, &ap);
        esl_sb_printf(out, "{\"mac\":\"%s\",", m);
        write_tag_cfg(out, &t, &ap);
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
            esl_store_tag_defaults(api->store, &ap);
            ap = esl_store_tag_ap(api->store, &t, &ap);
            esl_sb_printf(out, "{\"mac\":\"%s\",", m);
            write_tag_cfg(out, &t, &ap);
            esl_sb_printf(out, ",\"changed\":%s}", r.changed ? "true" : "false");
            status = 200;
        }
    }
    free(tok);
    return status;
}

/* "tx_power": a whole number of dBm from ESL_TX_POWER_MIN to ESL_TX_POWER_MAX. */
static bool parse_dbm(const json_t *j, int v, int8_t *out)
{
    const esl_jtok_t *t = &j->tok[v];
    int i = t->start, n = 0;
    bool neg = i < t->end && j->js[i] == '-';
    if (t->type != ESL_JSON_NUMBER || t->end - i - neg < 1 || t->end - i - neg > 2)
        return false;
    for (i += neg; i < t->end; i++) {
        if (j->js[i] < '0' || j->js[i] > '9')
            return false;
        n = n * 10 + (j->js[i] - '0');
    }
    n = neg ? -n : n;
    if (n < ESL_TX_POWER_MIN || n > ESL_TX_POWER_MAX)
        return false;
    *out = (int8_t)n;
    return true;
}

bool esl_ntp_name_ok(const char *s, size_t len)
{
    if (len < 1 || len > ESL_NTP_MAX || s[0] == '-' || s[0] == '.')
        return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '.')
            return false;
    return true;
}

bool esl_tz_ok(const char *s, size_t len)
{
    if (len < 1 || len > ESL_TZ_MAX)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)s[i]) && !strchr("+-,./:<>", s[i]))
            return false;
    /* the POSIX form, a name and its offset: "Europe/Oslo" would be taken as UTC */
    size_t i = 0;
    if (s[0] == '<') {
        while (++i < len && s[i] != '>')
            if (!isalnum((unsigned char)s[i]) && s[i] != '+' && s[i] != '-')
                return false;
        if (i < 2 || i >= len)
            return false;
        i++;
    } else {
        while (i < len && isalpha((unsigned char)s[i]))
            i++;
        if (i < 3)
            return false;
    }
    if (i < len && (s[i] == '+' || s[i] == '-'))
        i++;
    return i < len && isdigit((unsigned char)s[i]);
}

/* A string or null at key k (index i) into buf (cap bytes): its length, 0 for null, -1 when it
   is neither or does not fit, -2 when check (may be NULL) turns a non-empty one down. */
static int opt_str(const char *body, const esl_jtok_t *tok, int i, char *buf, size_t cap,
                   bool (*check)(const char *s, size_t len))
{
    buf[0] = '\0';
    if (tok[i].type == ESL_JSON_NULL)
        return 0;
    int n = tok[i].type == ESL_JSON_STRING ? esl_json_str_decode(body, &tok[i], buf, cap - 1) : -1;
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n && check && !check(buf, (size_t)n) ? -2 : n;
}

/* PUT/GET /api/config: AP-wide default interval, the webhook, the radio's transmit
 * power and the clock. poll_s 0 = tags keep their built-in interval, null = back to
 * the AP's built-in default (menuconfig). webhook null or "" = off. tx_power in dBm,
 * null = back to the built-in value. ntp null or "" = the server set in menuconfig;
 * tz null or "" = UTC. Keys left out stay as they are; one of them must be there. */
/* "channel_now" skips the wait for a move the tags were told of; a channel they have not heard
   of would leave them on the old one (docs/protokoll.md, "Changing the channel"). The built-in
   channel is fine: every tag goes back to it when it loses the AP. */
static bool channel_now_ok(const esl_channel_state_t *cs, uint8_t channel)
{
    return channel == cs->builtin || channel == cs->channel || channel == cs->next;
}

/* A move away from the built-in channel needs tags and a radio that pass it on: 0 = fine, or
   the 409 with why ("force" moves past tags with old firmware, not past an old radio) */
static int channel_move_ok(const esl_channel_state_t *cs, uint8_t channel, bool force, esl_sb_t *out)
{
    if (channel != cs->channel && (cs->taken >> channel & 1))
        return api_error(out, 409, "another radio of this AP is on that channel: pick another one");
    if (channel == cs->builtin || channel == cs->channel)
        return 0;
    if (cs->radio_old)
        return api_error(out, 409, "the radio on WiFi runs firmware that cannot tell the tags of a new channel: "
                                   "update the radio first");
    if (cs->unheard && !force) {
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "%u tag%s %s not polled since the AP started, so the AP does not know if %s can follow a "
                 "channel change: wait until %s %s (at most %u min), or send \"force\": true",
                 cs->unheard, cs->unheard == 1 ? "" : "s", cs->unheard == 1 ? "has" : "have",
                 cs->unheard == 1 ? "it" : "they", cs->unheard == 1 ? "it" : "they",
                 cs->unheard == 1 ? "has" : "have", (unsigned)((cs->unheard_s + 59) / 60));
        return api_error(out, 409, msg);
    }
    if (cs->old_tags && !force) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "%u tag%s run%s firmware that cannot follow a channel change: update %s first, "
                 "or send \"force\": true to leave %s behind",
                 cs->old_tags, cs->old_tags == 1 ? "" : "s", cs->old_tags == 1 ? "s" : "",
                 cs->old_tags == 1 ? "it" : "them", cs->old_tags == 1 ? "it" : "them");
        return api_error(out, 409, msg);
    }
    return 0;
}

static int ap_config(const esl_api_t *api, const char *method, const char *body, size_t len, uint64_t now_ms,
                     esl_sb_t *out)
{
    bool put = strcmp(method, "PUT") == 0;
    if (put) {
        esl_jtok_t *tok = NULL;
        int status = parse_json(body, len, &tok, out);
        if (status == 0) {
            json_t j = {body, tok};
            int v = esl_json_obj_get(body, tok, 0, "poll_s");
            int w = esl_json_obj_get(body, tok, 0, "webhook");
            int x = esl_json_obj_get(body, tok, 0, "tx_power");
            int n = esl_json_obj_get(body, tok, 0, "ntp");
            int z = esl_json_obj_get(body, tok, 0, "tz");
            int c = esl_json_obj_get(body, tok, 0, "channel");
            int cn = esl_json_obj_get(body, tok, 0, "channel_now");
            int cf = esl_json_obj_get(body, tok, 0, "force");
            esl_channel_state_t cs;
            esl_store_channel_state(api->store, now_ms, &cs);
            uint16_t poll_s, channel = 0;
            int8_t dbm = 0;
            char url[ESL_WEBHOOK_URL_MAX + 1], ntp[ESL_NTP_MAX + 1], tz[ESL_TZ_MAX + 1];
            int ul = 0;
            if (v < 0 && w < 0 && x < 0 && n < 0 && z < 0 && c < 0)
                status = api_error(out, 422, "poll_s, webhook, tx_power, ntp, tz or channel is required (null resets it)");
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
            else if (x >= 0 && !api->tx_power)
                status = api_error(out, 422, "this AP cannot set its transmit power");
            else if (x >= 0 && tok[x].type != ESL_JSON_NULL && !parse_dbm(&j, x, &dbm))
                status = api_error(out, 422, "tx_power must be null or a whole number of dBm from -24 to 20");
            else if ((n >= 0 || z >= 0) && !api->clock)
                status = api_error(out, 422, "this AP has no clock settings");
            else if (n >= 0 && opt_str(body, tok, n, ntp, sizeof(ntp), esl_ntp_name_ok) < 0)
                status = api_error(out, 422, "ntp must be null or a server name of at most 63 letters, digits, '-' and '.'");
            else if (z >= 0 && opt_str(body, tok, z, tz, sizeof(tz), esl_tz_ok) < 0)
                status = api_error(out, 422, "tz must be null or a POSIX time zone of at most 63 characters, "
                                             "such as CET-1CEST,M3.5.0,M10.5.0/3");
            else if (c >= 0 && !api->channel_settable)
                status = api_error(out, 422, "this AP cannot change its radio channel");
            else if (c >= 0 && tok[c].type != ESL_JSON_NULL &&
                     parse_uint(&j, 0, "channel", ESL_CHANNEL_MIN, ESL_CHANNEL_MAX, &channel) < 0)
                status = api_error(out, 422, "channel must be null or a whole number from 11 to 26");
            else if (cn >= 0 && (c < 0 || (tok[cn].type != ESL_JSON_TRUE && tok[cn].type != ESL_JSON_FALSE)))
                status = api_error(out, 422, "channel_now must be true or false, with channel");
            else if (cf >= 0 && (c < 0 || (tok[cf].type != ESL_JSON_TRUE && tok[cf].type != ESL_JSON_FALSE)))
                status = api_error(out, 422, "force must be true or false, with channel");
            else if (cn >= 0 && tok[cn].type == ESL_JSON_TRUE &&
                     !channel_now_ok(&cs, channel ? (uint8_t)channel : cs.builtin))
                status = api_error(out, 409, "channel_now moves only to the channel the tags were told of: "
                                             "set the channel first, then move once they have it");
            else if (c >= 0 && (status = channel_move_ok(&cs, channel ? (uint8_t)channel : cs.builtin,
                                                         cf >= 0 && tok[cf].type == ESL_JSON_TRUE, out)))
                ;
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
                if (x >= 0) {
                    bool reset = tok[x].type == ESL_JSON_NULL;
                    esl_tx_power_t *tp = api->tx_power;
                    tp->dbm = reset ? tp->builtin : dbm;
                    if (tp->set)
                        tp->set(tp->ctx, tp->dbm, reset);
                }
                if (n >= 0 || z >= 0)
                    api->clock->set(api->clock->ctx, n >= 0 ? ntp : NULL, z >= 0 ? tz : NULL);
                if (c >= 0)
                    esl_store_move_channel(api->store, (uint8_t)channel, cn >= 0 && tok[cn].type == ESL_JSON_TRUE,
                                           cf >= 0 && tok[cf].type == ESL_JSON_TRUE, now_ms);
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
    if (api->tx_power)
        esl_sb_printf(out, ",\"tx_power\":%d,\"builtin_tx_power\":%d", api->tx_power->dbm, api->tx_power->builtin);
    /* the channel, and a move the tags are being told about: how many of the tags heard
       lately still have to confirm it, and the most it waits for them */
    if (api->channel_settable) {
        esl_channel_state_t ch;
        esl_store_channel_state(api->store, now_ms, &ch);
        esl_sb_printf(out, ",\"channel\":%u,\"builtin_channel\":%u", (unsigned)ch.channel, (unsigned)ch.builtin);
        if (ch.next)
            esl_sb_printf(out, ",\"channel_next\":%u,\"channel_waiting\":%u,\"channel_due_s\":%" PRIu32,
                          (unsigned)ch.next, ch.waiting, ch.due_s);
        else
            esl_sb_puts(out, ",\"channel_next\":null");
        /* what stops a move: tags heard lately with firmware from before 0.9.0, an old radio on WiFi */
        esl_sb_printf(out, ",\"channel_old_tags\":%u,\"channel_unheard_tags\":%u,\"channel_radio_old\":%s"
                      ",\"channel_stopped\":%u", ch.old_tags, ch.unheard, ch.radio_old ? "true" : "false", ch.stopped);
    }
    if (api->clock) {
        const esl_clock_cfg_t *c = api->clock;
        const char *keys[] = {"ntp", "builtin_ntp", "tz"};
        const char *vals[] = {c->ntp, c->builtin_ntp, c->tz};
        for (unsigned i = 0; i < 3; i++) {
            esl_sb_printf(out, ",\"%s\":", keys[i]);
            if (vals[i] && vals[i][0])
                esl_sb_json_str(out, vals[i]);
            else
                esl_sb_puts(out, "null");
        }
    }
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
    return esl_health_flags(t, esl_store_tag_poll_s(api->store, t->mac), now_ms, cfg);
}

int esl_api_health(const esl_api_t *api, uint64_t now_ms, const esl_health_cfg_t *cfg, uint8_t *prev,
                   esl_sb_t *out)
{
    unsigned counts[8] = {0}, tags = 0, problems = 0, changed = 0;
    esl_sb_stream(out, 200); /* one row per tag with a problem: can be larger than the buffer */
    esl_sb_puts(out, "{\"problems\":[");
    for (unsigned s = 0, n = esl_store_slots(api->store); s < n; s++) {
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
            esl_sb_printf(out, "],\"seen_s_ago\":%" PRIu64 ",\"battery_mv\":%u}", esl_tag_seen_ms_ago(&t, now_ms) / 1000,
                          (unsigned)t.battery_mv);
        else
            esl_sb_puts(out, "],\"seen_s_ago\":null,\"battery_mv\":null}");
        problems++;
    }
    /* the radio coprocessors that are gone: no tag hears from the AP through them */
    unsigned nl, lost = 0;
    const esl_radio_link_t *links = radio_links(api, &nl);
    esl_sb_puts(out, "],\"radio_problems\":[");
    for (unsigned i = 0; i < nl; i++) {
        const esl_radio_link_t *l = &links[i];
        if (!radio_is_lost(l, i, now_ms))
            continue;
        esl_sb_printf(out, "%s{\"radio\":%u,\"id\":", lost ? "," : "", i);
        if (l->id) {
            char id[17];
            esl_mac_format(l->id, id);
            esl_sb_printf(out, "\"%s\"", id);
        } else {
            esl_sb_puts(out, "null");
        }
        esl_sb_printf(out, ",\"via\":\"%s\",\"problems\":[\"radio_lost\"],\"lost_s\":%" PRIu64 "}",
                      l->uart ? "uart" : "wifi", radio_lost_s(l, now_ms));
        lost++;
    }
    esl_sb_printf(out, "],\"ok\":%s,\"tags\":%u,\"with_problems\":%u,\"radio_lost\":%u",
                  problems || lost ? "false" : "true", tags, problems, lost);
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

esl_tag_level_t esl_api_tag_level(const esl_api_t *api, const esl_tag_t *t, uint64_t now_ms)
{
    esl_health_cfg_t cfg = {0, 0}; /* the defaults, as the dashboard asks GET /api/health */
    if (tag_health(api, t, now_ms, &cfg))
        return ESL_TAG_BAD;
    esl_tag_cfg_t ap, eff;
    esl_store_tag_defaults(api->store, &ap);
    ap = esl_store_tag_ap(api->store, t, &ap);
    esl_tag_effective_cfg(t, &ap, &eff); /* "poll_s" in GET /api/tags */
    uint64_t poll_s = eff.poll_s ? eff.poll_s : ESL_TAG_BUILTIN_POLL_S;
    uint64_t ago; /* "seen_s_ago": late also for a tag from NVS not heard since this boot */
    if (seen_s_ago(api, t, now_ms, &ago) && ago > poll_s + ESL_TAG_CHECKIN_GRACE_S)
        return ESL_TAG_BUSY;
    esl_tagfw_img_t img;
    uint16_t got;
    if (t->seen && esl_tagfw_get(api->tagfw, t->model, &img) && tag_fw_update(api, t, &img, &got) == FWU_UPDATING)
        return ESL_TAG_BUSY;
    if (esl_tag_pending(t) || esl_tag_cfg_pending(t, &ap))
        return ESL_TAG_BUSY;
    return ESL_TAG_OK;
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
                      "\"fixedTextBytes\":%u},\"layouts\":[",
                      ESL_BOARD, (unsigned)esl_limits.runs, (unsigned)esl_limits.rects, (unsigned)esl_limits.lines,
                      (unsigned)esl_limits.icons, (unsigned)ESL_DRAW_MAX_BARCODES, (unsigned)ESL_DRAW_MAX_QR,
                      (unsigned)esl_limits.file_bytes, (unsigned)esl_limits.json_bytes, (unsigned)ESL_MAX_FIELDS,
                      (unsigned)ESL_LBIN_TEXT_LEN_MAX);
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

bool esl_api_tagfw_piece_uri(const char *uri, size_t body_len, size_t done, char *buf, size_t cap)
{
    const size_t fbase = sizeof(PATH_TAGFW) - 1;
    if (strcspn(uri, "?") != fbase || memcmp(uri, PATH_TAGFW, fbase) != 0)
        return false;
    size_t offset = 0, total = body_len, tl = 0;
    bool has_off = query_size(uri, "offset", &offset), has_total = query_size(uri, "total", &total);
    if (has_off != has_total)
        return false;
    const char *to = query_str(uri, "to", &tl);
    int n = snprintf(buf, cap, PATH_TAGFW "?offset=%lu&total=%lu%s%.*s", (unsigned long)(offset + done),
                     (unsigned long)total, to ? "&to=" : "", to ? (int)tl : 0, to ? to : "");
    return n > 0 && (size_t)n < cap;
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
    for (unsigned s = 0, slots = esl_store_slots(api->store); s < slots; s++) {
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
   tag that shows images gets (docs/protokoll.md, "Image transfer"), for the dashboard.
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
    const char *dv = query_str(uri, "dither", &dl);
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
    leave_source(api, mac); /* first: a source's run cannot overwrite the picture after this */
    esl_store_err_t serr = esl_store_set_image(api->store, mac, stored, n, &c, &r);
    free(img);
    if (serr)
        return api_error(out, 507, serr == ESL_STORE_NOT_SAVED ? "no room to save the picture: NVS is full, or nearly (some is kept for content)"
                                                               : store_err_text(serr));
    write_set_result(out, mac, &r);
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
    sb_reset(out);
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
    esl_channel_state_t ch;
    esl_store_channel_state(api->store, now_ms, &ch);
    esl_sb_printf(out, ", channel %u, %u of %u tags", (unsigned)ch.channel,
                  esl_store_count(api->store), api->store->max);
    if (esl_store_full(api->store))
        esl_sb_puts(out, " <span class=\"p\">(full: no room for a new tag)</span>");
    unsigned attention = 0;
    for (unsigned s = 0, n = esl_store_slots(api->store); s < n; s++) {
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
            if (esl_model_layout_model(m) != m)
                esl_sb_printf(out, "the file of %s", esl_model_name(esl_model_layout_model(m)));
            else if (esl_layouts_info(api->layouts, m, &v, NULL, &bin)) {
                esl_sb_printf(out, "version %" PRIu32 " (%u bytes compiled): ", v, (unsigned)bin);
                esl_layouts_write_names(api->layouts, m, out); /* plain ASCII names */
            } else {
                esl_sb_puts(out, "none (built-in price and text only)");
            }
        }
        esl_sb_puts(out, ".</p>");
    }
    esl_sb_puts(out, "<table><tr><th>MAC</th><th>Model</th><th>Seen</th><th>Battery</th><th>RSSI</th>"
                     "<th>FW</th><th>Content</th><th>State</th><th>Attention</th></tr>");

    for (unsigned s = 0, n = esl_store_slots(api->store); s < n; s++) {
        esl_tag_t t;
        char m[17];
        if (!esl_store_get_slot(api->store, s, &t))
            continue;
        esl_mac_format(t.mac, m);
        const char *model = t.seen ? esl_model_name(t.model) : NULL;
        esl_sb_printf(out, "<tr><td>%s</td><td>%s</td><td>", m, model ? model : "-");
        if (t.seen) {
            html_ago(out, esl_tag_seen_ms_ago(&t, now_ms) / 1000);
            if (t.battery_mv)
                esl_sb_printf(out, " ago</td><td>%u mV</td>", (unsigned)t.battery_mv);
            else
                esl_sb_puts(out, " ago</td><td>-</td>");
            esl_sb_printf(out, "<td>%d</td><td>%u</td><td>", (int)t.rssi, (unsigned)t.fw);
        } else {
            esl_sb_puts(out, "-</td><td>-</td><td>-</td><td>-</td><td>");
        }
        esl_content_t c;
        if (t.has_content && esl_store_get_content(api->store, t.mac, &c)) {
            esl_sb_puts(out, layout_name(&c));
            if (text_fields(&c))
                esl_sb_puts(out, ": ");
            for (unsigned i = 0; i < text_fields(&c); i++) {
                uint8_t len;
                const uint8_t *f = esl_content_field(&c, i, &len);
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
        const char *err = esl_content_err_text(esl_tag_content_err(&t));
        if (err)
            esl_sb_printf(out, " (%s)", err);
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
        int status = ap_config(api, method, body, body_len, now_ms, out);
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

    const size_t dbase = sizeof(PATH_SOURCES) - 1;
    if (api->sources && plen >= dbase && memcmp(uri, PATH_SOURCES, dbase) == 0 && (plen == dbase || uri[dbase] == '/')) {
        uint32_t now = api->sched ? esl_sched_now(api->sched, now_ms, NULL) : 0;
        if (strcmp(method, "PUT") == 0 && plen <= dbase + 1) { /* 400 and 413 as for the other endpoints */
            esl_jtok_t *tok = NULL;
            int status = parse_json(body, body_len, &tok, out);
            free(tok);
            if (status)
                return status;
        }
        int status = api->sources->api(api->sources, method, uri, plen, body, body_len, now, out);
        return out->overflow ? api_error(out, 500, "response too large") : status;
    }

    const size_t rbase = sizeof(PATH_RADIOS) - 1;
    if (api->links && api->n_links && plen >= rbase && memcmp(uri, PATH_RADIOS, rbase) == 0 &&
        (plen == rbase || uri[rbase] == '/')) {
        int status = handle_radios(api, method, uri, plen, now_ms, out);
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

    bool collection = false, item = false, item_cfg = false, item_image = false;
    uint64_t mac = 0;
    if (plen >= base && memcmp(uri, PATH_TAGS, base) == 0) {
        if (plen == base || (plen == base + 1 && uri[base] == '/'))
            collection = true;
        else if (uri[base] == '/') {
            static const char cfg_suffix[] = "/config", img_suffix[] = "/image";
            const size_t sl = sizeof(cfg_suffix) - 1, il = sizeof(img_suffix) - 1;
            size_t ml = plen - base - 1;
            if (ml == 17 && uri[base + 17] == '/')
                ml = 16;
            else if ((ml == 16 + sl || ml == 17 + sl) && memcmp(&uri[base + 17], cfg_suffix, sl) == 0 &&
                     (ml == 16 + sl || uri[base + 17 + sl] == '/')) {
                ml = 16;
                item_cfg = true;
            } else if ((ml == 16 + il || ml == 17 + il) && memcmp(&uri[base + 17], img_suffix, il) == 0 &&
                       (ml == 16 + il || uri[base + 17 + il] == '/')) {
                ml = 16;
                item_image = true;
            }
            if (!esl_mac_parse(&uri[base + 1], ml, &mac))
                return api_error(out, 400, "mac must be 16 hex digits");
            item = !item_cfg && !item_image;
        }
    }

    int status;
    if (item_cfg)
        status = tag_config(api, method, mac, body, body_len, out);
    else if (item_image && strcmp(method, "GET") == 0)
        status = tag_image(api, mac, out);
    else if (item_image && strcmp(method, "PUT") == 0) {
        const uint8_t *p = (const uint8_t *)body;
        status = put_image(api, mac, uri, body ? body_len : 0, mem_read, &p, NULL, out);
    } else if (item_image)
        return api_error(out, 405, "method not allowed");
    else if (collection && strcmp(method, "GET") == 0)
        status = get_all(api, uri, now_ms, out);
    else if (collection && strcmp(method, "PUT") == 0)
        status = put_batch(api, body, body_len, now_ms, out);
    else if (item && strcmp(method, "PUT") == 0)
        status = put_one(api, mac, body, body_len, now_ms, out);
    else if (item && strcmp(method, "GET") == 0) {
        esl_tag_t t;
        if (!esl_store_get(api->store, mac, &t))
            return api_error(out, 404, "unknown tag");
        esl_tag_cfg_t ap;
        esl_store_tag_defaults(api->store, &ap);
        write_tag(api, out, &t, now_ms, &ap, true, false);
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
