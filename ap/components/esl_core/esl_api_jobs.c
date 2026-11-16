/* /api/jobs (#421): what became of the tags of each PUT /api/tags batch (esl_jobs.h). */
#include "esl_api_int.h"

static void write_job(const esl_job_t *j, uint64_t now_ms, bool with_tags, esl_sb_t *out)
{
    unsigned count[ESL_JOB_STATES] = {0};
    for (unsigned k = 0; k < j->n; k++)
        if (j->tags[k].state < ESL_JOB_STATES)
            count[j->tags[k].state]++;
    uint64_t age = now_ms > j->start_ms ? (now_ms - j->start_ms) / 1000 : 0;
    esl_sb_printf(out, "\"job\":%" PRIu32 ",\"state\":\"%s\",\"age_s\":%" PRIu64 ",\"took_s\":", j->id,
                  j->open ? "pending" : "done", age);
    if (j->open)
        esl_sb_puts(out, "null");
    else
        esl_sb_printf(out, "%" PRIu64, j->end_ms > j->start_ms ? (j->end_ms - j->start_ms) / 1000 : 0);
    esl_sb_printf(out, ",\"timeout_s\":%" PRIu32 ",\"notify\":%s,\"counts\":{", j->timeout_s,
                  j->notify ? "true" : "false");
    for (unsigned s = 0; s < ESL_JOB_STATES; s++)
        esl_sb_printf(out, "%s\"%s\":%u", s ? "," : "", esl_job_state_name((uint8_t)s), count[s]);
    esl_sb_putc(out, '}');
    if (!with_tags)
        return;
    esl_sb_puts(out, ",\"tags\":[");
    for (unsigned k = 0; k < j->n; k++) {
        const esl_job_tag_t *t = &j->tags[k];
        char m[17];
        esl_mac_format(t->mac, m);
        esl_sb_printf(out, "%s{\"mac\":\"%s\",\"state\":\"%s\",\"content_id\":", k ? "," : "", m,
                      esl_job_state_name(t->state));
        if (t->content_id)
            esl_sb_printf(out, "%" PRIu32, t->content_id);
        else
            esl_sb_puts(out, "null");
        if (t->state == ESL_JOB_FAILED) {
            esl_sb_puts(out, ",\"error\":");
            esl_sb_json_str(out, esl_job_why_text(t->why));
        }
        esl_sb_putc(out, '}');
    }
    esl_sb_putc(out, ']');
}

void esl_api_job_report(const esl_job_t *job, uint64_t now_ms, bool event, esl_sb_t *out)
{
    esl_sb_puts(out, event ? "{\"event\":\"job\"," : "{");
    write_job(job, now_ms, true, out);
    esl_sb_putc(out, '}');
}

/* GET /api/jobs: every kept job without its tags; GET /api/jobs/{n}: one with them */
int api_route_jobs(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    const size_t base = sizeof(PATH_JOBS) - 1;
    const char *uri = rq->uri;
    size_t plen = rq->plen;
    bool all = plen == base || (plen == base + 1 && uri[base] == '/');
    uint32_t id = 0;
    if (!all) {
        size_t end = plen > base + 1 && uri[plen - 1] == '/' ? plen - 1 : plen;
        uint64_t v = 0;
        size_t i = base + 1;
        for (; i < end && i - base <= 10 && isdigit((unsigned char)uri[i]); i++)
            v = v * 10 + (uint64_t)(uri[i] - '0');
        if (i != end || i == base + 1 || !v || v > UINT32_MAX)
            return api_error(out, 404, "not found");
        id = (uint32_t)v;
    }
    if (strcmp(rq->method, "GET") != 0)
        return api_error(out, 405, "method not allowed");

    esl_jobs_update(api->jobs, api->store, rq->now_ms);
    static esl_job_tag_t tags[ESL_JOB_TAGS_MAX]; /* under the API's mutex */
    esl_job_t j;
    if (!all) {
        if (!esl_jobs_get(api->jobs, id, &j, tags))
            return api_error(out, 404, "unknown job: the AP keeps only its last jobs, and not over a restart");
        esl_api_job_report(&j, rq->now_ms, false, out);
        return 200;
    }
    uint32_t ids[ESL_JOBS_MAX];
    unsigned n = esl_jobs_list(api->jobs, ids);
    esl_sb_puts(out, "{\"jobs\":[");
    bool first = true;
    for (unsigned i = 0; i < n; i++) {
        if (!esl_jobs_get(api->jobs, ids[i], &j, tags))
            continue;
        esl_sb_puts(out, first ? "{" : ",{");
        write_job(&j, rq->now_ms, false, out);
        esl_sb_putc(out, '}');
        first = false;
    }
    esl_sb_puts(out, "]}");
    return 200;
}
