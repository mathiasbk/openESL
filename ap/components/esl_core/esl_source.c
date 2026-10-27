#include "esl_source.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esl_proto.h"
#include "esl_text.h"

/* Data sources: the configuration, /api/sources, when each source runs, and the content made
 * from its values (esl_source.h). What is particular to a type (its settings, URL and how its
 * answer becomes values) is in its own file: esl_source_met.c. */

#define PATH_BASE_LEN 12 /* "/api/sources" */

static void lock(esl_sources_t *s)
{
    if (s->ops.lock)
        s->ops.lock(s->ops.ctx);
}

static void unlock(esl_sources_t *s)
{
    if (s->ops.unlock)
        s->ops.unlock(s->ops.ctx);
}

long esl_source_round(double d)
{
    return (long)(d < 0 ? d - 0.5 : d + 0.5);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void esl_values_set(esl_values_t *v, const char *key, const char *val)
{
    unsigned i;
    for (i = 0; i < v->n && strcmp(v->key[i], key) != 0; i++)
        ;
    if (i == v->n) {
        if (v->n >= ESL_VALUES_MAX)
            return;
        v->n++;
        copy_str(v->key[i], sizeof(v->key[i]), key);
    }
    copy_str(v->val[i], sizeof(v->val[i]), val);
}

const char *esl_values_get(const esl_values_t *v, const char *key)
{
    for (unsigned i = 0; i < v->n; i++)
        if (strcmp(v->key[i], key) == 0)
            return v->val[i];
    return NULL;
}

/* ---- the types ---- */

/* Every type of source: one file each (esl_source_<name>.c), one line here */
static const esl_source_def_t *const types[] = {
    &esl_source_met,
};

static const esl_source_def_t *type_of(uint8_t type)
{
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (types[i]->type == type)
            return types[i];
    return NULL;
}

static bool key_known(const esl_source_def_t *t, const char *k, size_t n)
{
    for (const char *const *p = t->keys; *p; p++)
        if (strlen(*p) == n && memcmp(*p, k, n) == 0)
            return true;
    return false;
}

/* ---- configuration ---- */

typedef struct {
    const char *js;
    const esl_jtok_t *tok;
    char *err;
    size_t errlen;
    int si; /* the source the message is about, -1 for none */
} pc_t;

static bool fail(pc_t *c, const char *fmt, ...)
{
    char msg[ESL_SOURCE_ERR_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (c->si >= 0)
        snprintf(c->err, c->errlen, "sources[%d].%s", c->si, msg);
    else
        snprintf(c->err, c->errlen, "%s", msg);
    return false;
}

/* A string token into out (UTF-8, cap bytes with the NUL): false when it is not a string or too long */
static bool get_str(pc_t *c, int t, char *out, size_t cap)
{
    if (c->tok[t].type != ESL_JSON_STRING)
        return false;
    int n = esl_json_str_decode(c->js, &c->tok[t], out, cap - 1);
    if (n < 0)
        return false;
    out[n] = '\0';
    return true;
}

bool esl_source_num(const char *js, const esl_jtok_t *t, double *out)
{
    char buf[32];
    size_t n = (size_t)(t->end - t->start);
    if (t->type != ESL_JSON_NUMBER || n >= sizeof(buf))
        return false;
    memcpy(buf, js + t->start, n);
    buf[n] = '\0';
    char *end;
    *out = strtod(buf, &end);
    return end == buf + n;
}

static bool id_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > ESL_SOURCE_ID_MAX)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-'))
            return false;
    return true;
}

/* A template: text with {key} for the type's values */
static bool template_ok(pc_t *c, const esl_source_def_t *t, const char *s, unsigned fi)
{
    for (const char *p = s; *p; p++) {
        if (*p == '}')
            return fail(c, "fields[%u]: a } with no { before it", fi);
        if (*p != '{')
            continue;
        const char *e = strchr(p, '}');
        if (!e)
            return fail(c, "fields[%u]: a { with no } after it", fi);
        if (!key_known(t, p + 1, (size_t)(e - p - 1)))
            return fail(c, "fields[%u]: {%.*s} is not a value of type %s", fi, (int)(e - p - 1 > 16 ? 16 : e - p - 1),
                        p + 1, t->name);
        p = e;
    }
    return true;
}

/* A key of a source's configuration: one every type has, or one of its type's own */
static bool cfg_key(const char *js, const esl_jtok_t *k, const esl_source_def_t *ty)
{
    static const char *const keys[] = {"id", "type", "place", "lang", "every_min", "layout", "fields", "tags",
                                       "status", NULL};
    for (const char *const *p = keys; *p; p++)
        if (esl_json_str_eq(js, k, *p))
            return true;
    for (const char *const *p = ty->cfg_keys; p && *p; p++)
        if (esl_json_str_eq(js, k, *p))
            return true;
    return false;
}

static bool one(pc_t *c, int o, esl_source_cfg_t *src)
{
    const char *js = c->js;
    const esl_jtok_t *tok = c->tok;
    char buf[ESL_SOURCE_TPL_MAX + 1];

    memset(src, 0, sizeof(*src));
    if (tok[o].type != ESL_JSON_OBJECT)
        return fail(c, "must be an object"), false;

    int t = esl_json_obj_get(js, tok, o, "type");
    const esl_source_def_t *ty = NULL;
    for (size_t i = 0; t >= 0 && i < sizeof(types) / sizeof(types[0]); i++)
        if (esl_json_str_eq(js, &tok[t], types[i]->name))
            ty = types[i];
    if (!ty) {
        char names[64] = "";
        for (size_t i = 0, n = 0; i < sizeof(types) / sizeof(types[0]); i++)
            n += (size_t)snprintf(names + n, n < sizeof(names) ? sizeof(names) - n : 0, "%s\"%s\" (%s)",
                                  i ? ", " : "", types[i]->name, types[i]->about);
        return fail(c, "type: one of %s", names);
    }
    src->type = ty->type;

    for (int k = o + 1, i = 0; i < tok[o].size; i++, k = tok[k + 1].next)
        if (!cfg_key(js, &tok[k], ty))
            return fail(c, "%.*s: unknown key", (int)(tok[k].end - tok[k].start > 20 ? 20 : tok[k].end - tok[k].start),
                        js + tok[k].start);

    t = esl_json_obj_get(js, tok, o, "id");
    if (t < 0 || !get_str(c, t, src->id, sizeof(src->id)) || !id_ok(src->id))
        return fail(c, "id: 1 to %d of a-z 0-9 _ -", ESL_SOURCE_ID_MAX);

    char msg[ESL_SOURCE_ERR_MAX];
    if (ty->parse && !ty->parse(js, tok, o, src, msg, sizeof(msg)))
        return fail(c, "%s", msg);
    t = esl_json_obj_get(js, tok, o, "place");
    if (t >= 0 && !get_str(c, t, src->place, sizeof(src->place)))
        return fail(c, "place: a string of up to %d bytes", ESL_SOURCE_PLACE_MAX);
    copy_str(src->lang, sizeof(src->lang), "nb");
    t = esl_json_obj_get(js, tok, o, "lang");
    if (t >= 0 && (!get_str(c, t, src->lang, sizeof(src->lang)) || (strcmp(src->lang, "nb") && strcmp(src->lang, "en"))))
        return fail(c, "lang: \"nb\" or \"en\"");

    src->every_min = ty->every_min;
    t = esl_json_obj_get(js, tok, o, "every_min");
    if (t >= 0) {
        double d;
        if (!esl_source_num(js, &tok[t], &d) || d != (double)(long)d || d < ty->every_min || d > ESL_SOURCE_EVERY_MAX)
            return fail(c, "every_min: a whole number from %u to %d", (unsigned)ty->every_min, ESL_SOURCE_EVERY_MAX);
        src->every_min = (uint16_t)d;
    }

    t = esl_json_obj_get(js, tok, o, "layout");
    if (t < 0 || !get_str(c, t, src->layout, sizeof(src->layout)) ||
        (strcmp(src->layout, "text") && !esl_layout_name_valid(src->layout, strlen(src->layout))))
        return fail(c, "layout: the name of a layout in the layout files, or \"text\"");

    t = esl_json_obj_get(js, tok, o, "fields");
    if (t < 0) {
        for (const char *const *p = ty->fields; *p; p++)
            copy_str(src->fields[src->nfields++], sizeof(src->fields[0]), *p);
    } else {
        if (tok[t].type != ESL_JSON_ARRAY || tok[t].size < 1 || tok[t].size > ESL_MAX_FIELDS)
            return fail(c, "fields: a list of 1 to %d templates", ESL_MAX_FIELDS);
        for (int k = t + 1, i = 0; i < tok[t].size; i++, k = tok[k].next) {
            if (!get_str(c, k, buf, sizeof(buf)))
                return fail(c, "fields[%d]: a string of up to %d bytes", i, ESL_SOURCE_TPL_MAX);
            if (!template_ok(c, ty, buf, (unsigned)i))
                return false;
            copy_str(src->fields[src->nfields++], sizeof(src->fields[0]), buf);
        }
    }

    t = esl_json_obj_get(js, tok, o, "tags");
    if (t >= 0) {
        if (tok[t].type != ESL_JSON_ARRAY || tok[t].size > ESL_SOURCE_TAGS_MAX)
            return fail(c, "tags: a list of up to %d MACs", ESL_SOURCE_TAGS_MAX);
        for (int k = t + 1, i = 0; i < tok[t].size; i++, k = tok[k].next) {
            uint64_t mac;
            if (tok[k].type != ESL_JSON_STRING || !esl_mac_parse(js + tok[k].start, tok[k].end - tok[k].start, &mac))
                return fail(c, "tags[%d]: a MAC of 16 hex digits", i);
            for (unsigned j = 0; j < src->ntags; j++)
                if (src->tags[j] == mac)
                    return fail(c, "tags[%d]: the same tag twice", i);
            src->tags[src->ntags++] = mac;
        }
    }
    return true;
}

bool esl_sources_parse(const char *js, size_t len, esl_source_cfg_t *cfg, uint8_t *n, char *err, size_t errlen)
{
    pc_t c = {js, NULL, err, errlen, -1};
    bool ok = false;
    *n = 0;
    if (len == 0 || len > ESL_SOURCE_CFG_MAX)
        return fail(&c, "the configuration must be 1 to %d bytes", ESL_SOURCE_CFG_MAX);
    unsigned max_tok = (unsigned)(len / 2 + 2);
    esl_jtok_t *tok = malloc(sizeof(*tok) * max_tok);
    if (!tok)
        return fail(&c, "out of memory");
    c.tok = tok;
    if (esl_json_parse(js, len, tok, max_tok) < 0 || tok[0].type != ESL_JSON_OBJECT) {
        fail(&c, "must be a JSON object: {\"sources\": [...]}");
        goto done;
    }
    int a = esl_json_obj_get(js, tok, 0, "sources");
    if (a < 0 || tok[a].type != ESL_JSON_ARRAY || tok[a].size > ESL_SOURCE_MAX) {
        fail(&c, "sources: a list of up to %d sources", ESL_SOURCE_MAX);
        goto done;
    }
    for (int k = a + 1, i = 0; i < tok[a].size; i++, k = tok[k].next) {
        c.si = i;
        if (!one(&c, k, &cfg[i]))
            goto done;
        for (int j = 0; j < i; j++)
            if (strcmp(cfg[j].id, cfg[i].id) == 0) {
                fail(&c, "id: \"%s\" is used twice", cfg[i].id);
                goto done;
            }
        /* a tag in two sources would get the content of each in turn */
        for (int j = 0; j < i; j++)
            for (unsigned a2 = 0; a2 < cfg[i].ntags; a2++)
                for (unsigned b = 0; b < cfg[j].ntags; b++)
                    if (cfg[i].tags[a2] == cfg[j].tags[b]) {
                        char m[17];
                        esl_mac_format(cfg[i].tags[a2], m);
                        fail(&c, "tags[%u]: %.16s is already in source \"%s\"", a2, m, cfg[j].id);
                        goto done;
                    }
    }
    *n = (uint8_t)tok[a].size;
    ok = true;
done:
    free(tok);
    return ok;
}

void esl_source_write_cfg(const esl_source_cfg_t *c, esl_sb_t *out)
{
    const esl_source_def_t *t = type_of(c->type);
    esl_sb_puts(out, "{\"id\":");
    esl_sb_json_str(out, c->id);
    esl_sb_puts(out, ",\"type\":");
    esl_sb_json_str(out, t ? t->name : "");
    if (t && t->write)
        t->write(c, out);
    esl_sb_puts(out, ",\"place\":");
    esl_sb_json_str(out, c->place);
    esl_sb_puts(out, ",\"lang\":");
    esl_sb_json_str(out, c->lang);
    esl_sb_printf(out, ",\"every_min\":%u,\"layout\":", (unsigned)c->every_min);
    esl_sb_json_str(out, c->layout);
    esl_sb_puts(out, ",\"fields\":[");
    for (unsigned i = 0; i < c->nfields; i++) {
        if (i)
            esl_sb_putc(out, ',');
        esl_sb_json_str(out, c->fields[i]);
    }
    esl_sb_puts(out, "],\"tags\":[");
    for (unsigned i = 0; i < c->ntags; i++) {
        char m[17];
        esl_mac_format(c->tags[i], m);
        esl_sb_printf(out, "%s\"%.16s\"", i ? "," : "", m);
    }
    esl_sb_puts(out, "]}");
}

static void write_all_cfg(const esl_source_cfg_t *cfg, unsigned n, esl_sb_t *out)
{
    esl_sb_puts(out, "{\"sources\":[");
    for (unsigned i = 0; i < n; i++) {
        if (i)
            esl_sb_putc(out, ',');
        esl_source_write_cfg(&cfg[i], out);
    }
    esl_sb_puts(out, "]}");
}

void esl_sources_init(esl_sources_t *s, const esl_sources_ops_t *ops)
{
    memset(s, 0, sizeof(*s));
    if (ops)
        s->ops = *ops;
    s->api = esl_sources_api;
    s->release = esl_sources_release;
}

/* The same URL gives the same data, so the last answer still holds */
static bool same_data(const esl_source_cfg_t *a, const esl_source_cfg_t *b)
{
    char ua[ESL_SOURCE_URL_MAX], ub[ESL_SOURCE_URL_MAX];
    return a->type == b->type && esl_source_url(a, ua, sizeof(ua)) && esl_source_url(b, ub, sizeof(ub)) &&
           strcmp(ua, ub) == 0;
}

/* Takes cfg (n sources), keeping what is known of a source with the same id: its
 * state, and its last answer when the data is the same. Under the lock. */
static void take(esl_sources_t *s, const esl_source_cfg_t *cfg, uint8_t n)
{
    esl_source_state_t *st = n ? calloc(n, sizeof(*st)) : NULL;
    if (n && !st) { /* no memory: the sources start afresh */
        memset(s->state, 0, sizeof(s->state));
        memcpy(s->cfg, cfg, sizeof(*cfg) * n);
        s->n = n;
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        for (unsigned j = 0; j < s->n; j++)
            if (strcmp(s->cfg[j].id, cfg[i].id) == 0 && same_data(&s->cfg[j], &cfg[i])) {
                st[i] = s->state[j];
                st[i].reapply = true;
            }
    }
    if (n) {
        memcpy(s->cfg, cfg, sizeof(*cfg) * n);
        memcpy(s->state, st, sizeof(st[0]) * n);
    }
    free(st);
    s->n = n;
}

bool esl_sources_restore(esl_sources_t *s, const char *js, size_t len)
{
    char err[ESL_SOURCE_ERR_MAX];
    uint8_t n;
    esl_source_cfg_t *cfg = malloc(sizeof(*cfg) * ESL_SOURCE_MAX);
    bool ok = cfg && esl_sources_parse(js, len, cfg, &n, err, sizeof(err));
    if (ok) {
        lock(s);
        take(s, cfg, n);
        unlock(s);
    }
    free(cfg);
    return ok;
}

bool esl_sources_release(esl_sources_t *s, uint64_t mac)
{
    esl_source_cfg_t *cfg = malloc(sizeof(*cfg) * ESL_SOURCE_MAX);
    if (!cfg)
        return false;
    bool found = false;
    uint8_t n = 0;
    lock(s);
    for (unsigned i = 0; i < s->n && !found; i++) {
        esl_source_cfg_t *c = &s->cfg[i];
        for (unsigned k = 0; k < c->ntags && !found; k++)
            if (c->tags[k] == mac) {
                memmove(&c->tags[k], &c->tags[k + 1], sizeof(c->tags[0]) * (c->ntags - k - 1));
                c->ntags--;
                found = true;
            }
        if (found && !c->ntags) {
            memmove(&s->cfg[i], &s->cfg[i + 1], sizeof(s->cfg[0]) * (s->n - i - 1));
            memmove(&s->state[i], &s->state[i + 1], sizeof(s->state[0]) * (s->n - i - 1));
            s->n--;
        }
    }
    if (found) {
        n = s->n;
        memcpy(cfg, s->cfg, sizeof(*cfg) * n);
    }
    unlock(s);
    char *js = found && s->ops.persist ? malloc(ESL_SOURCE_CFG_MAX) : NULL;
    if (js) {
        esl_sb_t sb;
        esl_sb_init(&sb, js, ESL_SOURCE_CFG_MAX);
        write_all_cfg(cfg, n, &sb);
        if (!sb.overflow)
            s->ops.persist(s->ops.ctx, js, sb.len);
    }
    free(js);
    free(cfg);
    return found;
}

/* ---- the API ---- */

static int api_error(esl_sb_t *out, int status, const char *msg)
{
    out->len = 0;
    out->overflow = false;
    if (out->cap)
        out->buf[0] = '\0';
    esl_sb_puts(out, "{\"error\":");
    esl_sb_json_str(out, msg);
    esl_sb_putc(out, '}');
    return status;
}

static void write_opt_time(esl_sb_t *out, const char *key, uint32_t t)
{
    if (t)
        esl_sb_printf(out, ",\"%s\":%lu", key, (unsigned long)t);
    else
        esl_sb_printf(out, ",\"%s\":null", key);
}

static void write_status(const esl_source_state_t *st, esl_sb_t *out)
{
    esl_sb_puts(out, "{\"ok\":");
    esl_sb_puts(out, st->fetched_at && !st->error[0] ? "true" : "false");
    write_opt_time(out, "fetched_at", st->fetched_at);
    write_opt_time(out, "tried_at", st->tried_at);
    write_opt_time(out, "data_at", st->data_at);
    write_opt_time(out, "next_at", st->next_at);
    esl_sb_printf(out, ",\"http\":%d,\"tags_set\":%u,\"tags_changed\":%u,\"tags_failed\":%u,\"error\":", st->http,
                  (unsigned)st->tags_set, (unsigned)st->tags_changed, (unsigned)st->tags_failed);
    if (st->error[0])
        esl_sb_json_str(out, st->error);
    else
        esl_sb_puts(out, "null");
    esl_sb_puts(out, ",\"values\":{");
    for (unsigned i = 0; i < st->values.n; i++) {
        esl_sb_printf(out, "%s\"%s\":", i ? "," : "", st->values.key[i]);
        esl_sb_json_str(out, st->values.val[i]);
    }
    esl_sb_puts(out, "}}");
}

static void write_get(esl_sources_t *s, esl_sb_t *out)
{
    esl_sb_puts(out, "{\"types\":{");
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        esl_sb_printf(out, "%s\"%s\":{\"values\":[", i ? "," : "", types[i]->name);
        for (const char *const *p = types[i]->keys; *p; p++)
            esl_sb_printf(out, "%s\"%s\"", p == types[i]->keys ? "" : ",", *p);
        esl_sb_puts(out, "],\"fields\":[");
        for (const char *const *p = types[i]->fields; *p; p++) {
            if (p != types[i]->fields)
                esl_sb_putc(out, ',');
            esl_sb_json_str(out, *p);
        }
        esl_sb_puts(out, "]}");
    }
    esl_sb_puts(out, "},\"sources\":[");
    lock(s);
    for (unsigned i = 0; i < s->n; i++) {
        if (i)
            esl_sb_putc(out, ',');
        esl_source_write_cfg(&s->cfg[i], out);
        out->len--; /* the closing brace: the status goes in the object */
        esl_sb_puts(out, ",\"status\":");
        write_status(&s->state[i], out);
        esl_sb_putc(out, '}');
    }
    unlock(s);
    esl_sb_puts(out, "]}");
}

static int put(esl_sources_t *s, const char *body, size_t len, esl_sb_t *out)
{
    char err[ESL_SOURCE_ERR_MAX + 16];
    uint8_t n;
    esl_source_cfg_t *cfg = malloc(sizeof(*cfg) * ESL_SOURCE_MAX);
    char *js = malloc(ESL_SOURCE_CFG_MAX);
    int status = 200;
    if (!cfg || !js)
        status = api_error(out, 500, "out of memory");
    else if (!body || !esl_sources_parse(body, len, cfg, &n, err, sizeof(err)))
        status = api_error(out, 422, body ? err : "no body");
    else {
        esl_sb_t sb;
        esl_sb_init(&sb, js, ESL_SOURCE_CFG_MAX);
        write_all_cfg(cfg, n, &sb);
        if (sb.overflow)
            status = api_error(out, 422, "the configuration is too big to save");
        else if (s->ops.persist && !s->ops.persist(s->ops.ctx, js, sb.len))
            status = api_error(out, 507, "no room to save it (NVS full)");
        else {
            lock(s);
            take(s, cfg, n);
            unlock(s);
            if (s->ops.wake)
                s->ops.wake(s->ops.ctx);
        }
    }
    free(cfg);
    free(js);
    if (status == 200)
        write_get(s, out);
    return status;
}

static int refresh(esl_sources_t *s, uint32_t now, esl_sb_t *out)
{
    lock(s);
    for (unsigned i = 0; i < s->n; i++) {
        esl_source_state_t *st = &s->state[i];
        /* not before the answer's Expires (MET's terms): until then the last answer is used again */
        if (!st->fetched_at || st->expires_at <= now)
            st->next_at = 0;
        else
            st->reapply = true;
    }
    unsigned n = s->n;
    unlock(s);
    if (s->ops.wake)
        s->ops.wake(s->ops.ctx);
    esl_sb_printf(out, "{\"refreshing\":%u}", n);
    return 202;
}

int esl_sources_api(esl_sources_t *s, const char *method, const char *path, size_t plen, const char *body,
                    size_t len, uint32_t now, esl_sb_t *out)
{
    static const char rf[] = "/api/sources/refresh";
    bool base = plen == PATH_BASE_LEN || (plen == PATH_BASE_LEN + 1 && path[PATH_BASE_LEN] == '/');
    if (plen == sizeof(rf) - 1 && memcmp(path, rf, plen) == 0)
        return strcmp(method, "POST") == 0 ? refresh(s, now, out) : api_error(out, 405, "method not allowed");
    if (!base)
        return api_error(out, 404, "not found");
    if (strcmp(method, "GET") == 0) {
        write_get(s, out);
        return 200;
    }
    if (strcmp(method, "PUT") == 0)
        return put(s, body, len, out);
    return api_error(out, 405, "method not allowed");
}

/* ---- running them ---- */

int esl_sources_due(esl_sources_t *s, uint32_t now, esl_source_cfg_t *cfg, char *last_modified, size_t lm_cap,
                    bool *fetch)
{
    int found = -1;
    lock(s);
    for (unsigned i = 0; i < s->n && found < 0; i++) {
        esl_source_state_t *st = &s->state[i];
        if (st->next_at > now && !st->reapply)
            continue;
        *fetch = st->next_at <= now;
        if (*fetch)
            st->next_at = now + 60; /* until esl_sources_done says when */
        st->reapply = false;
        *cfg = s->cfg[i];
        copy_str(last_modified, lm_cap, st->last_modified);
        found = (int)i;
    }
    unlock(s);
    return found;
}

uint32_t esl_sources_wait(esl_sources_t *s, uint32_t now, uint32_t max_s)
{
    uint32_t w = max_s;
    lock(s);
    for (unsigned i = 0; i < s->n; i++) {
        const esl_source_state_t *st = &s->state[i];
        uint32_t d = st->reapply || st->next_at <= now ? 0 : st->next_at - now;
        if (d < w)
            w = d;
    }
    unlock(s);
    return w;
}

void esl_sources_done(esl_sources_t *s, int i, const char *id, uint32_t now, int status, uint32_t expires,
                      const char *last_modified, const esl_values_t *values, uint32_t data_at,
                      const esl_source_state_t *run, const char *error)
{
    lock(s);
    if (i >= 0 && i < s->n && strcmp(s->cfg[i].id, id) == 0) {
        esl_source_state_t *st = &s->state[i];
        uint32_t every = (uint32_t)s->cfg[i].every_min * 60;
        /* a fetch was tried: it got an answer (status), or none (no values to make content from) */
        if (status || !values) {
            st->tried_at = now;
            st->http = (int16_t)status;
        }
        /* 203: data, from a deprecated version of the API (the caller passes a warning) */
        if (status == 200 || status == 203 || status == 304) {
            st->fails = 0;
            st->fetched_at = now;
            st->expires_at = expires;
            if (status != 304)
                copy_str(st->last_modified, sizeof(st->last_modified), last_modified ? last_modified : "");
            st->next_at = now + every;
            if (expires > st->next_at)
                st->next_at = expires;
        } else if (status || !values) {
            /* An answer without data: again in 10 minutes, or at the usual time if sooner. No
               answer at all is mostly a WiFi or DNS hiccup: again in 1 minute, then 3, then 10 */
            uint32_t wait = 600;
            if (!status) {
                if (st->fails < 255)
                    st->fails++;
                wait = st->fails == 1 ? 60 : st->fails == 2 ? 180 : 600;
            }
            st->next_at = now + (every < wait ? every : wait);
        }
        if (values) {
            st->values = *values;
            st->data_at = data_at;
        }
        if (run) {
            st->tags_set = run->tags_set;
            st->tags_changed = run->tags_changed;
            st->tags_failed = run->tags_failed;
        }
        copy_str(st->error, sizeof(st->error), error ? error : run && run->error[0] ? run->error : "");
    }
    unlock(s);
}

size_t esl_source_url(const esl_source_cfg_t *c, char *buf, size_t cap)
{
    const esl_source_def_t *t = type_of(c->type);
    size_t n = t ? t->url(c, buf, cap) : 0;
    return n < cap ? n : 0;
}

/* ---- time ---- */

/* Days since 1970-01-01 of a date in the Gregorian calendar */
static int32_t days_from_civil(int32_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static bool digits(const char *s, unsigned n, unsigned *out)
{
    *out = 0;
    for (unsigned i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        *out = *out * 10 + (unsigned)(s[i] - '0');
    }
    return true;
}

static uint32_t unix_of(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s)
{
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60)
        return 0;
    return (uint32_t)days_from_civil((int32_t)y, mo, d) * 86400u + h * 3600u + mi * 60u + s;
}

uint32_t esl_iso_time(const char *s, size_t len)
{
    unsigned y, mo, d, h, mi, se;
    if (len < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[len - 1] != 'Z' ||
        !digits(s, 4, &y) || !digits(s + 5, 2, &mo) || !digits(s + 8, 2, &d) || !digits(s + 11, 2, &h) ||
        !digits(s + 14, 2, &mi) || !digits(s + 17, 2, &se))
        return 0;
    return unix_of(y, mo, d, h, mi, se);
}

uint32_t esl_http_date(const char *s)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    unsigned d, y, h, mi, se, mo;
    /* "Mon, 26 Oct 2026 16:30:00 GMT" */
    if (!s || strlen(s) < 29 || s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' ||
        s[19] != ':' || s[22] != ':' || memcmp(s + 25, " GMT", 4) != 0 || !digits(s + 5, 2, &d) ||
        !digits(s + 12, 4, &y) || !digits(s + 17, 2, &h) || !digits(s + 20, 2, &mi) || !digits(s + 23, 2, &se))
        return 0;
    for (mo = 0; mo < 12 && memcmp(months + 3 * mo, s + 8, 3) != 0; mo++)
        ;
    return mo < 12 ? unix_of(y, mo + 1, d, h, mi, se) : 0;
}

bool esl_source_values(const esl_source_cfg_t *c, const char *body, size_t len, uint32_t now, esl_utc_offset_fn off,
                       esl_values_t *v, uint32_t *data_at, char *err, size_t errlen)
{
    const esl_source_def_t *t = type_of(c->type);
    *data_at = 0;
    if (t)
        return t->values(c, body, len, now, off, v, data_at, err, errlen);
    snprintf(err, errlen, "unknown type");
    return false;
}

/* ---- content ---- */

bool esl_source_content(const esl_source_cfg_t *c, const esl_values_t *v, esl_content_t *out, char *err,
                        size_t errlen)
{
    if (!strcmp(c->layout, "text"))
        esl_content_init(out, ESL_LAYOUT_TEXT, 0);
    else
        esl_content_init_named(out, c->layout, strlen(c->layout), 0);
    for (unsigned i = 0; i < c->nfields; i++) {
        char utf8[ESL_MAX_TEXT * 2 + 1];
        size_t n = 0;
        bool over = false;
        for (const char *p = c->fields[i]; *p && !over; p++) {
            const char *val = NULL;
            size_t vl = 1;
            if (*p == '{') {
                const char *e = strchr(p, '}');
                char key[ESL_VALUE_KEY_MAX + 1];
                size_t kl = e ? (size_t)(e - p - 1) : 0;
                if (!e || kl > ESL_VALUE_KEY_MAX) {
                    snprintf(err, errlen, "fields[%u]: bad template", i);
                    return false;
                }
                memcpy(key, p + 1, kl);
                key[kl] = '\0';
                val = esl_values_get(v, key);
                vl = val ? strlen(val) : 0;
                p = e;
            } else {
                val = p;
            }
            if (n + vl >= sizeof(utf8))
                over = true;
            else if (vl) {
                memcpy(utf8 + n, val, vl);
                n += vl;
            }
        }
        uint8_t latin1[ESL_MAX_TEXT];
        size_t ln = over ? 0 : esl_utf8_to_latin1(utf8, n, latin1, sizeof(latin1), &over);
        if (over || esl_content_add_field(out, latin1, ln) != ESL_CONTENT_OK) {
            snprintf(err, errlen, "the fields are longer than a tag's %d bytes of text", ESL_CONTENT_FIELD_SPACE);
            return false;
        }
    }
    esl_content_trim(out);
    return true;
}

/* Whether tag mac is still in source id. Under the lock. */
static bool still_in(const esl_sources_t *s, const char *id, uint64_t mac)
{
    for (unsigned i = 0; i < s->n; i++)
        if (strcmp(s->cfg[i].id, id) == 0)
            for (unsigned k = 0; k < s->cfg[i].ntags; k++)
                if (s->cfg[i].tags[k] == mac)
                    return true;
    return false;
}

void esl_source_apply(const esl_source_cfg_t *c, const esl_values_t *v, esl_store_t *store, esl_layouts_t *ls,
                      esl_sources_t *s, esl_source_state_t *st)
{
    esl_content_t content;
    st->tags_set = st->tags_changed = st->tags_failed = 0;
    st->error[0] = '\0';
    if (!esl_source_content(c, v, &content, st->error, sizeof(st->error))) {
        st->tags_failed = c->ntags;
        return;
    }
    bool named = content.layout == ESL_LAYOUT_NAMED;
    for (unsigned i = 0; i < c->ntags; i++) {
        esl_tag_t t;
        char msg[ESL_SOURCE_ERR_MAX];
        uint8_t model = esl_store_get(store, c->tags[i], &t) && t.seen ? t.model : ESL_MODEL_UNKNOWN;
        msg[0] = '\0';
        if (named && ls && model != ESL_MODEL_UNKNOWN && esl_model_name(model)) {
            if (esl_layouts_find(ls, model, c->layout, strlen(c->layout), NULL) < 0)
                snprintf(msg, sizeof(msg), "layout \"%s\" is not in the layout file for model %s", c->layout,
                         esl_model_name(model));
            else
                esl_layouts_check_content(ls, model, &content, msg, sizeof(msg));
        }
        esl_set_result_t r;
        bool gone = false;
        if (!msg[0]) {
            /* the tag leaves its source before the API sets other content (esl_api.c), so
               the check and the set under the lock cannot overwrite that */
            if (s)
                lock(s);
            gone = s && !still_in(s, c->id, c->tags[i]);
            if (!gone && esl_store_set(store, c->tags[i], &content, &r) != ESL_STORE_OK)
                snprintf(msg, sizeof(msg), "no room for the content of a tag (tag table or NVS full)");
            if (s)
                unlock(s);
        }
        if (gone)
            continue;
        if (msg[0]) {
            st->tags_failed++;
            if (!st->error[0])
                copy_str(st->error, sizeof(st->error), msg);
            continue;
        }
        st->tags_set++;
        st->tags_changed += r.changed;
    }
}
