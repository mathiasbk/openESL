/* /api/time and /api/schedule: the AP's clock and scheduled content. */
#include "esl_api_int.h"

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
        int status = api_parse_json(body, len, &tok, out);
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
        const char *rep = esl_sched_repeat_name(esl_sched_slot_repeat(api->sched, s));
        if (rep)
            esl_sb_printf(out, ",\"repeat\":\"%s\"", rep);
        api_write_content(out, &c);
        esl_sb_putc(out, '}');
        first = false;
    }
    esl_sb_puts(out, "]}");
    return 200;
}

int api_route_time(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    return ap_time(api, rq->method, rq->body, rq->body_len, rq->now_ms, out);
}

int api_route_schedule(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    const char *uri = rq->uri;
    const size_t sbase = sizeof(PATH_SCHEDULE) - 1;
    size_t ml = rq->plen > sbase ? rq->plen - sbase - 1 : 0;
    if (ml == 17 && uri[sbase + 17] == '/')
        ml = 16;
    uint64_t m = 0;
    if (ml && !esl_mac_parse(&uri[sbase + 1], ml, &m))
        return api_error(out, 400, "mac must be 16 hex digits");
    return ap_schedule(api, rq->method, ml != 0, m, rq->now_ms, out);
}
