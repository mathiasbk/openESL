/* Helpers the API files share: error replies, JSON and query parsing. */
#include "esl_api_int.h"

void api_sb_reset(esl_sb_t *sb)
{
    sb->len = 0;
    sb->overflow = sb->cap == 0;
    sb->streaming = false;
    sb->status = 0;
    sb->sent = 0;
    if (sb->cap)
        sb->buf[0] = '\0';
}

int api_error_at(esl_sb_t *out, int status, const char *msg, int index)
{
    api_sb_reset(out);
    esl_sb_puts(out, "{\"error\":");
    esl_sb_json_str(out, msg);
    if (index >= 0)
        esl_sb_printf(out, ",\"index\":%d", index);
    esl_sb_putc(out, '}');
    return status;
}

int api_error(esl_sb_t *out, int status, const char *msg)
{
    return api_error_at(out, status, msg, -1);
}

/* A tag given content here leaves its data source (esl_source.h), or the source's next run
   would overwrite it */
void api_leave_source(const esl_api_t *api, uint64_t mac)
{
    if (api->sources && api->sources->release)
        api->sources->release(api->sources, mac);
}

int api_parse_bool(const json_t *j, int obj, const char *key, bool *out)
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

/* An optional whole number in [min, max]: missing or null gives 0. */
int api_parse_uint(const json_t *j, int obj, const char *key, unsigned min, unsigned max, uint16_t *out)
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

int api_parse_json(const char *body, size_t len, esl_jtok_t **tok, esl_sb_t *out)
{
    if (!body || len == 0)
        return api_error(out, 400, "missing JSON body");
    if (len > ESL_API_MAX_BODY)
        return api_error(out, 413, "body too large");
    *tok = malloc(sizeof(esl_jtok_t) * ESL_API_MAX_TOKENS);
    if (!*tok)
        return api_error(out, 500, "out of memory");
    int n = esl_json_parse(body, len, *tok, ESL_API_MAX_TOKENS);
    if (n == ESL_JSON_ERR_NOMEM) {
        char msg[96];
        snprintf(msg, sizeof(msg), "body too large: more than %d JSON values (strings, numbers, lists, objects)",
                 ESL_API_MAX_TOKENS);
        return api_error(out, 413, msg);
    }
    if (n == ESL_JSON_ERR_TOO_BIG)
        return api_error(out, 413, "body too large");
    if (n < 0)
        return api_error(out, 400, "invalid JSON");
    if ((*tok)[0].type != ESL_JSON_OBJECT)
        return api_error(out, 400, "body must be a JSON object");
    return 0;
}

/* ?key=value in the query string: the value and its length, or NULL if absent */
const char *api_query_str(const char *uri, const char *key, size_t *len)
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
bool api_query_to(const char *uri, uint64_t *mac)
{
    size_t len;
    const char *v = api_query_str(uri, "to", &len);
    *mac = 0;
    if (!v || (len == 3 && strncmp(v, "all", 3) == 0))
        return true;
    return esl_mac_parse(v, len, mac) && *mac;
}
