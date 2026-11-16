/* /api/remote: a SoluM remote as a wireless button (esl_remote.h, docs/solum-remote.md). */
#include "esl_api_int.h"
#include "esl_remote.h"

static void lock(esl_remote_t *r, bool take)
{
    if (r->lock)
        r->lock(r, take);
}

static void write_remote(const esl_api_t *api, uint64_t now_ms, esl_sb_t *out)
{
    esl_remote_t *r = api->remote;
    uint8_t ch = api->store->channel;
    lock(r, true);
    esl_sb_printf(out, "{\"channel\":%u,\"listen\":%s,\"can_listen\":%s,\"hearing\":%s,\"webhook\":", ESL_REMOTE_CHANNEL,
                  r->listen ? "true" : "false", r->can_listen ? "true" : "false",
                  ch == ESL_REMOTE_CHANNEL || (r->listen && r->can_listen) ? "true" : "false");
    if (r->webhook[0])
        esl_sb_json_str(out, r->webhook);
    else
        esl_sb_puts(out, "null");
    if (r->webhook_status)
        esl_sb_printf(out, ",\"webhook_status\":%d", r->webhook_status);
    else
        esl_sb_puts(out, ",\"webhook_status\":null");
    esl_sb_printf(out, ",\"boot\":%" PRIu32 ",\"frames\":%" PRIu32 ",\"presses\":[", r->boot, r->frames);
    for (unsigned i = r->n; i-- > 0;) { /* newest first */
        const esl_remote_press_t *p = &r->log[i];
        esl_sb_printf(out, "%s{\"id\":%" PRIu32 ",\"button\":\"%s\",\"s_ago\":%" PRIu64 ",\"rssi\":%d}",
                      i + 1 == r->n ? "" : ",", p->id, esl_remote_button_name(p->button),
                      (now_ms > p->at_ms ? now_ms - p->at_ms : 0) / 1000, p->rssi);
    }
    lock(r, false);
    esl_sb_puts(out, "]}");
}

/* GET/PUT /api/remote */
int api_route_remote(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    esl_remote_t *r = api->remote;
    if (strcmp(rq->method, "PUT") == 0) {
        esl_jtok_t *tok = NULL;
        int status = api_parse_json(rq->body, rq->body_len, &tok, out);
        if (status) {
            free(tok);
            return status;
        }
        const char *body = rq->body;
        json_t j = {body, tok};
        int l = esl_json_obj_get(body, tok, 0, "listen");
        int w = esl_json_obj_get(body, tok, 0, "webhook");
        bool listen = r->listen;
        char url[ESL_WEBHOOK_URL_MAX + 1];
        int ul = 0;
        if (l < 0 && w < 0)
            status = api_error(out, 422, "listen or webhook is required");
        else if (l >= 0 && (tok[l].type == ESL_JSON_NULL || api_parse_bool(&j, 0, "listen", &listen) < 0))
            status = api_error(out, 422, "listen must be true or false");
        else if (listen && !r->listen && !r->can_listen)
            status = api_error(out, 422, "this AP's radio cannot visit channel 11: move the AP to channel 11 instead");
        else if (w >= 0 && tok[w].type != ESL_JSON_NULL &&
                 (tok[w].type != ESL_JSON_STRING || (ul = esl_json_str_decode(body, &tok[w], url, sizeof(url) - 1)) < 0 ||
                  (ul && !esl_webhook_url_ok(url, (size_t)ul))))
            status = api_error(out, 422, "webhook must be null or an http://host[:port][/path] URL of at most 127 characters");
        free(tok);
        if (status)
            return status;
        if (w >= 0)
            url[ul] = '\0';
        if (r->set)
            r->set(r, listen, w >= 0 ? url : NULL);
        else {
            lock(r, true);
            r->listen = listen;
            if (w >= 0) {
                strcpy(r->webhook, url);
                r->webhook_status = 0;
            }
            lock(r, false);
        }
    } else if (strcmp(rq->method, "GET") != 0)
        return api_error(out, 405, "method not allowed");
    write_remote(api, rq->now_ms, out);
    return 200;
}
