/* The API's entry point: the route table and the status lines. */
#include "esl_api_int.h"

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

/* What a route needs the AP to have; a route whose part is missing answers 404 */
enum { NEEDS_NONE, NEEDS_LAYOUTS, NEEDS_TAGFW, NEEDS_WIFI, NEEDS_SOURCES, NEEDS_RADIOS, NEEDS_SCHED, NEEDS_REMOTE };

static bool has(const esl_api_t *api, int needs)
{
    switch (needs) {
    case NEEDS_LAYOUTS: return api->layouts != NULL;
    case NEEDS_TAGFW:   return api->tagfw != NULL;
    case NEEDS_WIFI:    return api->wifi != NULL;
    case NEEDS_SOURCES: return api->sources != NULL;
    case NEEDS_RADIOS:  return api->links && api->n_links;
    case NEEDS_SCHED:   return api->sched != NULL;
    case NEEDS_REMOTE:  return api->remote != NULL;
    default:            return true;
    }
}

/* OTA is handled by the transport (main/ota.c) before the request gets
   here, since the body is a firmware image; this answers where it is not */
static int route_ota(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    (void)api;
    if (strcmp(rq->method, "GET") == 0 || strcmp(rq->method, "POST") == 0)
        return api_error(out, 501, "no OTA on this transport or build");
    return api_error(out, 405, "method not allowed");
}

/* The data sources (esl_source.h) answer for themselves */
static int route_sources(const esl_api_t *api, const esl_api_req_t *rq, esl_sb_t *out)
{
    const size_t dbase = sizeof(PATH_SOURCES) - 1;
    uint32_t now = api->sched ? esl_sched_now(api->sched, rq->now_ms, NULL) : 0;
    if (strcmp(rq->method, "PUT") == 0 && rq->plen <= dbase + 1) { /* 400 and 413 as for the other endpoints */
        esl_jtok_t *tok = NULL;
        int status = api_parse_json(rq->body, rq->body_len, &tok, out);
        free(tok);
        if (status)
            return status;
    }
    return api->sources->api(api->sources, rq->method, rq->uri, rq->plen, rq->body, rq->body_len, now, out);
}

#define ROUTE(path, sub, needs, fn) {path, sizeof(path) - 1, sub, needs, fn}

/* The endpoints. A route matches its path, or its path and a slash; with sub, anything
   under it as well. To add an endpoint: a line here, its handler in the file for its
   resource (esl_api_int.h), and the path in docs/openapi.yaml. */
static const struct {
    const char *path;
    size_t len;
    bool sub;
    int needs;
    esl_api_route_fn *fn;
} routes[] = {
    ROUTE(PATH_LAYOUTS, true, NEEDS_LAYOUTS, api_route_layouts),
    ROUTE(PATH_FONTS, true, NEEDS_LAYOUTS, api_route_fonts),
    ROUTE(PATH_TAGFW, true, NEEDS_TAGFW, api_route_tagfw),
    ROUTE(PATH_CONFIG, false, NEEDS_NONE, api_route_config),
    ROUTE(PATH_OTA, false, NEEDS_NONE, route_ota),
    ROUTE(PATH_HEALTH, false, NEEDS_NONE, api_route_health),
    ROUTE(PATH_WIFI, false, NEEDS_WIFI, api_route_wifi),
    ROUTE(PATH_SOURCES, true, NEEDS_SOURCES, route_sources),
    ROUTE(PATH_RADIOS, true, NEEDS_RADIOS, api_route_radios),
    ROUTE(PATH_TIME, false, NEEDS_SCHED, api_route_time),
    ROUTE(PATH_SCHEDULE, true, NEEDS_SCHED, api_route_schedule),
    ROUTE(PATH_REMOTE, false, NEEDS_REMOTE, api_route_remote),
    ROUTE(PATH_TAGS, true, NEEDS_NONE, api_route_tags),
};

static bool route_match(const char *uri, size_t plen, const char *path, size_t len, bool sub)
{
    if (plen < len || memcmp(uri, path, len) != 0)
        return false;
    if (plen == len)
        return true;
    return uri[len] == '/' && (sub || plen == len + 1);
}

int esl_api_handle(const esl_api_t *api, const char *method, const char *uri, const char *auth,
                   const char *body, size_t body_len, uint64_t now_ms, esl_sb_t *out)
{
    api_sb_reset(out);
    size_t plen = strcspn(uri, "?");

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

    const esl_api_req_t rq = {method, uri, plen, body, body_len, now_ms};
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (!route_match(uri, plen, routes[i].path, routes[i].len, routes[i].sub) || !has(api, routes[i].needs))
            continue;
        int status = routes[i].fn(api, &rq, out);
        return out->overflow ? api_error(out, 500, "response too large") : status;
    }
    return api_error(out, 404, "not found");
}
