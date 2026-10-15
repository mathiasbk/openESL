#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esl_json.h"
#include "esl_layout.h"
#include "esl_sched.h"
#include "esl_store.h"
#include "esl_wifi.h"
#include "esl_health.h"

/* The HTTP API from docs/protokoll.md part 2, independent of the HTTP server.
 * The ESP-IDF glue (main/http_api.c) reads the request and hands it here; the
 * host tests call it directly. */

#define ESL_API_MAX_BATCH  50
#define ESL_API_MAX_BODY   24576
#define ESL_API_MAX_TOKENS 1024

typedef struct {
    esl_store_t *store;
    const char *token;      /* bearer token; empty or NULL rejects every request */
    const char *fw_version; /* reported in GET /api/tags */
    uint8_t channel;
    esl_layouts_t *layouts; /* NULL: the /api/layouts endpoints answer 404 */
    const char *reset_reason; /* why the AP last restarted, as "reset" in GET /api/tags; NULL leaves it out */
    const esl_wifi_ops_t *wifi; /* NULL: /api/wifi answers 404 */
    esl_sched_t *sched;     /* NULL: no scheduled content; /api/time and /api/schedule answer 404 */
    esl_notify_t *notify;   /* the webhook (GET/PUT /api/config "webhook"); NULL: no webhook, shown as null */
    struct esl_api_lock *lock; /* lockout after wrong tokens (below); NULL: none, for a transport without a token */
} esl_api_t;

/* Against guessing the token: after ESL_API_AUTH_TRIES wrong tokens in a row the
 * API answers 429 to every request for ESL_API_AUTH_LOCK_S seconds, without
 * looking at the token; a right token resets the count. One per transport,
 * touched only by that transport's task. */
typedef struct esl_api_lock {
    uint8_t fails;
    uint64_t locked_until_ms;
} esl_api_lock_t;
#define ESL_API_AUTH_TRIES  5
#define ESL_API_AUTH_LOCK_S 60

/* Response buffer that always fits a full GET /api/tags, a batch reply and a
 * layout file. */
#define ESL_API_RESP_SIZE_(n) (512 + 448 * (n))
#define ESL_API_RESP_SIZE(max_tags) \
    ESL_API_RESP_SIZE_((max_tags) > ESL_API_MAX_BATCH ? (max_tags) : ESL_API_MAX_BATCH)
/* Buffer for a glue that streams (esl_sb_set_sink): every reply that is not
 * streamed (a batch reply, a layout file, errors) fits in it, and GET
 * /api/tags and the status page, which grow with the tag table, come through
 * the sink in pieces of this size. */
#define ESL_API_STREAM_BUF ESL_API_RESP_SIZE_(ESL_API_MAX_BATCH)

/* True if auth is "Bearer <token>" (case-insensitive scheme), in constant
 * time; false when the AP has no token. For transports that handle a request
 * themselves (main/ota.c). */
bool esl_api_auth_ok(const char *token, const char *auth);

/* esl_api_auth_ok() with api->lock: 0 when the request may go on, 401 for a
 * wrong token, 429 while locked (then *wait_s is the seconds left). */
int esl_api_auth_check(const esl_api_t *api, const char *auth, uint64_t now_ms, unsigned *wait_s);

/* Handles one request. method is "GET"/"PUT"/"DELETE", uri may contain a query
 * string, auth is the Authorization header (or NULL), now_ms is time since
 * boot. Writes a JSON body to out and returns the HTTP status code. */
int esl_api_handle(const esl_api_t *api, const char *method, const char *uri, const char *auth,
                   const char *body, size_t body_len, uint64_t now_ms, esl_sb_t *out);

/* GET /api/health as JSON: every tag with a problem (esl_health_flags) and
 * counts per problem. prev (cap bytes, may be NULL) holds the flags of the
 * last report: the tags whose flags differ are listed under "changed", and
 * every tag's new flags are written back to it. Returns the HTTP status;
 * streams like GET /api/tags. */
int esl_api_health(const esl_api_t *api, uint64_t now_ms, const esl_health_cfg_t *cfg, uint8_t *prev,
                   esl_sb_t *out);

/* The status page at / (krav A6): an HTML table with the same data as
 * GET /api/tags. Needs no token, since it only reads. Returns the HTTP status. */
int esl_api_status_page(const esl_api_t *api, uint64_t now_ms, esl_sb_t *out);
/* Buffer that always fits the status page. */
#define ESL_API_PAGE_SIZE(max_tags) (2816 + 1024 * (max_tags))

/* "200 OK", "401 Unauthorized", ... for esp_http_server. */
const char *esl_api_status_line(int status);
