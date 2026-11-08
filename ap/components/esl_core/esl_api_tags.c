/* /api/tags: content, batch, the tag list (?since=), one tag and its settings. */
#include "esl_api_int.h"

/* A layout name from the layout files (requirements A13, A14 in docs/requirements.md): it must be in the file
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
    if (api_parse_uint(j, obj, "poll_s", ESL_CFG_POLL_MIN_S, ESL_CFG_POLL_MAX_S, &cfg->poll_s) < 0 ||
        api_parse_uint(j, obj, "max_poll_s", ESL_CFG_POLL_MIN_S, ESL_CFG_POLL_MAX_S, &cfg->max_poll_s) < 0) {
        *msg = "poll_s and max_poll_s must be null or a whole number of seconds from 10 to 3600";
        return 422;
    }
    if (api_parse_uint(j, obj, "lost_ap_s", ESL_CFG_LOST_AP_MIN_S, ESL_CFG_LOST_AP_NEVER, &cfg->lost_ap_s) < 0) {
        *msg = "lost_ap_s must be null or a whole number of seconds from 60 to 65535 (65535 = never)";
        return 422;
    }
    if (api_parse_uint(j, obj, "long_poll_s", ESL_CFG_LONG_POLL_MIN_S, 0xFFFF, &cfg->long_poll_s) < 0) {
        *msg = "long_poll_s must be null or a whole number of seconds from 60 to 65535";
        return 422;
    }
    if (api_parse_uint(j, obj, "long_after_min", ESL_CFG_LONG_AFTER_MIN_MIN, ESL_CFG_LONG_NEVER, &cfg->long_after_min) < 0) {
        *msg = "long_after_min must be null or a whole number of minutes from 10 to 65535 (65535 = never)";
        return 422;
    }
    return 0;
}

static int text_too_long(char *msg)
{
    snprintf(msg, MSG_MAX, "text too long: the fields share %d bytes, one per field plus the text after "
                           "Latin-1 conversion (%d bytes of text with one field, %d with six), at most %d in one field",
             ESL_CONTENT_FIELD_SPACE, ESL_MAX_TEXT_FOR(1), ESL_MAX_TEXT_FOR(6), ESL_MAX_TEXT);
    return 422;
}

/* The names of the built-in layouts' fields, for "fields" as an object */
static const char *const price_field_names[] = {"name", "price", "subtitle", NULL};
static const char *const text_field_names[] = {"line1", "line2", "line3", "line4", NULL};

/* The index of the field called key (klen bytes, UTF-8) in content c's layout, for the tag
 * mac: in the layout file for its model, or, while the model is not known, in every file that
 * has the layout (they must agree). Returns the index, or -1 with msg set. */
static int field_by_name(const esl_api_t *api, uint64_t mac, const esl_content_t *c, const char *key, size_t klen,
                         char *msg)
{
    if (c->layout != ESL_LAYOUT_NAMED) {
        const char *const *names = c->layout == ESL_LAYOUT_PRICE ? price_field_names : text_field_names;
        for (int i = 0; names[i]; i++)
            if (strlen(names[i]) == klen && memcmp(names[i], key, klen) == 0)
                return i;
        snprintf(msg, MSG_MAX, "layout %s has no field \"%.*s\" (it has %s)", c->layout == ESL_LAYOUT_PRICE ? "price" : "text",
                 (int)(klen > 32 ? 32 : klen), key,
                 c->layout == ESL_LAYOUT_PRICE ? "name, price, subtitle" : "line1, line2, line3, line4");
        return -1;
    }
    esl_tag_t t;
    uint8_t model = ESL_MODEL_UNKNOWN;
    size_t nl = strlen(c->name);
    if (esl_store_get(api->store, mac, &t) && t.seen && esl_model_name(t.model))
        model = t.model;
    int found = -2;
    for (uint8_t m = 1; m < ESL_MODEL_COUNT; m++) {
        if (model != ESL_MODEL_UNKNOWN && m != model)
            continue;
        int i = esl_layouts_field_index(api->layouts, m, c->name, nl, key, klen);
        if (i == -2)
            continue;
        if (i == -1) {
            snprintf(msg, MSG_MAX, "layout %s for model %s has no field \"%.*s\"", c->name, esl_model_name(m),
                     (int)(klen > 32 ? 32 : klen), key);
            return -1;
        }
        if (found >= 0 && i != found) {
            snprintf(msg, MSG_MAX, "the field \"%.*s\" is not in the same place in layout %s for every model, and this "
                     "tag's model is not known yet: send the fields as a list", (int)(klen > 32 ? 32 : klen), key, c->name);
            return -1;
        }
        found = i;
    }
    if (found < 0) {
        snprintf(msg, MSG_MAX, "layout %s has no field \"%.*s\"", c->name, (int)(klen > 32 ? 32 : klen), key);
        return -1;
    }
    return found;
}

/* The fields of content c from the "fields" token arr of j (-1 = none): a list (by position)
 * or an object keyed by field name, which index_of turns into the field's index (or -1 with
 * msg set). lname names the layout in messages; at most max_fields fields. The text is
 * converted to Latin-1 and added to c, empty fields at the end dropped. Returns 0 or an HTTP
 * status with msg (MSG_MAX bytes) set. */
int api_content_fields(const json_t *j, int arr, const char *lname, unsigned max_fields,
                       int (*index_of)(void *ctx, const char *key, size_t klen, char *msg), void *ctx,
                       esl_content_t *c, char *msg)
{
    /* The token of each field's text, in the layout's order; -1 for none (an empty field) */
    int vt[ESL_MAX_FIELDS];
    unsigned nf = 0;
    if (arr >= 0 && j->tok[arr].type == ESL_JSON_ARRAY) {
        if (j->tok[arr].size > max_fields) {
            snprintf(msg, MSG_MAX, "layout %s takes at most %u fields", lname, max_fields);
            return 422;
        }
        nf = j->tok[arr].size;
        for (unsigned k = 0, i = (unsigned)arr + 1; k < nf; k++, i = j->tok[i].next)
            vt[k] = (int)i;
    } else if (arr >= 0 && j->tok[arr].type == ESL_JSON_OBJECT) {
        for (unsigned k = 0; k < ESL_MAX_FIELDS; k++)
            vt[k] = -1;
        for (unsigned p = 0, i = (unsigned)arr + 1; p < j->tok[arr].size; p++, i = j->tok[i + 1].next) {
            char key[ESL_LAYOUT_FIELD_NAME_MAX + 1];
            int kl = esl_json_str_decode(j->js, &j->tok[i], key, sizeof(key));
            if (kl == -1) {
                snprintf(msg, MSG_MAX, "invalid string escape");
                return 400;
            }
            if (kl < 0)
                kl = (int)sizeof(key); /* longer than any field name: not found */
            int f = index_of(ctx, kl < (int)sizeof(key) ? key : &j->js[j->tok[i].start],
                             kl < (int)sizeof(key) ? (size_t)kl : (size_t)(j->tok[i].end - j->tok[i].start), msg);
            if (f < 0)
                return 422;
            if ((unsigned)f >= max_fields || f >= ESL_MAX_FIELDS) {
                snprintf(msg, MSG_MAX, "layout %s takes at most %u fields", lname, max_fields);
                return 422;
            }
            if (vt[f] >= 0) {
                snprintf(msg, MSG_MAX, "the field \"%s\" is given twice", key);
                return 422;
            }
            vt[f] = (int)i + 1;
            if ((unsigned)f + 1 > nf)
                nf = (unsigned)f + 1;
        }
    } else {
        snprintf(msg, MSG_MAX, "fields must be a list of strings, or an object of strings by field name");
        return 422;
    }

    /* the fields as Latin-1, back to back; empty fields at the end are dropped
       before they go into c, so they cost no length bytes. One field's UTF-8 (up to 4
       bytes a character) is on the heap: the HTTP task's stack is 8 KB */
    uint8_t text[ESL_MAX_TEXT];
    size_t len[ESL_MAX_FIELDS] = {0}, used = 0;
    const size_t umax = ESL_MAX_TEXT * 4 + 4;
    char *utf8 = nf ? malloc(umax) : NULL;
    if (nf && !utf8) {
        snprintf(msg, MSG_MAX, "out of memory");
        return 500;
    }
    int st = 0;
    for (unsigned k = 0; k < nf && !st; k++) {
        const esl_jtok_t *t = vt[k] >= 0 ? &j->tok[vt[k]] : NULL;
        size_t n = 0;
        bool overflow = false;

        if (t && t->type == ESL_JSON_STRING) {
            int ul = esl_json_str_decode(j->js, t, utf8, umax);
            if (ul == -1) {
                snprintf(msg, MSG_MAX, "invalid string escape");
                st = 400;
                break;
            }
            if (ul >= 0)
                n = esl_utf8_to_latin1(utf8, (size_t)ul, text + used, sizeof(text) - used, &overflow);
            else
                overflow = true;
        } else if (t && t->type != ESL_JSON_NULL) {
            snprintf(msg, MSG_MAX, "fields must be a list of strings, or an object of strings by field name");
            st = 422;
            break;
        }
        if (overflow || n > ESL_MAX_TEXT)
            st = text_too_long(msg);
        len[k] = n;
        used += n;
    }
    free(utf8);
    if (st)
        return st;
    while (nf > 0 && len[nf - 1] == 0)
        nf--;
    used = 0;
    for (unsigned k = 0; k < nf; used += len[k++])
        if (esl_content_add_field(c, text + used, len[k]) != ESL_CONTENT_OK)
            return text_too_long(msg);

    return 0;
}

typedef struct {
    const esl_api_t *api;
    uint64_t mac;
    const esl_content_t *c;
} field_ctx_t;

static int field_index(void *ctx, const char *key, size_t klen, char *msg)
{
    const field_ctx_t *fc = ctx;
    return field_by_name(fc->api, fc->mac, fc->c, key, klen, msg);
}

/* Fills c from {layout, fields, price_highlight, frame} for the tag mac. fields is a list
 * (by position) or an object keyed by the layout's field names (#182).
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
    if (api_parse_bool(j, obj, "price_highlight", &price_highlight) < 0 || api_parse_bool(j, obj, "frame", &frame) < 0) {
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

    const char *lname = layout == ESL_LAYOUT_NAMED ? c->name : layout == ESL_LAYOUT_PRICE ? "price" : "text";
    field_ctx_t fc = {api, mac, c};
    int arr = esl_json_obj_get(j->js, j->tok, obj, "fields");
    int st = api_content_fields(j, arr, lname, max_fields, field_index, &fc, c, msg);
    if (st)
        return st;

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

void api_write_set_result(esl_sb_t *out, uint64_t mac, const esl_set_result_t *r)
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
void api_write_content(esl_sb_t *out, const esl_content_t *c)
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

/* Where one tag is with the model's image: a tag it is not for, or that cannot or
   will not take it, is not updating. Bytes it has so far in *got. */
fwu_t api_tag_fw_update(const esl_api_t *api, const esl_tag_t *t, const esl_tagfw_img_t *img, uint16_t *got)
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
bool api_seen_s_ago(const esl_api_t *api, const esl_tag_t *t, uint64_t now_ms, uint64_t *s)
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

/* ,"layout":"<name>", from what the tag's record has in RAM: a name from the layout files
   by its hash, the content read back only when no file has it; null without content */
static void write_tag_layout(const esl_api_t *api, esl_sb_t *out, const esl_tag_t *t)
{
    char name[ESL_LAYOUT_NAME_MAX + 1];
    esl_content_t c;
    const char *l = NULL;
    if (t->has_content) {
        if (t->has_image)
            l = "image";
        else if (!t->layout_hash)
            l = t->layout_price ? "price" : "text";
        else if (esl_layouts_name(api->layouts, t->model, t->layout_hash, name))
            l = name;
        else if (esl_store_get_content(api->store, t->mac, &c) && esl_content_id(&c) == t->content_id)
            l = layout_name(&c);
    }
    esl_sb_puts(out, ",\"layout\":");
    if (l)
        esl_sb_json_str(out, l);
    else
        esl_sb_puts(out, "null");
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
                      api_seen_s_ago(api, t, now_ms, &ago));
    else if (api_seen_s_ago(api, t, now_ms, &ago))
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
    if (api_several_radios(api))
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
    /* the tag shows images the AP draws (protocol.md "Image transfer"): the id of its
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
    if (t->seen && esl_tagfw_get(api->tagfw, t->model, &img) && api_tag_fw_update(api, t, &img, &got) == FWU_UPDATING) {
        esl_sb_puts(out, ",\"fw_update\":{\"version\":");
        esl_sb_printf(out, "\"%u.%u.%u\"", img.ver / 10000u, img.ver / 100u % 100u, img.ver % 100u);
        esl_sb_printf(out, ",\"received\":%u,\"size\":%u}", (unsigned)got, (unsigned)img.size);
    } else {
        esl_sb_puts(out, ",\"fw_update\":null");
    }
    /* its content's layout (#302, #305), as GET /api/tags/{mac} has it; not fingerprinted:
       content_id changes with it; with_content: with the content's fields below */
    if (!fingerprint && !with_content)
        write_tag_layout(api, out, t);

    esl_content_t c;
    if (with_content && t->has_content && esl_store_get_content(api->store, t->mac, &c))
        api_write_content(out, &c);
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

const char *api_store_err_text(esl_store_err_t err)
{
    return err == ESL_STORE_NOT_SAVED ? "no room to save it (NVS full)" : "tag table full";
}

static int put_one(const esl_api_t *api, uint64_t mac, const char *body, size_t len, uint64_t now_ms,
                   esl_sb_t *out)
{
    esl_jtok_t *tok = NULL;
    int status = api_parse_json(body, len, &tok, out);
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
                api_leave_source(api, mac);
                write_sched_result(out, mac, at);
                status = 200;
            }
        } else if ((api_leave_source(api, mac), err = esl_store_set(api->store, mac, &c, &r)) != ESL_STORE_OK)
            status = api_error(out, 507, api_store_err_text(err));
        else {
            api_write_set_result(out, mac, &r);
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
    int status = api_parse_json(body, len, &tok, out);
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
            api_leave_source(api, macs[k]);
            write_sched_result(out, macs[k], ats[k]);
        } else if (!ats[k] && (api_leave_source(api, macs[k]), err = esl_store_set(api->store, macs[k], &contents[k], &r)) == ESL_STORE_OK) {
            api_write_set_result(out, macs[k], &r);
        } else {
            char m[17];
            esl_mac_format(macs[k], m);
            esl_sb_printf(out, "{\"mac\":\"%s\",\"error\":\"%s\"}", m,
                          ats[k] ? "schedule full" : api_store_err_text(err));
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
        api_write_link_state(out, l, 0, now_ms);
        esl_sb_puts(out, "}");
    }
    if (api->links && api->n_links) {
        esl_sb_puts(out, ",\"radios\":");
        api_write_radios(api, out, ch.channel, now_ms);
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
    const char *since = api_query_str(uri, "since", &sl);
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
    int status = api_parse_json(body, len, &tok, out);
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

/* /api/tags, /api/tags/{mac}, /api/tags/{mac}/config, /api/tags/{mac}/image and /api/tags/{mac}/draw */
int api_route_tags(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    const char *method = rq->method, *uri = rq->uri, *body = rq->body;
    size_t plen = rq->plen, body_len = rq->body_len;
    uint64_t now_ms = rq->now_ms;
    const size_t base = sizeof(PATH_TAGS) - 1;

    bool collection = false, item = false, item_cfg = false, item_image = false, item_draw = false;
    uint64_t mac = 0;
    {
        if (plen == base || (plen == base + 1 && uri[base] == '/'))
            collection = true;
        else if (uri[base] == '/') {
            static const char cfg_suffix[] = "/config", img_suffix[] = "/image", draw_suffix[] = "/draw";
            const size_t sl = sizeof(cfg_suffix) - 1, il = sizeof(img_suffix) - 1, dl = sizeof(draw_suffix) - 1;
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
            } else if ((ml == 16 + dl || ml == 17 + dl) && memcmp(&uri[base + 17], draw_suffix, dl) == 0 &&
                       (ml == 16 + dl || uri[base + 17 + dl] == '/')) {
                ml = 16;
                item_draw = true;
            }
            if (!esl_mac_parse(&uri[base + 1], ml, &mac))
                return api_error(out, 400, "mac must be 16 hex digits");
            item = !item_cfg && !item_image && !item_draw;
        }
    }

    int status;
    if (item_cfg)
        status = tag_config(api, method, mac, body, body_len, out);
    else if (item_image)
        status = api_tag_image(api, mac, rq, out);
    else if (item_draw)
        status = api_tag_draw(api, mac, rq, out);
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
    return status;

}
